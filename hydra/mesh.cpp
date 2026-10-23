#include "mesh.h"

#include "instancer.h"
#include "material.h"
#include "renderDelegate.h"
#include "scene.h"

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

TF_DEFINE_PRIVATE_TOKENS(_tokens, (st));

// Finds a primvar's interpolation; false if the prim doesn't have it.
bool FindPrimvar(HdSceneDelegate *sd, SdfPath const &id, TfToken const &name,
                 HdInterpolation *interp)
{
    for (int i = 0; i < HdInterpolationCount; ++i) {
        for (const HdPrimvarDescriptor &d :
             sd->GetPrimvarDescriptors(id, HdInterpolation(i))) {
            if (d.name == name) {
                *interp = HdInterpolation(i);
                return true;
            }
        }
    }
    return false;
}

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
}

HdDirtyBits HdVkMesh::GetInitialDirtyBitsMask() const
{
    return HdChangeTracker::InitRepr | HdChangeTracker::DirtyPoints |
           HdChangeTracker::DirtyTopology | HdChangeTracker::DirtyTransform |
           HdChangeTracker::DirtyVisibility | HdChangeTracker::DirtyPrimvar |
           HdChangeTracker::DirtyNormals | HdChangeTracker::DirtyMaterialId |
           HdChangeTracker::DirtyInstancer | HdChangeTracker::DirtyInstanceIndex;
}

void HdVkMesh::UpdateRenderTag(HdSceneDelegate *delegate, HdRenderParam *renderParam)
{
    HdMesh::UpdateRenderTag(delegate, renderParam);
    _mesh->tag = HdVkRenderTagBit(GetRenderTag());
}

// ponytail: renders the subdivision cage, reads only the "st" primvar for
// texcoords.
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
    if (HdChangeTracker::IsTopologyDirty(*dirtyBits, id)) {
        HdMeshTopology topology(GetMeshTopology(sceneDelegate), 0);
        if (!(topology == _topology)) {
            _topology = topology;
            HdMeshUtil(&_topology, id).ComputeTriangleIndices(&_triangles, &_primitiveParams);
            geometryDirty = true;
        }
    }
    if (HdChangeTracker::IsPrimvarDirty(*dirtyBits, id, HdTokens->points)) {
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
    if (HdChangeTracker::IsPrimvarDirty(*dirtyBits, id, HdTokens->normals) ||
        HdChangeTracker::IsPrimvarDirty(*dirtyBits, id, _tokens->st)) {
        HdInterpolation normalsInterp = _normalsInterp, uvsInterp = _uvsInterp;
        const VtValue normals = FindPrimvar(sceneDelegate, id, HdTokens->normals, &normalsInterp)
            ? sceneDelegate->Get(id, HdTokens->normals) : VtValue();
        const VtValue uvs = FindPrimvar(sceneDelegate, id, _tokens->st, &uvsInterp)
            ? sceneDelegate->Get(id, _tokens->st) : VtValue();
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
        if (influences != _influences || perPoint != _influencesPerPoint || constant != _constantInfluences ||
            blendOffsets != _blendOffsets || blendRanges != _blendRanges || blendRangeCount != _blendRangeCount) {
            _influences = influences;
            _influencesPerPoint = perPoint;
            _constantInfluences = constant;
            _blendOffsets = blendOffsets;
            _blendRanges = blendRanges;
            _blendRangeCount = blendRangeCount;
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
    if (geometryDirty) {
        _Flatten();
    }

    // Instancers are synced by the rprims that use them (as in hdEmbree);
    // the helper locks, so parallel rprim syncs are fine.
    _UpdateInstancer(sceneDelegate, dirtyBits);
    HdInstancer::_SyncInstancerAndParents(sceneDelegate->GetRenderIndex(), GetInstancerId());
    if (HdChangeTracker::IsTransformDirty(*dirtyBits, id) ||
        HdChangeTracker::IsInstancerDirty(*dirtyBits, id) ||
        HdChangeTracker::IsInstanceIndexDirty(*dirtyBits, id)) {
        const GfMatrix4d xf = sceneDelegate->GetTransform(id);
        VtMatrix4dArray instances(1, GfMatrix4d(1));
        if (!GetInstancerId().IsEmpty()) {
            auto *instancer = static_cast<HdVkInstancer *>(
                sceneDelegate->GetRenderIndex().GetInstancer(GetInstancerId()));
            instances = instancer ? instancer->ComputeInstanceTransforms(id) : VtMatrix4dArray();
        }
        std::vector<float> transforms(instances.size() * 16);
        for (size_t i = 0; i < instances.size(); ++i) {
            const GfMatrix4f world(xf * instances[i]);  // prototype first, then instance
            std::copy(world.data(), world.data() + 16, transforms.data() + i * 16);
        }
        // Animated skinned prototypes arrive with their instancer dirty every
        // frame; re-upload thousands of instances only when they moved.
        if (transforms != m.transforms) {
            m.transforms = std::move(transforms);
            m.transformsDirty = true;
        }
    }
    if (jointsDirty || geometryDirty) {
        _UpdateJoints();
    }

    if (HdChangeTracker::IsVisibilityDirty(*dirtyBits, id)) {
        _UpdateVisibility(sceneDelegate, dirtyBits);
        m.visible = IsVisible();
    }

    // Sprims sync before rprims, so the material already exists here.
    if (*dirtyBits & HdChangeTracker::DirtyMaterialId) {
        const auto *material = static_cast<const HdVkMaterial *>(
            sceneDelegate->GetRenderIndex().GetSprim(
                HdPrimTypeTokens->material, sceneDelegate->GetMaterialId(id)));
        m.material = material ? material->Get() : nullptr;
    }

    *dirtyBits &= ~HdChangeTracker::AllSceneDirtyBits;
}

bool HdVkMesh::_Skinned() const
{
    return _influencesPerPoint > 0 && !_influences.empty() && !_skinningXforms.empty();
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
    const HdMeshUtil util(&_topology, GetId());

    // Joint influences per corner: the four strongest, renormalized.
    // ponytail: more than four influences per point are cut to four.
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
        for (const GfVec3i &tri : _triangles) {
            for (int k = 0; k < 3; ++k) {
                const size_t base = _constantInfluences ? 0 : size_t(std::max(tri[k], 0)) * perPoint;
                std::array<std::pair<float, uint16_t>, 4> top{};
                for (size_t n = 0; n < perPoint && base + n < _influences.size(); ++n) {
                    const GfVec2f &influence = _influences[base + n];
                    const int joint = int(influence[0]);
                    if (influence[1] > top[3].first && joint >= 0 && uint32_t(joint) < joints) {
                        top[3] = {influence[1], uint16_t(joint)};
                        std::sort(top.begin(), top.end(), [](auto &a, auto &b) { return a.first > b.first; });
                    }
                }
                const float sum = top[0].first + top[1].first + top[2].first + top[3].first;
                for (const auto &[weight, joint] : top) {
                    m.jointIndices.push_back(joint);
                    m.jointWeights.push_back(sum > 0.0f ? weight / sum : 0.0f);
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
    m.colors.clear();
    if (_colorsInterp != HdInterpolationConstant) {
        FlattenPrimvar<GfVec3f>(_colors, _colorsInterp, util, _triangles, _primitiveParams,
                                HdTypeFloatVec3, &m.colors);
    }
}

PXR_NAMESPACE_CLOSE_SCOPE
