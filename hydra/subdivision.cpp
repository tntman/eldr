#include "subdivision.h"

#include "instancer.h"
#include "mesh.h"
#include "renderDelegate.h"
#include "scene.h"

#include "pxr/base/gf/matrix4f.h"
#include "pxr/base/gf/vec4d.h"
#include "pxr/base/trace/trace.h"
#include "pxr/imaging/hd/meshUtil.h"
#include "pxr/imaging/hd/renderIndex.h"
#include "pxr/imaging/hd/sceneDelegate.h"
#include "pxr/imaging/hd/smoothNormals.h"
#include "pxr/imaging/hd/vertexAdjacency.h"
#include "pxr/imaging/pxOsd/meshTopologyValidation.h"
#include "pxr/imaging/pxOsd/refinerFactory.h"
#include "pxr/imaging/pxOsd/tokens.h"

#include <opensubdiv/far/primvarRefiner.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>
#include <optional>
#include <unordered_map>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

namespace Far = OpenSubdiv::Far;

// One element of a dense primvar (up to four floats) while it is refined.
struct Value {
    float v[4] = {0, 0, 0, 0};
    void Clear() { v[0] = v[1] = v[2] = v[3] = 0.0f; }
    void AddWithWeight(Value const &s, float w)
    {
        for (int k = 0; k < 4; ++k) {
            v[k] += w * s.v[k];
        }
    }
};

// Adds `value` under `key`.
void Put(HdVkSubdivision::Sparse &s, int key, GfVec3f const &value)
{
    const auto it = std::find_if(s.begin(), s.end(), [&](auto const &e) { return e.first == key; });
    if (it != s.end()) {
        it->second += value;
    } else {
        s.emplace_back(key, value);
    }
}

// One vertex's sparse values while they are refined.
struct SparseValue {
    HdVkSubdivision::Sparse e;
    void Clear() { e.clear(); }
    void AddWithWeight(SparseValue const &s, float w)
    {
        for (const auto &[key, value] : s.e) {
            Put(e, key, value * w);
        }
    }
};

// `src` (the cage's elements) refined level by level to the last level;
// the limit is up to the caller.
template <typename T>
std::vector<T> RefineLevels(Far::TopologyRefiner const &refiner, std::vector<T> src, HdInterpolation interp,
                            int channel)
{
    const Far::PrimvarRefiner primvar(refiner);
    for (int level = 1; level <= refiner.GetMaxLevel(); ++level) {
        const Far::TopologyLevel &l = refiner.GetLevel(level);
        std::vector<T> dst(interp == HdInterpolationFaceVarying ? l.GetNumFVarValues(channel) : l.GetNumVertices());
        if (interp == HdInterpolationVertex) {
            primvar.Interpolate(level, src, dst);
        } else if (interp == HdInterpolationVarying) {
            primvar.InterpolateVarying(level, src, dst);
        } else {
            primvar.InterpolateFaceVarying(level, src, dst, channel);
        }
        src = std::move(dst);
    }
    return src;
}

} // namespace

bool HdVkSubdivision::Update(HdMeshTopology const &cage, int level, std::vector<VtIntArray> const &fvar)
{
    if (level == _level && fvar == _fvar && cage == _cage) {
        return _refiner != nullptr;
    }
    TRACE_FUNCTION();
    _cage = cage;
    _level = level;
    _fvar = fvar;
    _refiner.reset();
    // Invalid topology (indices past the counts, bad tags) stays a cage.
    if (level <= 0 || !cage.GetPxOsdMeshTopology().Validate()) {
        return false;
    }
    _refiner = PxOsdRefinerFactory::Create(cage.GetPxOsdMeshTopology(), fvar);
    if (!_refiner) {
        return false;
    }
    Far::TopologyRefiner::UniformOptions options(level);
    options.fullTopologyInLastLevel = true;  // the limit needs it
    _refiner->RefineUniform(options);

    // Refined face -> cage face, level by level.
    const Far::PrimvarRefiner primvar(*_refiner);
    _faceParents.resize(size_t(_refiner->GetLevel(0).GetNumFaces()));
    std::iota(_faceParents.begin(), _faceParents.end(), 0);
    for (int l = 1; l <= level; ++l) {
        std::vector<int> child(size_t(_refiner->GetLevel(l).GetNumFaces()));
        primvar.InterpolateFaceUniform(l, _faceParents, child);
        _faceParents = std::move(child);
    }

    const Far::TopologyLevel &last = _refiner->GetLevel(level);
    std::vector<bool> hole(size_t(cage.GetNumFaces()));
    for (const int f : cage.GetHoleIndices()) {
        if (f >= 0 && size_t(f) < hole.size()) {
            hole[f] = true;
        }
    }
    VtIntArray counts(size_t(last.GetNumFaces())), holes;
    std::vector<int> indices;
    for (int f = 0; f < last.GetNumFaces(); ++f) {
        const Far::ConstIndexArray v = last.GetFaceVertices(f);
        counts[f] = v.size();
        indices.insert(indices.end(), v.begin(), v.end());
        if (hole[_faceParents[f]]) {
            holes.push_back(f);
        }
    }
    _topology = HdMeshTopology(cage.GetScheme(), PxOsdOpenSubdivTokens->rightHanded, counts,
                               VtIntArray(indices.begin(), indices.end()), holes);

    // GeomSubsets: each cage face's refined faces.
    HdGeomSubsets subsets = cage.GetGeomSubsets();
    if (!subsets.empty()) {
        const size_t faces = hole.size();
        std::vector<int> first(faces + 1, 0), order(_faceParents.size());
        for (const int p : _faceParents) {
            ++first[p + 1];
        }
        std::partial_sum(first.begin(), first.end(), first.begin());
        std::vector<int> next(first.begin(), first.end() - 1);
        for (size_t f = 0; f < _faceParents.size(); ++f) {
            order[next[_faceParents[f]]++] = int(f);
        }
        for (HdGeomSubset &s : subsets) {
            VtIntArray refined;
            for (const int f : s.indices) {
                if (f >= 0 && size_t(f) < faces) {
                    refined.insert(refined.end(), order.begin() + first[f], order.begin() + first[f + 1]);
                }
            }
            s.indices = refined;
        }
        _topology.SetGeomSubsets(subsets);
    }
    return true;
}

bool HdVkSubdivision::Refine(std::vector<float> const &values, uint32_t n, HdInterpolation interp, int channel,
                             std::vector<float> *out) const
{
    out->clear();
    if (!_refiner || n < 1 || n > 4) {
        return false;
    }
    if (interp == HdInterpolationConstant) {
        *out = values;
        return values.size() >= n;
    }
    if (interp == HdInterpolationUniform) {
        if (values.size() < size_t(_cage.GetNumFaces()) * n) {
            return false;
        }
        out->reserve(_faceParents.size() * n);
        for (const int parent : _faceParents) {
            out->insert(out->end(), values.begin() + size_t(parent) * n, values.begin() + size_t(parent + 1) * n);
        }
        return true;
    }
    const bool fvar = interp == HdInterpolationFaceVarying;
    if ((fvar && (channel < 0 || channel >= _refiner->GetNumFVarChannels())) ||
        (!fvar && interp != HdInterpolationVertex && interp != HdInterpolationVarying)) {
        return false;
    }
    const Far::TopologyLevel &cage = _refiner->GetLevel(0);
    const size_t elements = size_t(fvar ? cage.GetNumFVarValues(channel) : cage.GetNumVertices());
    if (values.size() < elements * n) {
        return false;
    }
    std::vector<Value> src(elements);
    for (size_t i = 0; i < elements; ++i) {
        std::copy_n(&values[i * n], n, src[i].v);
    }
    std::vector<Value> last = RefineLevels(*_refiner, std::move(src), interp, channel);
    // Vertex and face-varying data go to the limit, where the points are.
    std::vector<Value> limit(last.size());
    const Far::PrimvarRefiner primvar(*_refiner);
    if (interp == HdInterpolationVertex) {
        primvar.Limit(last, limit);
    } else if (fvar) {
        primvar.LimitFaceVarying(last, limit, channel);
    } else {
        limit = std::move(last);
    }
    out->reserve(limit.size() * n);
    for (const Value &v : limit) {
        out->insert(out->end(), v.v, v.v + n);
    }
    return true;
}

bool HdVkSubdivision::RefinePoints(VtVec3fArray const &points, VtVec3fArray *out, VtVec3fArray *normals) const
{
    if (!_refiner || points.size() < size_t(_refiner->GetLevel(0).GetNumVertices())) {
        return false;
    }
    std::vector<Value> src(size_t(_refiner->GetLevel(0).GetNumVertices()));
    for (size_t i = 0; i < src.size(); ++i) {
        std::copy_n(points[i].data(), 3, src[i].v);
    }
    const std::vector<Value> last = RefineLevels(*_refiner, std::move(src), HdInterpolationVertex, 0);
    std::vector<Value> position(last.size()), du(last.size()), dv(last.size());
    Far::PrimvarRefiner(*_refiner).Limit(last, position, du, dv);
    out->resize(last.size());
    normals->resize(last.size());
    std::vector<size_t> degenerate;
    for (size_t i = 0; i < last.size(); ++i) {
        (*out)[i] = GfVec3f(position[i].v[0], position[i].v[1], position[i].v[2]);
        GfVec3f &n = (*normals)[i];
        n = GfCross(GfVec3f(du[i].v[0], du[i].v[1], du[i].v[2]), GfVec3f(dv[i].v[0], dv[i].v[1], dv[i].v[2]));
        if (!(n.Normalize() > 1e-20f)) {
            degenerate.push_back(i);
        }
    }
    // Bilinear surfaces stay faceted; where a limit normal is degenerate
    // (collapsed geometry) the refined mesh's smooth normal stands in.
    if (_cage.GetScheme() == PxOsdOpenSubdivTokens->bilinear) {
        normals->clear();
    } else if (!degenerate.empty()) {
        Hd_VertexAdjacency adjacency;
        adjacency.BuildAdjacencyTable(&_topology);
        const VtVec3fArray smooth = Hd_SmoothNormals::ComputeSmoothNormals(&adjacency, int(out->size()), out->cdata());
        for (const size_t i : degenerate) {
            (*normals)[i] = i < smooth.size() ? smooth[i] : GfVec3f(0.0f);
        }
    }
    return true;
}

std::vector<HdVkSubdivision::Sparse> HdVkSubdivision::RefineSparse(std::vector<Sparse> const &values) const
{
    std::vector<SparseValue> src(static_cast<size_t>(_refiner->GetLevel(0).GetNumVertices()));
    for (size_t i = 0; i < src.size() && i < values.size(); ++i) {
        src[i].e = values[i];
    }
    const std::vector<SparseValue> last = RefineLevels(*_refiner, std::move(src), HdInterpolationVertex, 0);
    std::vector<SparseValue> limit(last.size());
    Far::PrimvarRefiner(*_refiner).Limit(last, limit);
    std::vector<Sparse> out(limit.size());
    for (size_t i = 0; i < limit.size(); ++i) {
        out[i] = std::move(limit[i].e);
    }
    return out;
}

HdVkSubdivision::Triangles HdVkSubdivision::Triangulate(bool perFace) const
{
    Triangles t;
    const Far::TopologyLevel &level = _refiner->GetLevel(_level);
    const int channels = _refiner->GetNumFVarChannels(), stride = channels + 1;
    // Each refined vertex's vertices so far, chained (usually one to four).
    std::vector<int> head(size_t(level.GetNumVertices()), -1), next;
    std::vector<int> key(static_cast<size_t>(stride));
    std::vector<uint32_t> corners;
    const VtIntArray &holes = _topology.GetHoleIndices();  // sorted
    size_t hole = 0;
    for (int f = 0; f < level.GetNumFaces(); ++f) {
        if (hole < holes.size() && holes[hole] == f) {
            ++hole;
            continue;
        }
        const Far::ConstIndexArray verts = level.GetFaceVertices(f);
        corners.clear();
        for (int k = 0; k < verts.size(); ++k) {
            for (int c = 0; c < channels; ++c) {
                key[c] = level.GetFaceFVarValues(f, c)[k];
            }
            key[channels] = perFace ? f : -1;
            int o = head[verts[k]];
            while (o >= 0 && !std::equal(key.begin(), key.end(), t.keys.begin() + size_t(o) * stride)) {
                o = next[o];
            }
            if (o < 0) {
                o = int(t.vertex.size());
                t.vertex.push_back(verts[k]);
                t.keys.insert(t.keys.end(), key.begin(), key.end());
                next.push_back(head[verts[k]]);
                head[verts[k]] = o;
            }
            corners.push_back(uint32_t(o));
        }
        for (size_t k = 2; k < corners.size(); ++k) {  // a fan, as HdMeshUtil splits quads
            t.indices.insert(t.indices.end(), {corners[0], corners[k - 1], corners[k]});
            t.faces.push_back(f);
        }
    }
    return t;
}

VtIntArray HdVkSubdivision::Weld(HdMeshTopology const &cage, std::vector<float> *values, uint32_t n)
{
    const VtIntArray &verts = cage.GetFaceVertexIndices();
    if (n < 1 || n > 4 || values->size() != verts.size() * n) {
        return VtIntArray();
    }
    struct Key {
        int vertex;
        float v[4];
        bool operator==(Key const &o) const { return std::memcmp(this, &o, sizeof(Key)) == 0; }
    };
    struct Hash {
        size_t operator()(Key const &k) const
        {
            uint64_t h = 1469598103934665603ull;  // FNV-1a
            const auto *p = reinterpret_cast<const uint8_t *>(&k);
            for (size_t i = 0; i < sizeof(Key); ++i) {
                h = (h ^ p[i]) * 1099511628211ull;
            }
            return size_t(h);
        }
    };
    std::unordered_map<Key, int, Hash> ids;
    std::vector<float> welded;
    VtIntArray indices(verts.size());
    for (size_t c = 0; c < verts.size(); ++c) {
        Key k{verts[c], {0, 0, 0, 0}};
        std::copy_n(&(*values)[c * n], n, k.v);
        const auto [it, inserted] = ids.emplace(k, int(welded.size() / n));
        if (inserted) {
            welded.insert(welded.end(), k.v, k.v + n);
        }
        indices[c] = it->second;
    }
    *values = std::move(welded);
    return indices;
}

void HdVkSplitIndexed(vkr::Mesh const &m, std::vector<size_t> const &tris, vkr::Mesh *d)
{
    d->indices.clear();
    if (m.indices.empty()) {
        return;
    }
    const size_t vertices = m.points.size() / 3, triangles = m.indices.size() / 3;
    std::vector<uint32_t> remap(vertices, vkr::kNone), used;
    for (const size_t t : tris) {
        for (int k = 0; k < 3; ++k) {
            uint32_t &r = remap[m.indices[t * 3 + k]];
            if (r == vkr::kNone) {
                r = uint32_t(used.size());
                used.push_back(m.indices[t * 3 + k]);
            }
            d->indices.push_back(r);
        }
    }
    // The used vertices' values of a per-vertex array (n per vertex; others empty).
    auto gather = [&](auto const &src, size_t n, auto *dst) {
        dst->clear();
        if (src.size() == vertices * n) {
            dst->reserve(used.size() * n);
            for (const uint32_t v : used) {
                dst->insert(dst->end(), src.begin() + v * n, src.begin() + (v + 1) * n);
            }
        }
    };
    gather(m.points, 3, &d->points);
    gather(m.normals, 3, &d->normals);
    gather(m.uvs, 2, &d->uvs);
    gather(m.colors, 3, &d->colors);
    gather(m.velocities, 3, &d->velocities);
    gather(m.jointIndices, m.influences, &d->jointIndices);
    gather(m.jointWeights, m.influences, &d->jointWeights);
    gather(m.blendRanges, 2, &d->blendRanges);
    d->primvars.clear();
    for (const vkr::Primvar &p : m.primvars) {
        vkr::Primvar q{p.name, p.rate, p.components};
        if (p.rate == vkr::Primvar::Rate::Vertex) {
            gather(p.values, p.components, &q.values);
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
}

int HdVkScreenRefineLevel(HdVkDicingView const &view, HdMeshTopology const &topology, VtVec3fArray const &points,
                          std::vector<float> const &transforms, float quality, int maxLevel)
{
    // Per cage face (object space): centre, radius and area (a fan).
    struct Face {
        GfVec3d centre;
        double radius, area;
    };
    std::vector<Face> faces;
    const VtIntArray &counts = topology.GetFaceVertexCounts();
    const VtIntArray &indices = topology.GetFaceVertexIndices();
    faces.reserve(counts.size());
    size_t k = 0;
    for (const int n : counts) {
        const size_t first = k;
        k += size_t(std::max(n, 0));
        if (n < 3 || k > indices.size()) {
            continue;
        }
        auto p = [&](int i) {
            const int v = indices[first + i];
            return v >= 0 && size_t(v) < points.size() ? GfVec3d(points[v]) : GfVec3d(0.0);
        };
        Face f{GfVec3d(0.0), 0.0, 0.0};
        for (int i = 0; i < n; ++i) {
            f.centre += p(i);
        }
        f.centre /= double(n);
        for (int i = 0; i < n; ++i) {
            f.radius = std::max(f.radius, (p(i) - f.centre).GetLength());
            if (i >= 2) {
                f.area += 0.5 * GfCross(p(i - 1) - p(0), p(i) - p(0)).GetLength();
            }
        }
        faces.push_back(f);
    }
    if (faces.empty() || quality <= 0.0f || view.width <= 0 || view.height <= 0 || maxLevel <= 0) {
        return 0;
    }
    // Polygons a cage face needs; each level splits it in 4.
    const double enough = std::pow(4.0, maxLevel), q2 = double(quality) * quality;
    constexpr double kOffscreen = 0.1 * 0.1;  // Karma's offscreen quality, squared
    const size_t instances = transforms.size() / 16;
    // ponytail: every face under every instance; past ~4M pairs only every
    // n-th instance (a nearby one may be skipped).
    const size_t step = std::max<size_t>(1, faces.size() * instances / (size_t(1) << 22));
    double need = 0.0;
    for (size_t t = 0; t < instances; t += step) {
        const GfMatrix4d m(GfMatrix4f(reinterpret_cast<const float(*)[4]>(transforms.data() + t * 16)));
        const double scale = std::cbrt(std::abs(m.GetDeterminant3()));  // its uniform part
        for (const Face &f : faces) {
            const GfVec3d c = m.Transform(f.centre);
            const double r = f.radius * scale;
            double perUnit = view.pixels;  // pixels per unit of size there
            if (!view.ortho) {
                const double distance = (c - view.eye).GetLength() - r;
                if (distance <= 1e-9) {
                    return maxLevel;  // the eye is at the face
                }
                perUnit /= distance;
            }
            // On screen: the face's sphere reaches into the frame.
            const GfVec4d clip = GfVec4d(c[0], c[1], c[2], 1.0) * view.viewProj;
            const double rx = 2.0 * r * perUnit / view.width, ry = 2.0 * r * perUnit / view.height;
            const bool on = clip[3] > 0.0 ? std::abs(clip[0] / clip[3]) <= 1.0 + rx &&
                                                std::abs(clip[1] / clip[3]) <= 1.0 + ry
                                          : clip[3] > -r;
            need = std::max(need, f.area * scale * scale * perUnit * perUnit * q2 * (on ? 1.0 : kOffscreen));
            if (need >= enough) {
                return maxLevel;
            }
        }
    }
    return need <= 1.0 ? 0 : std::min(maxLevel, int(std::ceil(0.5 * std::log2(need))));
}

// The display style's refine level (usdview's Complexity, Houdini's Level of
// Detail) or, with eldr:screenDicing in final quality, the level for the
// mesh's size on screen (primvars:eldr:object:dicingquality, else Karma's,
// 0.5); the mesh's own primvars:eldr:refineLevel wins over both. At least
// eldr:object:dicingdepthmin, at most eldr:object:dicingdepthmax (else
// Karma's; -1: none), within the render settings' limit; 0 draws the cage.
int HdVkMesh::_RefineLevel(HdSceneDelegate *sceneDelegate) const
{
    auto *delegate = static_cast<HdVkRenderDelegate *>(sceneDelegate->GetRenderIndex().GetRenderDelegate());
    const auto &t = *HdVkRenderSettingsTokens;
    if (!delegate->GetRenderSetting<bool>(t.subdivision, kHdVkSubdivision)) {
        return 0;
    }
    const SdfPath &id = GetId();
    const int limit = std::clamp(delegate->GetRenderSetting<int>(t.maxRefineLevel, kHdVkMaxRefineLevel), 0, 8);
    static const TfToken own("eldr:refineLevel");
    int level = sceneDelegate->GetDisplayStyle(id).refineLevel;
    if (const std::optional<int> fixed = HdVkPrimvarIfSet<int>(sceneDelegate, id, own)) {
        level = *fixed;
    } else if (delegate->ScreenDicing()) {
        // Before a frame named the camera: the cage offline (husk renders
        // only after the re-dice), else the display style's level.
        if (const HdVkDicingView *view = delegate->DicingView()) {
            level = HdVkScreenRefineLevel(*view, _topology, _points,
                                          HdVkWorldTransforms(sceneDelegate, id, GetInstancerId()),
                                          HdVkObjectPrimvar<float>(sceneDelegate, id, "object:dicingquality", 0.5f),
                                          limit);
        } else if (delegate->Offline()) {
            level = 0;
        }
    }
    level = std::max(level, HdVkObjectPrimvar<int>(sceneDelegate, id, "object:dicingdepthmin", -1));
    const int karmaMax = HdVkObjectPrimvar<int>(sceneDelegate, id, "object:dicingdepthmax", -1);
    if (karmaMax >= 0) {
        level = std::min(level, karmaMax);
    }
    return std::clamp(level, 0, limit);
}

// _Flatten for a subdivision surface: the cage refined to _refineLevel, as
// indexed vertices (per corner, a level 3 Kitchen_set took gigabytes), then
// split into GeomSubsets. False: flatten the cage.
bool HdVkMesh::_FlattenRefined()
{
    using Sparse = HdVkSubdivision::Sparse;
    vkr::Mesh &m = *_mesh;
    m.indices.clear();  // the cage is flattened per corner
    if (_refineLevel <= 0 || _topology.GetScheme() == PxOsdOpenSubdivTokens->none) {
        _subdivision.reset();
        return false;
    }
    TRACE_FUNCTION();
    // _Flatten's primvars as floats (uvs, colors, then the programs'); the
    // face-varying ones welded into a channel each.
    struct Input {
        std::vector<float> values;
        uint32_t n = 0;
        HdInterpolation interp = HdInterpolationConstant;
        int channel = -1;
    };
    std::vector<Input> inputs;
    std::vector<VtIntArray> fvar;
    auto add = [&](std::vector<float> values, uint32_t n, HdInterpolation interp) {
        Input in{std::move(values), n, interp};
        if (interp == HdInterpolationFaceVarying) {
            VtIntArray indices = HdVkSubdivision::Weld(_topology, &in.values, n);
            in.channel = indices.empty() ? -1 : int(fvar.size());
            if (!indices.empty()) {
                fvar.push_back(std::move(indices));
            }
        }
        inputs.push_back(std::move(in));
    };
    for (auto [value, interp, n] : {std::tuple{&_uvs, _uvsInterp, 2u}, {&_colors, _colorsInterp, 3u}}) {
        std::vector<float> f;
        uint32_t components = 0;
        add(HdVkToFloats(*value, &f, &components) && components == n ? f : std::vector<float>(), n, interp);
    }
    for (const ProgramPrimvar &p : _programPrimvars) {
        add(p.values, p.components, p.interp);
    }
    if (!_subdivision) {
        _subdivision = std::make_shared<HdVkSubdivision>();
    }
    const HdVkSubdivision &sub = *_subdivision;
    VtVec3fArray points, normals;
    if (!_subdivision->Update(_topology, _refineLevel, fvar) || !sub.RefinePoints(_points, &points, &normals)) {
        return false;
    }

    // Vertices: a refined vertex per distinct combination of face-varying
    // values (and per face, for uniform uvs or colours).
    const HdVkSubdivision::Triangles tri =
        sub.Triangulate(_uvsInterp == HdInterpolationUniform || _colorsInterp == HdInterpolationUniform);
    const size_t vertices = tri.vertex.size(), stride = size_t(sub.Channels()) + 1;
    // A refined primvar per vertex (constant ones aren't).
    auto perVertex = [&](Input const &in, std::vector<float> *out) {
        std::vector<float> refined;
        out->clear();
        if (in.interp == HdInterpolationConstant || !sub.Refine(in.values, in.n, in.interp, in.channel, &refined)) {
            return;
        }
        out->resize(vertices * in.n);
        for (size_t o = 0; o < vertices; ++o) {
            const size_t e = in.interp == HdInterpolationFaceVarying ? size_t(tri.keys[o * stride + in.channel])
                             : in.interp == HdInterpolationUniform   ? size_t(tri.keys[o * stride + stride - 1])
                                                                     : size_t(tri.vertex[o]);
            std::copy_n(&refined[e * in.n], in.n, &(*out)[o * in.n]);
        }
    };
    m.geometryDirty = true;
    m.indices = tri.indices;
    m.points.resize(vertices * 3);
    m.normals.resize(normals.empty() ? 0 : vertices * 3);
    for (size_t o = 0; o < vertices; ++o) {
        std::copy_n(points[tri.vertex[o]].data(), 3, &m.points[o * 3]);
        if (!normals.empty()) {
            std::copy_n(normals[tri.vertex[o]].data(), 3, &m.normals[o * 3]);
        }
    }
    perVertex(inputs[0], &m.uvs);
    perVertex(inputs[1], &m.colors);
    // Velocities refine as the points do (limit stencils are linear).
    m.velocities.clear();
    VtVec3fArray velocities, unused;
    if (!_velocities.empty() && sub.RefinePoints(_velocities, &velocities, &unused)) {
        m.velocities.resize(vertices * 3);
        for (size_t o = 0; o < vertices; ++o) {
            std::copy_n(velocities[tri.vertex[o]].data(), 3, &m.velocities[o * 3]);
        }
    }
    // The programs' primvars: per mesh, triangle or vertex.
    m.primvars.clear();
    for (size_t i = 0; i < _programPrimvars.size(); ++i) {
        const Input &in = inputs[2 + i];
        vkr::Primvar out{_programPrimvars[i].name.GetString(), vkr::Primvar::Rate::Vertex, in.n};
        if (in.interp == HdInterpolationConstant) {
            out.rate = vkr::Primvar::Rate::Constant;
            out.values.assign(in.values.begin(), in.values.begin() + std::min<size_t>(in.n, in.values.size()));
        } else if (in.interp == HdInterpolationUniform) {
            std::vector<float> refined;
            out.rate = vkr::Primvar::Rate::Uniform;
            if (sub.Refine(in.values, in.n, in.interp, -1, &refined)) {
                for (const int f : tri.faces) {
                    out.values.insert(out.values.end(), refined.begin() + size_t(f) * in.n,
                                      refined.begin() + size_t(f + 1) * in.n);
                }
            }
        } else {
            perVertex(in, &out.values);
        }
        if (!out.values.empty()) {
            m.primvars.push_back(std::move(out));
        }
    }

    // Skinning: each refined vertex blends its stencil's influences (the
    // strongest four, eight if the cage had more than four; renormalized)
    // and blend shape offsets.
    // ponytail: skinning after refinement, not refinement of the posed
    // cage; the difference is small where neighbouring weights are alike.
    m.jointIndices.clear();
    m.jointWeights.clear();
    m.jointCount = 0;
    m.blendOffsets.clear();
    m.blendRanges.clear();
    const size_t perPoint = size_t(std::max(_influencesPerPoint, 0));
    const uint32_t joints = uint32_t(_numJoints > 0 ? _numJoints : _skinningXforms.size());
    if (_Skinned() && _influences.size() >= (_constantInfluences ? perPoint : _points.size() * perPoint)) {
        std::vector<Sparse> src(_constantInfluences ? 1 : _points.size());
        for (size_t p = 0; p < src.size(); ++p) {
            for (size_t k = 0; k < perPoint; ++k) {
                const GfVec2f &w = _influences[p * perPoint + k];
                if (w[1] > 0.0f && w[0] >= 0.0f && uint32_t(w[0]) < joints) {
                    Put(src[p], int(w[0]), GfVec3f(w[1], 0, 0));
                }
            }
        }
        std::vector<Sparse> refined = _constantInfluences ? src : sub.RefineSparse(src);
        const size_t kept = perPoint > 4 ? 8 : 4;
        m.jointCount = joints;
        m.influences = uint32_t(kept);
        m.dualQuaternion = _dualQuaternion;
        for (Sparse &s : refined) {
            std::sort(s.begin(), s.end(), [](auto const &a, auto const &b) { return a.second[0] > b.second[0]; });
            s.resize(std::min(s.size(), kept));
        }
        m.jointIndices.reserve(vertices * kept);
        m.jointWeights.reserve(vertices * kept);
        for (size_t o = 0; o < vertices; ++o) {
            const Sparse &s = refined[_constantInfluences ? 0 : size_t(tri.vertex[o])];
            float sum = 0.0f;
            for (const auto &e : s) {
                sum += std::max(e.second[0], 0.0f);
            }
            for (size_t k = 0; k < kept; ++k) {
                m.jointIndices.push_back(k < s.size() ? uint16_t(s[k].first) : uint16_t(0));
                m.jointWeights.push_back(k < s.size() && sum > 0.0f ? std::max(s[k].second[0], 0.0f) / sum : 0.0f);
            }
        }
        if (!_blendOffsets.empty() && _blendRangeCount > 0) {
            std::vector<Sparse> shapes(std::min({size_t(_blendRangeCount), _blendRanges.size(), _points.size()}));
            for (size_t p = 0; p < shapes.size(); ++p) {
                const GfVec2i range = _blendRanges[p];
                for (int o = std::max(range[0], 0); o < range[1] && size_t(o) < _blendOffsets.size(); ++o) {
                    const GfVec4f &d = _blendOffsets[o];
                    Put(shapes[p], int(d[3]), GfVec3f(d[0], d[1], d[2]));
                }
            }
            // Offsets per refined vertex, then a range per vertex.
            std::vector<uint32_t> first;
            for (const Sparse &s : sub.RefineSparse(shapes)) {
                first.push_back(uint32_t(m.blendOffsets.size() / 4));
                for (const auto &[shape, d] : s) {
                    m.blendOffsets.insert(m.blendOffsets.end(), {d[0], d[1], d[2], float(shape)});
                }
            }
            first.push_back(uint32_t(m.blendOffsets.size() / 4));
            for (size_t o = 0; o < vertices; ++o) {
                const size_t v = size_t(tri.vertex[o]);
                m.blendRanges.insert(m.blendRanges.end(), {first[v], first[v + 1] - first[v]});
            }
        }
    }

    // GeomSubsets: _SplitSubsets on the refined faces.
    HdMeshTopology topology = sub.Topology();
    VtIntArray params(tri.faces.size());
    for (size_t t = 0; t < tri.faces.size(); ++t) {
        params[t] = HdMeshUtil::EncodeCoarseFaceParam(tri.faces[t], 0);
    }
    std::swap(_topology, topology);
    std::swap(_primitiveParams, params);
    _SplitSubsets();
    std::swap(_topology, topology);
    std::swap(_primitiveParams, params);
    if (!_animated) {
        _subdivision.reset();  // `sub` is not used past here
    }
    return true;
}

PXR_NAMESPACE_CLOSE_SCOPE
