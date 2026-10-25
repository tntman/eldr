#include "mesh.h"

#include "instancer.h"
#include "material.h"
#include "renderDelegate.h"
#include "scene.h"
#include "subdivision.h"

#include "pxr/base/gf/matrix4f.h"
#include "pxr/base/gf/vec2f.h"
#include "pxr/imaging/hd/meshUtil.h"
#include "pxr/imaging/hd/renderIndex.h"
#include "pxr/imaging/hd/sceneDelegate.h"
#include "pxr/imaging/hd/smoothNormals.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/imaging/hd/vertexAdjacency.h"
#include "pxr/imaging/pxOsd/tokens.h"

#include <algorithm>
#include <array>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

TF_DEFINE_PRIVATE_TOKENS(
    _tokens,
    (st)
    ((eldrMatte, "eldr:matte"))
    ((karmaHoldout, "karma:object:holdoutmode"))
    ((planarReflection, "eldr:planarReflection"))
    ((reflectionProbe, "eldr:reflectionProbe")));

// Finds a primvar's interpolation; false if the prim doesn't have it. An
// empty name finds the first texture coordinate (float2) primvar.
bool FindPrimvar(HdSceneDelegate *sd, SdfPath const &id, TfToken const &name,
                 HdInterpolation *interp, TfToken *found = nullptr)
{
    for (int i = 0; i < HdInterpolationCount; ++i) {
        for (const HdPrimvarDescriptor &d :
             sd->GetPrimvarDescriptors(id, HdInterpolation(i))) {
            if (d.name == name || (name.IsEmpty() && d.role == HdPrimvarRoleTokens->textureCoordinate)) {
                *interp = HdInterpolation(i);
                if (found) {
                    *found = d.name;
                }
                return true;
            }
        }
    }
    return false;
}

} // namespace

bool HdVkHoldout(HdSceneDelegate *sceneDelegate, SdfPath const &id)
{
    for (const TfToken &name : {_tokens->eldrMatte, _tokens->karmaHoldout}) {
        HdInterpolation interp;
        if (!FindPrimvar(sceneDelegate, id, name, &interp)) {
            continue;
        }
        const VtValue value = sceneDelegate->Get(id, name);
        const VtValue scalar = value.IsArrayValued() && value.GetArraySize() > 0
            ? VtValue::Cast<VtIntArray>(value) : VtValue::Cast<int>(value);
        if (scalar.IsHolding<int>() ? scalar.UncheckedGet<int>() != 0
                                    : scalar.IsHolding<VtIntArray>() && scalar.UncheckedGet<VtIntArray>()[0] != 0) {
            return true;
        }
    }
    return false;
}

namespace {

// Expands a primvar of any interpolation to one value per triangle corner.
// Returns false (and leaves `out` empty) if the data doesn't fit the topology.
template <typename T>
bool FlattenPrimvar(VtValue const &value, HdInterpolation interp, HdMeshUtil const &util,
                    VtVec3iArray const &triangles, VtIntArray const &primitiveParams,
                    HdType type, std::vector<float> *out)
{
    out->clear();
    if (!value.IsHolding<VtArray<T>>()) {
        return false;
    }
    const VtArray<T> &src = value.UncheckedGet<VtArray<T>>();
    const size_t corners = triangles.size() * 3;
    constexpr size_t dim = T::dimension;
    std::vector<float> result(corners * dim);
    auto put = [&](size_t corner, const T &v) {
        std::copy(v.data(), v.data() + dim, result.data() + corner * dim);
    };

    switch (interp) {
    case HdInterpolationConstant:
        if (src.empty()) {
            return false;
        }
        for (size_t c = 0; c < corners; ++c) {
            put(c, src[0]);
        }
        break;
    case HdInterpolationUniform:
        for (size_t t = 0; t < triangles.size(); ++t) {
            const size_t face = size_t(
                HdMeshUtil::DecodeFaceIndexFromCoarseFaceParam(primitiveParams[t]));
            if (face >= src.size()) {
                return false;
            }
            put(t * 3, src[face]);
            put(t * 3 + 1, src[face]);
            put(t * 3 + 2, src[face]);
        }
        break;
    case HdInterpolationVertex:
    case HdInterpolationVarying:
        for (size_t t = 0; t < triangles.size(); ++t) {
            for (int k = 0; k < 3; ++k) {
                const int i = triangles[t][k];
                if (i < 0 || size_t(i) >= src.size()) {
                    return false;
                }
                put(t * 3 + k, src[i]);
            }
        }
        break;
    case HdInterpolationFaceVarying: {
        VtValue triangulated;
        const HdMeshComputationResult r = util.ComputeTriangulatedFaceVaryingPrimvar(
            src.cdata(), int(src.size()), type, &triangulated);
        if (r == HdMeshComputationResult::Error) {
            return false;
        }
        const VtArray<T> &perCorner = r == HdMeshComputationResult::Success
            ? triangulated.Get<VtArray<T>>() : src;
        if (perCorner.size() != corners) {
            return false;
        }
        for (size_t c = 0; c < corners; ++c) {
            put(c, perCorner[c]);
        }
        break;
    }
    default:
        return false;
    }
    *out = std::move(result);
    return true;
}

} // namespace

HdVkMesh::HdVkMesh(SdfPath const &id, vkr::Scene *scene)
    : HdMesh(id)
    , _scene(scene)
    , _mesh(scene->AddMesh())
{
}

HdVkMesh::~HdVkMesh()
{
    _scene->RemoveMesh(_mesh);
    for (vkr::Mesh *subset : _subsetMeshes) {
        _scene->RemoveMesh(subset);
    }
}

HdDirtyBits HdVkMesh::GetInitialDirtyBitsMask() const
{
    return HdChangeTracker::InitRepr | HdChangeTracker::DirtyPoints |
           HdChangeTracker::DirtyTopology | HdChangeTracker::DirtyTransform |
           HdChangeTracker::DirtyVisibility | HdChangeTracker::DirtyPrimvar |
           HdChangeTracker::DirtyNormals | HdChangeTracker::DirtyMaterialId |
           HdChangeTracker::DirtyInstancer | HdChangeTracker::DirtyInstanceIndex |
           HdChangeTracker::DirtyDisplayStyle | HdChangeTracker::DirtySubdivTags;
}

void HdVkMesh::UpdateRenderTag(HdSceneDelegate *delegate, HdRenderParam *renderParam)
{
    HdMesh::UpdateRenderTag(delegate, renderParam);
    _mesh->tag = HdVkRenderTagBit(GetRenderTag());
    for (vkr::Mesh *subset : _subsetMeshes) {
        subset->tag = _mesh->tag;
    }
}

// ponytail: reads only the "st" primvar for texcoords.
void HdVkMesh::Sync(HdSceneDelegate *sceneDelegate, HdRenderParam *,
                    HdDirtyBits *dirtyBits, TfToken const &)
{
    const SdfPath &id = GetId();
    vkr::Mesh &m = *_mesh;
    bool geometryDirty = false;
    m.primId = GetPrimId();  // assigned by the render index after construction

    // Skinned meshes get points and topology dirtied with every new pose
    // (UsdSkelImaging's own points come from the pose), while HdVk's are
    // the rest points: re-flatten only on a real change.
    if (HdChangeTracker::IsTopologyDirty(*dirtyBits, id) || (*dirtyBits & HdChangeTracker::DirtySubdivTags)) {
        HdMeshTopology topology(GetMeshTopology(sceneDelegate), 0);
        topology.SetSubdivTags(GetSubdivTags(sceneDelegate));  // creases, corners, boundary rules
        if (!(topology == _topology)) {
            _topology = topology;
            HdMeshUtil(&_topology, id).ComputeTriangleIndices(&_triangles, &_primitiveParams);
            geometryDirty = true;
        }
    }
    // Subdivision surfaces: refined to the display style's level (usdview's
    // Complexity) when flattened. The render delegate re-dirties the
    // display style when the subdivision settings change.
    if (HdChangeTracker::IsDisplayStyleDirty(*dirtyBits, id)) {
        const int level = _RefineLevel(sceneDelegate);
        geometryDirty |= level != _refineLevel && _topology.GetScheme() != PxOsdOpenSubdivTokens->none;
        _refineLevel = level;
    }
    const bool pointsDirty = HdChangeTracker::IsPrimvarDirty(*dirtyBits, id, HdTokens->points);
    if (pointsDirty) {
        const VtValue value = GetPoints(sceneDelegate);
        VtVec3fArray points = value.IsHolding<VtVec3fArray>() ? value.UncheckedGet<VtVec3fArray>()
                                                              : VtVec3fArray();
        if (points != _points) {
            _points = std::move(points);
            geometryDirty = true;
        }
    }
    // DirtyPrimvar is all primvars at once, and a skinned mesh gets it every
    // frame for its skinning transforms: re-flatten only what really changed.
    bool jointsDirty = false;
    // The uv set: the one the bound material's textures read, else "st",
    // else the first texture coordinate primvar.
    // ponytail: read when the mesh syncs; a material rewired to another uv
    // set takes effect with the mesh's next primvar or binding change. A
    // GeomSubset's material whose first uv set is another reads this one
    // there (its other sets are primvars and fine).
    TfToken uvName = _tokens->st;
    if (const auto *material = static_cast<const HdVkMaterial *>(sceneDelegate->GetRenderIndex().GetSprim(
            HdPrimTypeTokens->material, sceneDelegate->GetMaterialId(id)))) {
        if (!material->UvPrimvar().IsEmpty()) {
            uvName = material->UvPrimvar();
        }
    }
    if (uvName != _uvName ||
        HdChangeTracker::IsPrimvarDirty(*dirtyBits, id, HdTokens->normals) ||
        HdChangeTracker::IsPrimvarDirty(*dirtyBits, id, uvName)) {
        _uvName = uvName;
        HdInterpolation normalsInterp = _normalsInterp, uvsInterp = _uvsInterp;
        const VtValue normals = FindPrimvar(sceneDelegate, id, HdTokens->normals, &normalsInterp)
            ? sceneDelegate->Get(id, HdTokens->normals) : VtValue();
        TfToken uvFound = uvName;
        const VtValue uvs =
            FindPrimvar(sceneDelegate, id, uvName, &uvsInterp) ||
                    FindPrimvar(sceneDelegate, id, _tokens->st, &uvsInterp, &uvFound) ||
                    FindPrimvar(sceneDelegate, id, TfToken(), &uvsInterp, &uvFound)
                ? sceneDelegate->Get(id, uvFound) : VtValue();
        HdInterpolation colorsInterp = _colorsInterp;
        const VtValue colors = FindPrimvar(sceneDelegate, id, HdTokens->displayColor, &colorsInterp)
            ? sceneDelegate->Get(id, HdTokens->displayColor) : VtValue();
        if (normals != _normals || uvs != _uvs || colors != _colors || normalsInterp != _normalsInterp ||
            uvsInterp != _uvsInterp || colorsInterp != _colorsInterp) {
            _normals = normals;
            _uvs = uvs;
            _colors = colors;
            _normalsInterp = normalsInterp;
            _uvsInterp = uvsInterp;
            _colorsInterp = colorsInterp;
            geometryDirty = true;
        }
        // A constant displayColor is a per-mesh value, not vertex data.
        if (_colors.IsHolding<VtVec3fArray>() && !_colors.UncheckedGet<VtVec3fArray>().empty()) {
            const GfVec3f &c = _colors.UncheckedGet<VtVec3fArray>()[0];
            std::copy(c.data(), c.data() + 3, m.color);
        }

        auto get = [&](TfToken const &name) { return sceneDelegate->Get(id, name); };
        const VtVec2fArray influences = get(HdSkinningInputTokens->influences).GetWithDefault(VtVec2fArray());
        const int perPoint = get(HdSkinningInputTokens->numInfluencesPerComponent).GetWithDefault(0);
        const bool constant = get(HdSkinningInputTokens->hasConstantInfluences).GetWithDefault(false);
        const VtVec4fArray blendOffsets = get(HdSkinningInputTokens->blendShapeOffsets).GetWithDefault(VtVec4fArray());
        const VtVec2iArray blendRanges =
            get(HdSkinningInputTokens->blendShapeOffsetRanges).GetWithDefault(VtVec2iArray());
        const int blendRangeCount = get(HdSkinningInputTokens->numBlendShapeOffsetRanges).GetWithDefault(0);
        const bool dualQuaternion = get(HdSkinningInputTokens->numSkinningMethod).GetWithDefault(0) == 1;
        if (influences != _influences || perPoint != _influencesPerPoint || constant != _constantInfluences ||
            blendOffsets != _blendOffsets || blendRanges != _blendRanges || blendRangeCount != _blendRangeCount ||
            dualQuaternion != _dualQuaternion) {
            _influences = influences;
            _influencesPerPoint = perPoint;
            _constantInfluences = constant;
            _blendOffsets = blendOffsets;
            _blendRanges = blendRanges;
            _blendRangeCount = blendRangeCount;
            _dualQuaternion = dualQuaternion;
            geometryDirty = true;
        }
        _blendWeights = get(HdSkinningInputTokens->blendShapeWeights).GetWithDefault(VtFloatArray());
        _numBlendShapeWeights = get(HdSkinningInputTokens->numBlendShapeWeights).GetWithDefault(0);
        _geomBind = GfMatrix4d(get(HdSkinningSkelInputTokens->geomBindTransform).GetWithDefault(GfMatrix4f(1.0f)));
        _skinningXforms = get(HdSkinningInputTokens->skinningXforms).GetWithDefault(VtMatrix4fArray());
        _numJoints = get(HdSkinningInputTokens->numJoints).GetWithDefault(0);
        _skelToPrim = get(HdSkinningInputTokens->skelLocalToCommonSpace).GetWithDefault(GfMatrix4d(1.0)) *
                      get(HdSkinningInputTokens->commonSpaceToPrimLocal).GetWithDefault(GfMatrix4d(1.0));
        jointsDirty = true;
        // The joint count is baked into the flattened influences (index range).
        const size_t joints = _numJoints > 0 ? size_t(_numJoints) : _skinningXforms.size();
        geometryDirty |= _Skinned() && joints != _mesh->jointCount;
    }
    // Deformation blur: the points' motion through the shutter.
    if (pointsDirty || geometryDirty) {
        VtVec3fArray velocities = _ShutterVelocities(sceneDelegate);
        if (velocities != _velocities) {
            _velocities = std::move(velocities);
            geometryDirty = true;
        }
    }
    // Primvars the material programs read, as the mesh has them (re-read
    // with any primvar change; flattened with the geometry).
    // ponytail: like the uv set, a material rewired to other primvars takes
    // effect with the mesh's next primvar, topology or binding change.
    const std::vector<TfToken> programNames = _ProgramPrimvarNames(sceneDelegate);
    const bool programNamesChanged = programNames != _programPrimvarNames;
    if (programNamesChanged || HdChangeTracker::IsAnyPrimvarDirty(*dirtyBits, id) ||
        HdChangeTracker::IsTopologyDirty(*dirtyBits, id)) {
        std::vector<ProgramPrimvar> read;
        for (const TfToken &name : programNames) {
            ProgramPrimvar p{name};
            if (FindPrimvar(sceneDelegate, id, name, &p.interp) &&
                HdVkToFloats(sceneDelegate->Get(id, name), &p.values, &p.components) && p.components <= 4) {
                read.push_back(std::move(p));
            }
        }
        if (!(read == _programPrimvars)) {
            _programPrimvars = std::move(read);
            geometryDirty = true;
        }
        _programPrimvarNames = programNames;
    }
    // Planar reflections: primvars:eldr:planarReflection (bool or int)
    // makes this mesh the mirror.
    if (HdChangeTracker::IsPrimvarDirty(*dirtyBits, id, _tokens->planarReflection)) {
        const VtValue flag = sceneDelegate->Get(id, _tokens->planarReflection);
        m.planarReflection = flag.IsHolding<bool>() ? flag.UncheckedGet<bool>()
                           : flag.IsHolding<int>()  ? flag.UncheckedGet<int>() != 0
                                                    : false;
    }
    // Reflection probes: primvars:eldr:reflectionProbe (bool or int) makes
    // this mesh a probe's influence box instead of geometry.
    if (HdChangeTracker::IsPrimvarDirty(*dirtyBits, id, _tokens->reflectionProbe)) {
        const VtValue flag = sceneDelegate->Get(id, _tokens->reflectionProbe);
        m.reflectionProbe = flag.IsHolding<bool>() ? flag.UncheckedGet<bool>()
                          : flag.IsHolding<int>()  ? flag.UncheckedGet<int>() != 0
                                                   : false;
    }
    // Subdivision surfaces flatten their refined mesh (subdivision.cpp).
    if (geometryDirty && !_FlattenRefined()) {
        _Flatten();
        _SplitSubsets();
    }

    // Instancers are synced by the rprims that use them (as in hdEmbree);
    // the helper locks, so parallel rprim syncs are fine.
    _UpdateInstancer(sceneDelegate, dirtyBits);
    HdInstancer::_SyncInstancerAndParents(sceneDelegate->GetRenderIndex(), GetInstancerId());
    if (HdChangeTracker::IsTransformDirty(*dirtyBits, id) ||
        HdChangeTracker::IsInstancerDirty(*dirtyBits, id) ||
        HdChangeTracker::IsInstanceIndexDirty(*dirtyBits, id)) {
        HdVkSetTransforms(sceneDelegate, id, GetInstancerId(), m);
    }
    // Per-instance primvars (the instancer's), in the transforms' order.
    if (programNamesChanged || HdChangeTracker::IsInstancerDirty(*dirtyBits, id) ||
        HdChangeTracker::IsInstanceIndexDirty(*dirtyBits, id)) {
        _instancePrimvars.clear();
        if (auto *instancer = GetInstancerId().IsEmpty()
                                  ? nullptr
                                  : static_cast<HdVkInstancer *>(
                                        sceneDelegate->GetRenderIndex().GetInstancer(GetInstancerId()))) {
            for (const TfToken &name : _programPrimvarNames) {
                vkr::Primvar p{name.GetString(), vkr::Primvar::Rate::Instance};
                if (instancer->ComputeInstancePrimvar(id, name, &p.values, &p.components) && p.components <= 4) {
                    _instancePrimvars.push_back(std::move(p));
                }
            }
        }
    }
    _ApplyInstancePrimvars(m);
    if (jointsDirty || geometryDirty) {
        _UpdateJoints();
    }

    if (HdChangeTracker::IsVisibilityDirty(*dirtyBits, id)) {
        _UpdateVisibility(sceneDelegate, dirtyBits);
        m.visible = IsVisible();
    }
    if (HdChangeTracker::IsAnyPrimvarDirty(*dirtyBits, id)) {
        m.holdout = HdVkHoldout(sceneDelegate, id);
    }

    // Sprims sync before rprims, so the material already exists here.
    if (*dirtyBits & HdChangeTracker::DirtyMaterialId) {
        SetMaterialId(sceneDelegate->GetMaterialId(id));  // Cryptomatte's material ids
        const auto *material = static_cast<const HdVkMaterial *>(
            sceneDelegate->GetRenderIndex().GetSprim(
                HdPrimTypeTokens->material, GetMaterialId()));
        m.material = material ? material->Get() : nullptr;
    }
    _SyncSubsets(sceneDelegate);

    *dirtyBits &= ~HdChangeTracker::AllSceneDirtyBits;
}

// Moves the triangles of each GeomSubset with a material from _mesh into
// its own vkr::Mesh (a face in several subsets goes to the first).
void HdVkMesh::_SplitSubsets()
{
    std::vector<int> subsetOf;  // per HdGeomSubset: index into _subsetMeshes, or -1
    std::vector<SdfPath> materials;
    for (const HdGeomSubset &s : _topology.GetGeomSubsets()) {
        const bool used = s.type == HdGeomSubset::TypeFaceSet && !s.materialId.IsEmpty();
        subsetOf.push_back(used ? int(materials.size()) : -1);
        if (used) {
            materials.push_back(s.materialId);
        }
    }
    while (_subsetMeshes.size() > materials.size()) {
        _scene->RemoveMesh(_subsetMeshes.back());
        _subsetMeshes.pop_back();
    }
    while (_subsetMeshes.size() < materials.size()) {
        _subsetMeshes.push_back(_scene->AddMesh());
    }
    _subsetMaterials = std::move(materials);
    if (_subsetMeshes.empty()) {
        return;
    }

    // Triangles per part: 0 is _mesh, 1 + i is _subsetMeshes[i].
    const int faces = _topology.GetNumFaces();
    std::vector<int> faceSubset(size_t(std::max(faces, 0)), -1);
    const HdGeomSubsets &subsets = _topology.GetGeomSubsets();
    for (size_t s = 0; s < subsets.size(); ++s) {
        for (const int f : subsets[s].indices) {
            if (subsetOf[s] >= 0 && f >= 0 && f < faces && faceSubset[f] < 0) {
                faceSubset[f] = subsetOf[s];
            }
        }
    }
    std::vector<std::vector<size_t>> parts(_subsetMeshes.size() + 1);
    for (size_t t = 0; t < _primitiveParams.size(); ++t) {
        const int face = HdMeshUtil::DecodeFaceIndexFromCoarseFaceParam(_primitiveParams[t]);
        parts[face >= 0 && face < faces ? size_t(faceSubset[face] + 1) : 0].push_back(t);
    }

    vkr::Mesh &m = *_mesh;
    const size_t triangles = _primitiveParams.size();
    // The triangles `tris` of a per-corner array (n values per corner);
    // arrays that aren't per corner (empty ones) stay empty.
    auto pick = [&](auto const &src, size_t n, std::vector<size_t> const &tris, auto *dst) {
        dst->clear();
        if (src.size() != triangles * 3 * n) {
            return;
        }
        dst->reserve(tris.size() * 3 * n);
        for (const size_t t : tris) {
            dst->insert(dst->end(), src.begin() + t * 3 * n, src.begin() + (t + 1) * 3 * n);
        }
    };
    auto split = [&](std::vector<size_t> const &tris, vkr::Mesh *d) {
        d->primvars.clear();
        for (const vkr::Primvar &p : m.primvars) {
            vkr::Primvar q{p.name, p.rate, p.components};
            if (p.rate == vkr::Primvar::Rate::Vertex) {
                pick(p.values, p.components, tris, &q.values);
            } else if (p.rate == vkr::Primvar::Rate::Uniform && p.values.size() == triangles * p.components) {
                for (const size_t t : tris) {
                    q.values.insert(q.values.end(), p.values.begin() + t * p.components,
                                    p.values.begin() + (t + 1) * p.components);
                }
            } else if (p.rate == vkr::Primvar::Rate::Constant) {
                q.values = p.values;
            }
            if (!q.values.empty()) {
                d->primvars.push_back(std::move(q));
            }
        }
        pick(m.points, 3, tris, &d->points);
        pick(m.normals, 3, tris, &d->normals);
        pick(m.uvs, 2, tris, &d->uvs);
        pick(m.colors, 3, tris, &d->colors);
        pick(m.velocities, 3, tris, &d->velocities);
        pick(m.jointIndices, m.influences, tris, &d->jointIndices);
        pick(m.jointWeights, m.influences, tris, &d->jointWeights);
        d->influences = m.influences;
        d->dualQuaternion = m.dualQuaternion;
        pick(m.blendRanges, 2, tris, &d->blendRanges);
        d->blendOffsets = m.blendOffsets;
        d->jointCount = m.jointCount;
        d->geometryDirty = true;
        HdVkSplitIndexed(m, tris, d);  // refined subdivision surfaces are indexed
    };
    for (size_t i = 0; i < _subsetMeshes.size(); ++i) {
        split(parts[i + 1], _subsetMeshes[i]);
    }
    vkr::Mesh rest;
    split(parts[0], &rest);
    m.points = std::move(rest.points);
    m.normals = std::move(rest.normals);
    m.uvs = std::move(rest.uvs);
    m.colors = std::move(rest.colors);
    m.velocities = std::move(rest.velocities);
    m.jointIndices = std::move(rest.jointIndices);
    m.jointWeights = std::move(rest.jointWeights);
    m.blendRanges = std::move(rest.blendRanges);
    m.primvars = std::move(rest.primvars);
    m.indices = std::move(rest.indices);
}

// Subset meshes share everything but their triangles and material.
void HdVkMesh::_SyncSubsets(HdSceneDelegate *sceneDelegate)
{
    const vkr::Mesh &m = *_mesh;
    for (size_t i = 0; i < _subsetMeshes.size(); ++i) {
        vkr::Mesh &d = *_subsetMeshes[i];
        // Compared, not flagged: a new subset mesh needs them even when
        // _mesh's were uploaded long ago.
        if (d.transforms != m.transforms) {
            d.prevTransforms = m.shutterMotion ? m.prevTransforms : std::move(d.transforms);
            d.shutterMotion = m.shutterMotion;
            d.transforms = m.transforms;
            d.transformsDirty = true;
        }
        if (d.skinningTransforms != m.skinningTransforms || d.blendWeights != m.blendWeights) {
            d.skinningTransforms = m.skinningTransforms;
            d.blendWeights = m.blendWeights;
            d.blendShapeCount = m.blendShapeCount;
            d.jointsDirty = true;
        }
        std::copy(m.color, m.color + 3, d.color);
        d.primId = m.primId;
        d.visible = m.visible;
        d.holdout = m.holdout;
        d.tag = m.tag;
        const auto *material = static_cast<const HdVkMaterial *>(
            sceneDelegate->GetRenderIndex().GetSprim(HdPrimTypeTokens->material, _subsetMaterials[i]));
        d.material = material ? material->Get() : nullptr;
        _ApplyInstancePrimvars(d);
    }
}

// The primvars the programs of the mesh's material and its subsets' read.
std::vector<TfToken> HdVkMesh::_ProgramPrimvarNames(HdSceneDelegate *sceneDelegate) const
{
    std::vector<TfToken> names;
    auto add = [&](SdfPath const &materialId) {
        const auto *material = static_cast<const HdVkMaterial *>(
            sceneDelegate->GetRenderIndex().GetSprim(HdPrimTypeTokens->material, materialId));
        for (const TfToken &name : material ? material->Primvars() : std::vector<TfToken>()) {
            if (std::find(names.begin(), names.end(), name) == names.end()) {
                names.push_back(name);
            }
        }
    };
    add(sceneDelegate->GetMaterialId(GetId()));
    for (const HdGeomSubset &s : _topology.GetGeomSubsets()) {
        if (!s.materialId.IsEmpty()) {
            add(s.materialId);
        }
    }
    return names;
}

// Replaces `mesh`'s per-instance primvars with the instancer's.
void HdVkMesh::_ApplyInstancePrimvars(vkr::Mesh &mesh) const
{
    auto &p = mesh.primvars;
    p.erase(std::remove_if(p.begin(), p.end(), [](auto const &v) { return v.rate == vkr::Primvar::Rate::Instance; }),
            p.end());
    p.insert(p.end(), _instancePrimvars.begin(), _instancePrimvars.end());
}

bool HdVkMesh::_Skinned() const
{
    return _influencesPerPoint > 0 && !_influences.empty() && !_skinningXforms.empty();
}

// Deformation blur (eldr:deformationBlur, with sampled motion's shutter):
// the points' velocity per frame from their samples at shutter open and
// close, continued linearly as the camera's and the transforms' are. Empty
// when they don't move through it, when skinned (joints move those) or
// when the point count changes inside it.
VtVec3fArray HdVkMesh::_ShutterVelocities(HdSceneDelegate *sceneDelegate) const
{
    auto *vk = static_cast<HdVkRenderDelegate *>(sceneDelegate->GetRenderIndex().GetRenderDelegate());
    float open, close;
    if (_Skinned() || !vk->GetRenderSetting<bool>(HdVkRenderSettingsTokens2->deformationBlur, true) ||
        !vk->SampledMotionShutter(&open, &close)) {
        return {};
    }
    HdTimeSampleArray<VtValue, 4> boxed;
    sceneDelegate->SamplePrimvar(GetId(), HdTokens->points, open, close, &boxed);
    HdTimeSampleArray<VtVec3fArray, 4> samples;
    if (boxed.count < 2 || !samples.UnboxFrom(boxed)) {
        return {};
    }
    const VtVec3fArray a = samples.Resample(open), b = samples.Resample(close);
    if (a.size() != _points.size() || b.size() != a.size() || a == b) {
        return {};
    }
    VtVec3fArray velocities(a.size());
    for (size_t i = 0; i < a.size(); ++i) {
        velocities[i] = (b[i] - a[i]) / (close - open);
    }
    return velocities;
}

// Skinning transform of joint j: a rest point through geomBind, the joint's
// skinning transform and skeleton to prim space, the chain of
// UsdSkelImaging's skinning kernel in one matrix (USD row-vector order). The
// GPU adds each instance's transform. Deferred skinning gives every
// instance its own set (_numJoints per instance).
void HdVkMesh::_UpdateJoints()
{
    vkr::Mesh &m = *_mesh;
    m.jointsDirty = true;
    if (!_Skinned() || m.jointCount == 0) {
        m.skinningTransforms.clear();
        m.blendWeights.clear();
        return;
    }
    m.blendWeights.assign(_blendWeights.cbegin(), _blendWeights.cend());
    m.blendShapeCount = uint32_t(_numBlendShapeWeights > 0 ? _numBlendShapeWeights : int(_blendWeights.size()));
    const GfMatrix4f geomBind(_geomBind), skelToPrim(_skelToPrim);
    m.skinningTransforms.resize(_skinningXforms.size() * 16);
    for (size_t s = 0; s < _skinningXforms.size(); ++s) {
        const GfMatrix4f joint = geomBind * _skinningXforms[s] * skelToPrim;
        std::copy(joint.data(), joint.data() + 16, m.skinningTransforms.data() + s * 16);
    }
}

void HdVkMesh::_Flatten()
{
    vkr::Mesh &m = *_mesh;
    m.geometryDirty = true;
    m.primvars.clear();  // rebuilt at the end; instance ones are applied after
    const HdMeshUtil util(&_topology, GetId());

    // Joint influences per corner: up to eight (four when the skeleton
    // binding has no more), the strongest, renormalized.
    m.jointIndices.clear();
    m.jointWeights.clear();
    m.jointCount = 0;
    m.blendOffsets.clear();
    m.blendRanges.clear();
    const size_t perPoint = size_t(std::max(_influencesPerPoint, 0));
    const uint32_t joints = uint32_t(_numJoints > 0 ? _numJoints : _skinningXforms.size());
    if (_Skinned() && _influences.size() >= (_constantInfluences ? perPoint : _points.size() * perPoint)) {
        m.jointCount = joints;
        m.jointIndices.reserve(_triangles.size() * 12);
        m.jointWeights.reserve(_triangles.size() * 12);
        // Blend shapes: the offsets as they are, a range per corner.
        if (!_blendOffsets.empty() && _blendRangeCount > 0) {
            m.blendOffsets.reserve(_blendOffsets.size() * 4);
            for (const GfVec4f &o : _blendOffsets) {
                m.blendOffsets.insert(m.blendOffsets.end(), o.data(), o.data() + 4);
            }
            m.blendRanges.reserve(_triangles.size() * 6);
            for (const GfVec3i &tri : _triangles) {
                for (int k = 0; k < 3; ++k) {
                    const int point = tri[k];
                    const bool has = point >= 0 && point < _blendRangeCount && size_t(point) < _blendRanges.size();
                    const GfVec2i r = has ? _blendRanges[point] : GfVec2i(0, 0);
                    const bool ok = r[0] >= 0 && r[1] >= r[0] && size_t(r[1]) <= _blendOffsets.size();
                    m.blendRanges.push_back(ok ? uint32_t(r[0]) : 0u);
                    m.blendRanges.push_back(ok ? uint32_t(r[1] - r[0]) : 0u);
                }
            }
        }
        const size_t kept = perPoint > 4 ? 8 : 4;
        m.influences = uint32_t(kept);
        m.dualQuaternion = _dualQuaternion;
        for (const GfVec3i &tri : _triangles) {
            for (int k = 0; k < 3; ++k) {
                const size_t base = _constantInfluences ? 0 : size_t(std::max(tri[k], 0)) * perPoint;
                std::array<std::pair<float, uint16_t>, 8> top{};
                for (size_t n = 0; n < perPoint && base + n < _influences.size(); ++n) {
                    const GfVec2f &influence = _influences[base + n];
                    const int joint = int(influence[0]);
                    if (influence[1] > top[kept - 1].first && joint >= 0 && uint32_t(joint) < joints) {
                        top[kept - 1] = {influence[1], uint16_t(joint)};
                        std::sort(top.begin(), top.begin() + kept, [](auto &a, auto &b) { return a.first > b.first; });
                    }
                }
                float sum = 0.0f;
                for (size_t n = 0; n < kept; ++n) {
                    sum += top[n].first;
                }
                for (size_t n = 0; n < kept; ++n) {
                    m.jointIndices.push_back(top[n].second);
                    m.jointWeights.push_back(sum > 0.0f ? top[n].first / sum : 0.0f);
                }
            }
        }
    }

    if (!FlattenPrimvar<GfVec3f>(VtValue(_points), HdInterpolationVertex, util, _triangles,
                                 _primitiveParams, HdTypeFloatVec3, &m.points)) {
        // Indices past the points array: draw nothing rather than garbage.
        m.normals.clear();
        m.uvs.clear();
        return;
    }

    // Authored normals win. Otherwise cage-only schemes stay faceted (empty
    // normals: the shader uses the geometric normal) and subdivision schemes
    // get smooth vertex normals as a stand-in for the limit surface.
    if (!FlattenPrimvar<GfVec3f>(_normals, _normalsInterp, util, _triangles,
                                 _primitiveParams, HdTypeFloatVec3, &m.normals)) {
        const TfToken scheme = _topology.GetScheme();
        if (scheme != PxOsdOpenSubdivTokens->none && scheme != PxOsdOpenSubdivTokens->bilinear) {
            Hd_VertexAdjacency adjacency;
            adjacency.BuildAdjacencyTable(&_topology);
            const VtVec3fArray smooth = Hd_SmoothNormals::ComputeSmoothNormals(
                &adjacency, int(_points.size()), _points.cdata());
            FlattenPrimvar<GfVec3f>(VtValue(smooth), HdInterpolationVertex, util, _triangles,
                                    _primitiveParams, HdTypeFloatVec3, &m.normals);
        }
    }

    FlattenPrimvar<GfVec2f>(_uvs, _uvsInterp, util, _triangles, _primitiveParams,
                            HdTypeFloatVec2, &m.uvs);
    m.velocities.clear();
    if (!_velocities.empty()) {
        FlattenPrimvar<GfVec3f>(VtValue(_velocities), HdInterpolationVertex, util, _triangles, _primitiveParams,
                                HdTypeFloatVec3, &m.velocities);
    }
    m.colors.clear();
    if (_colorsInterp != HdInterpolationConstant) {
        FlattenPrimvar<GfVec3f>(_colors, _colorsInterp, util, _triangles, _primitiveParams,
                                HdTypeFloatVec3, &m.colors);
    }

    // The programs' primvars: one value (constant), one per triangle
    // (uniform) or per corner (vertex, varying, faceVarying).
    for (const ProgramPrimvar &p : _programPrimvars) {
        const size_t n = p.components, elements = p.values.size() / n;
        vkr::Primvar out{p.name.GetString(), vkr::Primvar::Rate::Vertex, p.components};
        auto element = [&](size_t i) {
            if (i < elements) {
                out.values.insert(out.values.end(), p.values.begin() + i * n, p.values.begin() + (i + 1) * n);
            } else {
                out.values.insert(out.values.end(), n, 0.0f);
            }
        };
        switch (p.interp) {
        case HdInterpolationConstant:
            out.rate = vkr::Primvar::Rate::Constant;
            element(0);
            break;
        case HdInterpolationUniform:
            out.rate = vkr::Primvar::Rate::Uniform;
            for (const int param : _primitiveParams) {
                element(size_t(std::max(HdMeshUtil::DecodeFaceIndexFromCoarseFaceParam(param), 0)));
            }
            break;
        case HdInterpolationVertex:
        case HdInterpolationVarying:
            for (const GfVec3i &tri : _triangles) {
                for (int k = 0; k < 3; ++k) {
                    element(size_t(std::max(tri[k], 0)));
                }
            }
            break;
        case HdInterpolationFaceVarying: {
            static const HdType types[4] = {HdTypeFloat, HdTypeFloatVec2, HdTypeFloatVec3, HdTypeFloatVec4};
            VtValue triangulated;
            const HdMeshComputationResult r = util.ComputeTriangulatedFaceVaryingPrimvar(
                p.values.data(), int(elements), types[n - 1], &triangulated);
            uint32_t components = 0;
            if (r == HdMeshComputationResult::Success) {
                HdVkToFloats(triangulated, &out.values, &components);
            } else if (r == HdMeshComputationResult::Unchanged) {
                out.values = p.values;
            }
            if (out.values.size() != _triangles.size() * 3 * n) {
                out.values.clear();
            }
            break;
        }
        default:
            break;
        }
        if (!out.values.empty()) {
            m.primvars.push_back(std::move(out));
        }
    }
}

PXR_NAMESPACE_CLOSE_SCOPE
