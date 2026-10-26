#include "curves.h"

#include "instancer.h"
#include "material.h"
#include "mesh.h"
#include "renderDelegate.h"
#include "scene.h"

#include "pxr/base/gf/vec3f.h"
#include "pxr/imaging/hd/renderIndex.h"
#include "pxr/imaging/hd/sceneDelegate.h"
#include "pxr/imaging/hd/tokens.h"

#include <algorithm>
#include <cmath>
#include <type_traits>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

constexpr int kTubeSides = 6;
// Polyline steps of a cubic segment, by how far its hull turns: one when
// straight (hair stays cheap), 8 for a quarter turn, at most 16.
constexpr float kStepsPerRadian = 5.1f;
constexpr int kMaxSteps = 16;

// A primvar's values and interpolation (constant without values if the
// prim doesn't have it).
template <typename T>
struct Primvar {
    VtArray<T> values;
    HdInterpolation interp = HdInterpolationConstant;

    Primvar(HdSceneDelegate *sd, SdfPath const &id, TfToken const &name)
    {
        for (int i = 0; i < HdInterpolationCount; ++i) {
            for (const HdPrimvarDescriptor &d : sd->GetPrimvarDescriptors(id, HdInterpolation(i))) {
                if (d.name == name) {
                    interp = HdInterpolation(i);
                    values = sd->Get(id, name).GetWithDefault(VtArray<T>());
                }
            }
        }
    }
    T At(size_t i, T const &fallback) const { return i < values.size() ? values[i] : fallback; }
    bool Varying() const { return interp == HdInterpolationVarying || interp == HdInterpolationFaceVarying; }
};

// What a curve carries along its length.
struct Sample {
    GfVec3f p{0.0f}, color{0.0f}, normal{0.0f};
    float width = 0.0f;
};

void Accumulate(Sample *out, float w, Sample const &s)
{
    out->p += w * s.p;
    out->color += w * s.color;
    out->normal += w * s.normal;
    out->width += w * s.width;
}

// Weights of a segment's control vertices (two when linear) at t.
void Basis(bool linear, TfToken const &basis, float t, float w[4])
{
    const float s = 1.0f - t, t2 = t * t, t3 = t2 * t;
    if (linear) {
        w[0] = s;
        w[1] = t;
    } else if (basis == HdTokens->bezier) {
        w[0] = s * s * s;
        w[1] = 3.0f * t * s * s;
        w[2] = 3.0f * t2 * s;
        w[3] = t3;
    } else if (basis == HdTokens->catmullRom) {
        w[0] = 0.5f * (-t3 + 2.0f * t2 - t);
        w[1] = 0.5f * (3.0f * t3 - 5.0f * t2 + 2.0f);
        w[2] = 0.5f * (-3.0f * t3 + 4.0f * t2 + t);
        w[3] = 0.5f * (t3 - t2);
    } else {  // bspline
        w[0] = s * s * s / 6.0f;
        w[1] = (3.0f * t3 - 6.0f * t2 + 4.0f) / 6.0f;
        w[2] = (-3.0f * t3 + 3.0f * t2 + 3.0f * t + 1.0f) / 6.0f;
        w[3] = t3 / 6.0f;
    }
}

// A curve primvar at one control vertex; varying ones are applied per
// sample instead.
template <typename T>
T Value(Primvar<T> const &pv, size_t curve, size_t point, T const &fallback)
{
    switch (pv.interp) {
    case HdInterpolationConstant: return pv.At(0, fallback);
    case HdInterpolationUniform: return pv.At(curve, fallback);
    case HdInterpolationVertex: return pv.At(point, fallback);
    default: return fallback;
    }
}

// Triangles built straight into a vkr::Mesh, indexed.
struct Builder {
    vkr::Mesh &m;
    bool colors;  // per vertex; else the mesh's constant colour
    bool uvs;     // per vertex (curves)

    uint32_t Vertex(GfVec3f const &p, GfVec3f const &n, GfVec3f const &c, float u = 0.0f, float v = 0.0f)
    {
        m.points.insert(m.points.end(), p.data(), p.data() + 3);
        m.normals.insert(m.normals.end(), n.data(), n.data() + 3);
        if (colors) {
            m.colors.insert(m.colors.end(), c.data(), c.data() + 3);
        }
        if (uvs) {
            m.uvs.insert(m.uvs.end(), {u, v});
        }
        return uint32_t(m.points.size() / 3 - 1);
    }
    void Triangle(uint32_t a, uint32_t b, uint32_t c) { m.indices.insert(m.indices.end(), {a, b, c}); }
};

// A tube along a polyline, or with normals a ribbon across them, or
// `facing` a camera-facing ribbon (vkr::Mesh::facing: both vertices on the
// curve, the tangent times the half width as their normal). A closed
// polyline (last sample on the first) is welded there. Their uvs: u runs
// along the curve, 0 at its first point to 1 at its last (hair shading's
// strand direction, shaders/brdf.glsl), v around or across it.
// ponytail: a closed curve's u jumps back to 0 at the weld.
void Extrude(std::vector<Sample> const &line, bool ribbon, bool facing, bool closed, Builder &out)
{
    const size_t n = line.size();
    closed = closed && n >= 3;
    ribbon = ribbon || facing;
    const uint32_t ring = ribbon ? 2 : kTubeSides;
    const uint32_t first = uint32_t(out.m.points.size() / 3);
    std::vector<float> along(n, 0.0f);
    for (size_t i = 1; i < n; ++i) {
        along[i] = along[i - 1] + (line[i].p - line[i - 1].p).GetLength();
    }
    const float length = n > 0 && along.back() > 0.0f ? along.back() : 1.0f;
    GfVec3f tangent(0.0f), frame(0.0f);
    for (size_t i = 0; i < (closed ? n - 1 : n); ++i) {
        const size_t next = i + 1 < n ? i + 1 : closed ? 1 : n - 1;
        const size_t prev = i > 0 ? i - 1 : closed ? n - 2 : 0;
        const GfVec3f d = line[next].p - line[prev].p;
        tangent = d.GetLength() > 1e-12f ? d.GetNormalized() : tangent;
        const Sample &s = line[i];
        const float r = 0.5f * s.width, u = along[i] / length;
        if (facing) {
            out.Vertex(s.p, r * tangent, s.color, u, 0.0f);
            out.Vertex(s.p, r * tangent, s.color, u, 1.0f);
            continue;
        }
        if (ribbon) {
            const GfVec3f side = GfCross(tangent, s.normal).GetNormalized() * r;
            out.Vertex(s.p - side, s.normal, s.color, u, 0.0f);
            out.Vertex(s.p + side, s.normal, s.color, u, 1.0f);
            continue;
        }
        // Parallel transport: the last frame vector, made perpendicular again.
        frame -= GfDot(frame, tangent) * tangent;
        if (frame.GetLength() < 1e-6f) {
            frame = GfCross(tangent, std::abs(tangent[0]) < 0.9f ? GfVec3f(1, 0, 0) : GfVec3f(0, 1, 0));
        }
        frame.Normalize();
        const GfVec3f binormal = GfCross(tangent, frame);
        for (uint32_t k = 0; k < ring; ++k) {
            const float a = 6.2831853f * float(k) / float(ring);
            const GfVec3f dir = std::cos(a) * frame + std::sin(a) * binormal;
            out.Vertex(s.p + r * dir, dir, s.color, u, float(k) / float(ring));
        }
    }
    for (size_t i = 0; i + 1 < n; ++i) {
        const uint32_t r0 = first + uint32_t(i) * ring;
        const uint32_t r1 = closed && i + 2 == n ? first : r0 + ring;
        for (uint32_t k = 0; k < (ribbon ? 1 : ring); ++k) {
            const uint32_t k1 = (k + 1) % ring;
            out.Triangle(r0 + k, r0 + k1, r1 + k1);
            out.Triangle(r0 + k, r1 + k1, r1 + k);
        }
    }
}

// Each curve as a polyline: cubic segments in steps by how they bend, vertex
// primvars with the same basis, varying ones linearly along each segment
// (UsdGeomBasisCurves' rules), then extruded.
void Curves(HdBasisCurvesTopology const &topology, VtVec3fArray const &points, Primvar<float> const &widths,
            Primvar<GfVec3f> const &colors, Primvar<GfVec3f> const &normals, GfVec3f const &color, bool facing,
            Builder &out)
{
    const TfToken basis = topology.GetCurveBasis();
    const bool linear = topology.GetCurveType() == HdTokens->linear;
    const bool bezier = !linear && basis == HdTokens->bezier;
    const bool periodic = topology.GetCurveWrap() == HdTokens->periodic;
    const bool pinned = !linear && !bezier && topology.GetCurveWrap() == HdTokens->pinned;
    const VtIntArray &counts = topology.GetCurveVertexCounts();
    const VtIntArray &indices = topology.GetCurveIndices();
    const bool ribbon = !normals.values.empty();
    const int step = bezier ? 3 : 1;
    std::vector<Sample> cv, line;
    size_t vertex0 = 0, varying0 = 0;
    for (size_t c = 0; c < counts.size(); vertex0 += size_t(std::max(counts[c], 0)), ++c) {
        const int n = counts[c];
        if (n < 1) {
            continue;
        }
        // Varying values: one per segment end (as Hydra counts them).
        const int varyings = linear ? n : periodic ? std::max(n / step, 1) : std::max(n - 4, 0) / step + 2;
        const size_t varyingBase = varying0;
        varying0 += size_t(varyings);

        cv.assign(size_t(n), Sample{});
        for (int v = 0; v < n; ++v) {
            const size_t vi = vertex0 + size_t(v);
            const size_t point = indices.empty() ? vi : vi < indices.size() ? size_t(std::max(indices[vi], 0)) : 0;
            Sample &s = cv[v];
            s.p = point < points.size() ? points[point] : GfVec3f(0.0f);
            s.width = Value(widths, c, point, 1.0f);
            s.color = Value(colors, c, point, color);
            s.normal = Value(normals, c, point, GfVec3f(0, 0, 1));
        }
        // Pinned: phantom points that make the curve reach its ends.
        if (pinned && n >= 2) {
            Sample first, last;
            Accumulate(&first, 2.0f, cv[0]);
            Accumulate(&first, -1.0f, cv[1]);
            Accumulate(&last, 2.0f, cv[n - 1]);
            Accumulate(&last, -1.0f, cv[n - 2]);
            first.width = std::max(first.width, 0.0f);
            last.width = std::max(last.width, 0.0f);
            cv.insert(cv.begin(), first);
            cv.push_back(last);
        }
        const int size = int(cv.size());
        const int segments = linear ? (periodic ? n : n - 1)
                             : bezier ? (periodic ? n / 3 : (n - 1) / 3)
                                      : (periodic ? n : size - 3);
        line.clear();
        for (int k = 0; k < segments; ++k) {
            float turn = 0.0f;
            for (int j = 0; !linear && j < 2; ++j) {
                const GfVec3f &a = cv[(k * step + j) % size].p, &b = cv[(k * step + j + 1) % size].p;
                const GfVec3f e0 = b - a, e1 = cv[(k * step + j + 2) % size].p - b;
                const float l = e0.GetLength() * e1.GetLength();
                turn += l > 0.0f ? std::acos(std::clamp(GfDot(e0, e1) / l, -1.0f, 1.0f)) : 0.0f;
            }
            const int steps = std::clamp(int(std::ceil(turn * kStepsPerRadian)), 1, kMaxSteps);
            const size_t v0 = varyingBase + size_t(std::min(k, varyings - 1));
            const size_t v1 = varyingBase + size_t(periodic ? (k + 1) % varyings : std::min(k + 1, varyings - 1));
            for (int s = k == 0 ? 0 : 1; s <= steps; ++s) {
                const float t = float(s) / float(steps);
                float w[4];
                Basis(linear, basis, t, w);
                Sample out;
                for (int j = 0; j < (linear ? 2 : 4); ++j) {
                    Accumulate(&out, w[j], cv[(k * step + j) % size]);
                }
                auto varying = [&](auto const &pv, auto *field) {
                    if (pv.Varying()) {
                        *field = (1.0f - t) * pv.At(v0, *field) + t * pv.At(v1, *field);
                    }
                };
                varying(widths, &out.width);
                varying(colors, &out.color);
                varying(normals, &out.normal);
                line.push_back(out);
            }
        }
        Extrude(line, ribbon, facing && !ribbon, periodic, out);
    }
}

// A constant bool (or int) primvar in Eldr's own namespace (as
// hydra/material.cpp's toon inputs), `fallback` without one:
// `primvars:eldr:hair` (curves shade as hair, default true; false shades
// cables and ropes as ordinary surfaces), `eldr:ribbons` and `eldr:sprites`
// (camera-facing curves and points; default: the render setting).
bool BoolPrimvar(HdSceneDelegate *sd, SdfPath const &id, char const *name, bool fallback)
{
    const VtValue v = sd->Get(id, TfToken(name));
    if (v.IsHolding<bool>()) {
        return v.UncheckedGet<bool>();
    }
    const VtValue i = VtValue::Cast<int>(v);
    return i.IsEmpty() ? fallback : i.UncheckedGet<int>() != 0;
}

// A unit icosahedron: its vertices (also the normals) and outward faces.
constexpr float kA = 0.5257311f, kB = 0.8506508f;
const GfVec3f kIcosahedron[12] = {{-kA, kB, 0}, {kA, kB, 0},  {-kA, -kB, 0}, {kA, -kB, 0},
                                  {0, -kA, kB}, {0, kA, kB},  {0, -kA, -kB}, {0, kA, -kB},
                                  {kB, 0, -kA}, {kB, 0, kA},  {-kB, 0, -kA}, {-kB, 0, kA}};
constexpr int kIcosahedronFaces[20][3] = {{0, 11, 5}, {0, 5, 1},  {0, 1, 7},   {0, 7, 10}, {0, 10, 11},
                                          {1, 5, 9},  {5, 11, 4}, {11, 10, 2}, {10, 7, 6}, {7, 1, 8},
                                          {3, 9, 4},  {3, 4, 2},  {3, 2, 6},   {3, 6, 8},  {3, 8, 9},
                                          {4, 9, 5},  {2, 4, 11}, {6, 2, 10},  {8, 6, 7},  {9, 8, 1}};

// Each point as a sphere `widths` across, or `facing` a camera-facing disc
// (vkr::Mesh::facing): an octagon of 8 triangles around a centre vertex,
// all on the point, their normals the offsets within the disc.
void Spheres(VtVec3fArray const &points, Primvar<float> const &widths, Primvar<GfVec3f> const &colors,
             GfVec3f const &color, bool facing, Builder &out)
{
    auto at = [](auto const &pv, size_t i, auto const &fallback) {
        return pv.At(pv.interp == HdInterpolationConstant ? 0 : i, fallback);
    };
    for (size_t i = 0; i < points.size(); ++i) {
        const float r = 0.5f * at(widths, i, 1.0f);
        const GfVec3f c = at(colors, i, color);
        const uint32_t first = uint32_t(out.m.points.size() / 3);
        if (facing) {
            // Corners 1.0539 r out: the octagon's area is the disc's.
            const float corner = r * 1.0539f;
            out.Vertex(points[i], GfVec3f(0.0f), c);
            for (int k = 0; k < 8; ++k) {
                const float a = 0.7853982f * float(k);
                out.Vertex(points[i], corner * GfVec3f(std::cos(a), std::sin(a), 0.0f), c);
                out.Triangle(first, first + 1 + uint32_t(k), first + 1 + uint32_t((k + 1) % 8));
            }
            continue;
        }
        for (const GfVec3f &v : kIcosahedron) {
            out.Vertex(points[i] + r * v, v, c);
        }
        for (const auto &f : kIcosahedronFaces) {
            out.Triangle(first + f[0], first + f[1], first + f[2]);
        }
    }
}

} // namespace

template <typename Base>
HdVkTessellated<Base>::HdVkTessellated(SdfPath const &id, vkr::Scene *scene)
    : Base(id)
    , _scene(scene)
    , _mesh(scene->AddMesh())
{
}

template <typename Base>
HdVkTessellated<Base>::~HdVkTessellated()
{
    _scene->RemoveMesh(_mesh);
}

template <typename Base>
HdDirtyBits HdVkTessellated<Base>::GetInitialDirtyBitsMask() const
{
    return HdChangeTracker::InitRepr | HdChangeTracker::DirtyPoints | HdChangeTracker::DirtyTopology |
           HdChangeTracker::DirtyWidths | HdChangeTracker::DirtyNormals | HdChangeTracker::DirtyPrimvar |
           HdChangeTracker::DirtyTransform | HdChangeTracker::DirtyVisibility |
           HdChangeTracker::DirtyMaterialId | HdChangeTracker::DirtyInstancer |
           HdChangeTracker::DirtyInstanceIndex;
}

template <typename Base>
void HdVkTessellated<Base>::UpdateRenderTag(HdSceneDelegate *delegate, HdRenderParam *renderParam)
{
    Base::UpdateRenderTag(delegate, renderParam);
    _mesh->tag = HdVkRenderTagBit(this->GetRenderTag());
}

template <typename Base>
void HdVkTessellated<Base>::Sync(HdSceneDelegate *sceneDelegate, HdRenderParam *,
                                 HdDirtyBits *dirtyBits, TfToken const &)
{
    HdVkRenderDelegate::NoteSceneEdit(sceneDelegate);
    const SdfPath &id = this->GetId();
    vkr::Mesh &m = *_mesh;
    m.primId = this->GetPrimId();

    if (*dirtyBits & (HdChangeTracker::DirtyPoints | HdChangeTracker::DirtyTopology |
                      HdChangeTracker::DirtyWidths | HdChangeTracker::DirtyNormals |
                      HdChangeTracker::DirtyPrimvar)) {
        const VtVec3fArray points = sceneDelegate->Get(id, HdTokens->points).GetWithDefault(VtVec3fArray());
        const Primvar<float> widths(sceneDelegate, id, HdTokens->widths);
        const Primvar<GfVec3f> colors(sceneDelegate, id, HdTokens->displayColor);
        const GfVec3f color = colors.interp == HdInterpolationConstant ? colors.At(0, GfVec3f(0.5f)) : GfVec3f(0.5f);
        std::copy(color.data(), color.data() + 3, m.color);
        m.points.clear();
        m.normals.clear();
        m.uvs.clear();
        m.colors.clear();
        m.indices.clear();
        constexpr bool curves = std::is_same_v<Base, HdBasisCurves>;
        Builder out{m, colors.interp != HdInterpolationConstant, curves};
        m.hair = curves && BoolPrimvar(sceneDelegate, id, "eldr:hair", true);
        HdRenderDelegate *delegate = sceneDelegate->GetRenderIndex().GetRenderDelegate();
        const auto &t2 = *HdVkRenderSettingsTokens2;
        if constexpr (curves) {
            const Primvar<GfVec3f> normals(sceneDelegate, id, HdTokens->normals);
            // Curves with normals are ribbons already, oriented by them.
            const bool facing = normals.values.empty() &&
                                BoolPrimvar(sceneDelegate, id, "eldr:ribbons",
                                            delegate->GetRenderSetting<bool>(t2.curveRibbons, kHdVkCurveRibbons));
            m.facing = facing ? vkr::Mesh::kFacingRibbon : vkr::Mesh::kNotFacing;
            Curves(this->GetBasisCurvesTopology(sceneDelegate), points, widths, colors, normals, color, facing, out);
        } else {
            const bool facing = BoolPrimvar(sceneDelegate, id, "eldr:sprites",
                                            delegate->GetRenderSetting<bool>(t2.pointSprites, kHdVkPointSprites));
            m.facing = facing ? vkr::Mesh::kFacingDisc : vkr::Mesh::kNotFacing;
            Spheres(points, widths, colors, color, facing, out);
        }
        m.geometryDirty = true;
    }

    // As HdVkMesh: instancers are synced by the rprims that use them.
    this->_UpdateInstancer(sceneDelegate, dirtyBits);
    HdInstancer::_SyncInstancerAndParents(sceneDelegate->GetRenderIndex(), this->GetInstancerId());
    if (HdChangeTracker::IsTransformDirty(*dirtyBits, id) ||
        HdChangeTracker::IsInstancerDirty(*dirtyBits, id) ||
        HdChangeTracker::IsInstanceIndexDirty(*dirtyBits, id)) {
        HdVkSetTransforms(sceneDelegate, id, this->GetInstancerId(), m);
    }
    if (HdChangeTracker::IsVisibilityDirty(*dirtyBits, id)) {
        this->_UpdateVisibility(sceneDelegate, dirtyBits);
        m.visible = this->IsVisible();
    }
    if (HdChangeTracker::IsAnyPrimvarDirty(*dirtyBits, id)) {
        m.holdout = HdVkHoldout(sceneDelegate, id);
    }
    if (*dirtyBits & HdChangeTracker::DirtyMaterialId) {
        this->SetMaterialId(sceneDelegate->GetMaterialId(id));  // Cryptomatte's material ids
        const auto *material = static_cast<const HdVkMaterial *>(sceneDelegate->GetRenderIndex().GetSprim(
            HdPrimTypeTokens->material, this->GetMaterialId()));
        m.material = material ? material->Get() : nullptr;
    }
    *dirtyBits &= ~HdChangeTracker::AllSceneDirtyBits;
}

template class HdVkTessellated<HdBasisCurves>;
template class HdVkTessellated<HdPoints>;

PXR_NAMESPACE_CLOSE_SCOPE
