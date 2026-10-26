#include "instancer.h"
#include "renderDelegate.h"
#include "scene.h"

#include "pxr/base/gf/matrix4d.h"
#include "pxr/base/gf/matrix4f.h"
#include "pxr/base/gf/quatd.h"
#include "pxr/base/gf/quatf.h"
#include "pxr/base/gf/quath.h"
#include "pxr/base/gf/vec2d.h"
#include "pxr/base/gf/vec2f.h"
#include "pxr/base/gf/vec2h.h"
#include "pxr/base/gf/vec2i.h"
#include "pxr/base/gf/vec3d.h"
#include "pxr/base/gf/vec3h.h"
#include "pxr/base/gf/vec3i.h"
#include "pxr/base/gf/vec4d.h"
#include "pxr/base/gf/vec4f.h"
#include "pxr/base/gf/vec4h.h"
#include "pxr/base/gf/vec4i.h"
#include "pxr/imaging/hd/renderIndex.h"
#include "pxr/imaging/hd/sceneDelegate.h"
#include "pxr/imaging/hd/timeSampleArray.h"
#include "pxr/imaging/hd/tokens.h"

#include <type_traits>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

template <typename T>
void AppendFloats(T const &x, std::vector<float> *out)
{
    if constexpr (std::is_arithmetic_v<T> || std::is_same_v<T, GfHalf>) {
        out->push_back(float(x));
    } else {
        for (size_t k = 0; k < T::dimension; ++k) {
            out->push_back(float(x[k]));
        }
    }
}

template <typename T>
bool TryFloats(VtValue const &v, std::vector<float> *out, uint32_t *components)
{
    if constexpr (std::is_arithmetic_v<T> || std::is_same_v<T, GfHalf>) {
        *components = 1;
    } else {
        *components = uint32_t(T::dimension);
    }
    if (v.IsHolding<VtArray<T>>()) {
        for (T const &x : v.UncheckedGet<VtArray<T>>()) {
            AppendFloats(x, out);
        }
        return true;
    }
    if (v.IsHolding<T>()) {
        AppendFloats(v.UncheckedGet<T>(), out);
        return true;
    }
    return false;
}

template <typename T>
bool CastArray(VtValue const &v, VtArray<T> *out)
{
    const VtValue c = VtValue::Cast<VtArray<T>>(v);
    if (c.IsEmpty()) {
        return false;
    }
    *out = c.UncheckedGet<VtArray<T>>();
    return true;
}

// Orientations arrive as half/float/double quaternions (or legacy vec4 with
// the real part first).
std::vector<GfQuatd> Rotations(VtValue const &v)
{
    std::vector<GfQuatd> out;
    auto add = [&](auto const &arr) {
        for (auto const &q : arr) {
            out.emplace_back(q.GetReal(), GfVec3d(q.GetImaginary()));
        }
    };
    if (v.IsHolding<VtQuathArray>()) {
        add(v.UncheckedGet<VtQuathArray>());
    } else if (v.IsHolding<VtQuatfArray>()) {
        add(v.UncheckedGet<VtQuatfArray>());
    } else if (v.IsHolding<VtQuatdArray>()) {
        add(v.UncheckedGet<VtQuatdArray>());
    } else if (v.IsHolding<VtVec4fArray>()) {
        for (GfVec4f const &q : v.UncheckedGet<VtVec4fArray>()) {
            out.emplace_back(q[0], GfVec3d(q[1], q[2], q[3]));
        }
    }
    return out;
}

} // namespace

HdVkInstancer::HdVkInstancer(HdSceneDelegate *delegate, SdfPath const &id)
    : HdInstancer(delegate, id)
{
}

void HdVkInstancer::Sync(HdSceneDelegate *delegate, HdRenderParam *, HdDirtyBits *dirtyBits)
{
    HdVkRenderDelegate::NoteSceneEdit(delegate);
    const SdfPath &id = GetId();
    if (*dirtyBits & HdChangeTracker::DirtyVisibility) {
        _visible = delegate->GetVisible(id);
    }
    _UpdateInstancer(delegate, dirtyBits);
    if (HdChangeTracker::IsAnyPrimvarDirty(*dirtyBits, id)) {
        for (HdPrimvarDescriptor const &pv :
             delegate->GetPrimvarDescriptors(id, HdInterpolationInstance)) {
            if (HdChangeTracker::IsPrimvarDirty(*dirtyBits, id, pv.name)) {
                _primvars[pv.name] = delegate->Get(id, pv.name);
            }
        }
    }
}

// Same composition order as hdEmbree: instancer transform, then translate,
// rotate, scale and per-instance transform applied on top (row-vector).
// Instance indices past the end of a primvar array leave that term out.
VtMatrix4dArray HdVkInstancer::ComputeInstanceTransforms(SdfPath const &prototypeId, const HdVkShutterTime *at)
{
    if (!_visible) {
        return {};
    }
    HdSceneDelegate *delegate = GetDelegate();
    GfMatrix4d instancerTransform = delegate->GetInstancerTransform(GetId());
    if (at) {
        HdTimeSampleArray<GfMatrix4d, 4> samples;
        delegate->SampleInstancerTransform(GetId(), at->open, at->close, &samples);
        instancerTransform = samples.count > 0 ? samples.Resample(at->time) : instancerTransform;
    }
    const VtIntArray indices = delegate->GetInstanceIndices(GetId(), prototypeId);
    VtMatrix4dArray transforms(indices.size(), instancerTransform);

    std::map<TfToken, VtValue> sampled;  // at `at`
    auto primvar = [&](TfToken const &name) -> VtValue const * {
        const auto it = _primvars.find(name);
        if (it == _primvars.end()) {
            return nullptr;
        }
        if (at) {
            HdTimeSampleArray<VtValue, 4> samples;
            delegate->SamplePrimvar(GetId(), name, at->open, at->close, &samples);
            if (samples.count > 0) {
                VtValue &v = sampled[name] = samples.Resample(at->time);
                return v.IsEmpty() ? &it->second : &v;
            }
        }
        return &it->second;
    };
    auto valid = [](int i, size_t size) { return i >= 0 && size_t(i) < size; };

    VtVec3fArray vec;
    if (VtValue const *v = primvar(HdInstancerTokens->instanceTranslations); v && CastArray(*v, &vec)) {
        for (size_t i = 0; i < indices.size(); ++i) {
            if (valid(indices[i], vec.size())) {
                GfMatrix4d m(1);
                m.SetTranslate(GfVec3d(vec[indices[i]]));
                transforms[i] = m * transforms[i];
            }
        }
    }
    if (VtValue const *v = primvar(HdInstancerTokens->instanceRotations)) {
        const std::vector<GfQuatd> rotations = Rotations(*v);
        for (size_t i = 0; i < indices.size(); ++i) {
            if (valid(indices[i], rotations.size())) {
                GfMatrix4d m(1);
                m.SetRotate(rotations[indices[i]]);
                transforms[i] = m * transforms[i];
            }
        }
    }
    if (VtValue const *v = primvar(HdInstancerTokens->instanceScales); v && CastArray(*v, &vec)) {
        for (size_t i = 0; i < indices.size(); ++i) {
            if (valid(indices[i], vec.size())) {
                GfMatrix4d m(1);
                m.SetScale(GfVec3d(vec[indices[i]]));
                transforms[i] = m * transforms[i];
            }
        }
    }
    VtMatrix4dArray matrices;
    if (VtValue const *v = primvar(HdInstancerTokens->instanceTransforms); v && CastArray(*v, &matrices)) {
        for (size_t i = 0; i < indices.size(); ++i) {
            if (valid(indices[i], matrices.size())) {
                transforms[i] = matrices[indices[i]] * transforms[i];
            }
        }
    }

    if (GetParentId().IsEmpty()) {
        return transforms;
    }
    auto *parent = static_cast<HdVkInstancer *>(delegate->GetRenderIndex().GetInstancer(GetParentId()));
    if (!TF_VERIFY(parent)) {
        return transforms;
    }
    const VtMatrix4dArray parentTransforms = parent->ComputeInstanceTransforms(GetId(), at);
    VtMatrix4dArray result(parentTransforms.size() * transforms.size());
    for (size_t i = 0; i < parentTransforms.size(); ++i) {
        for (size_t j = 0; j < transforms.size(); ++j) {
            result[i * transforms.size() + j] = transforms[j] * parentTransforms[i];
        }
    }
    return result;
}

bool HdVkInstancer::ComputeInstancePrimvar(SdfPath const &prototypeId, TfToken const &name,
                                           std::vector<float> *values, uint32_t *components)
{
    HdSceneDelegate *delegate = GetDelegate();
    const VtIntArray indices = delegate->GetInstanceIndices(GetId(), prototypeId);
    std::vector<float> own, outer;
    uint32_t ownN = 0, outerN = 0;
    const auto it = _primvars.find(name);
    const bool hasOwn = it != _primvars.end() && HdVkToFloats(it->second, &own, &ownN);
    auto *parent = GetParentId().IsEmpty()
                       ? nullptr
                       : static_cast<HdVkInstancer *>(delegate->GetRenderIndex().GetInstancer(GetParentId()));
    const bool hasOuter = parent && parent->ComputeInstancePrimvar(GetId(), name, &outer, &outerN);
    if (!hasOwn && !hasOuter) {
        return false;
    }
    const size_t parents = !parent ? 1 : hasOuter ? outer.size() / outerN
                                                  : delegate->GetInstanceIndices(GetParentId(), GetId()).size();
    *components = hasOwn ? ownN : outerN;
    values->assign(parents * indices.size() * *components, 0.0f);
    for (size_t i = 0; i < parents; ++i) {
        for (size_t j = 0; j < indices.size(); ++j) {
            float *dst = &(*values)[(i * indices.size() + j) * *components];
            if (hasOwn && indices[j] >= 0 && size_t(indices[j]) * ownN + ownN <= own.size()) {
                std::copy_n(&own[size_t(indices[j]) * ownN], ownN, dst);
            } else if (!hasOwn) {
                std::copy_n(&outer[i * outerN], outerN, dst);
            }
        }
    }
    return true;
}

bool HdVkToFloats(VtValue const &v, std::vector<float> *out, uint32_t *components)
{
    out->clear();
    return TryFloats<float>(v, out, components) || TryFloats<double>(v, out, components) ||
           TryFloats<int>(v, out, components) || TryFloats<bool>(v, out, components) ||
           TryFloats<GfHalf>(v, out, components) || TryFloats<GfVec2f>(v, out, components) ||
           TryFloats<GfVec3f>(v, out, components) || TryFloats<GfVec4f>(v, out, components) ||
           TryFloats<GfVec2d>(v, out, components) || TryFloats<GfVec3d>(v, out, components) ||
           TryFloats<GfVec4d>(v, out, components) || TryFloats<GfVec2h>(v, out, components) ||
           TryFloats<GfVec3h>(v, out, components) || TryFloats<GfVec4h>(v, out, components) ||
           TryFloats<GfVec2i>(v, out, components) || TryFloats<GfVec3i>(v, out, components) ||
           TryFloats<GfVec4i>(v, out, components);
}

std::vector<float> HdVkWorldTransforms(HdSceneDelegate *delegate, SdfPath const &id, SdfPath const &instancerId,
                                       const HdVkShutterTime *at)
{
    GfMatrix4d xf = delegate->GetTransform(id);
    if (at) {
        HdTimeSampleArray<GfMatrix4d, 4> samples;
        delegate->SampleTransform(id, at->open, at->close, &samples);
        xf = samples.count > 0 ? samples.Resample(at->time) : xf;
    }
    VtMatrix4dArray instances(1, GfMatrix4d(1));
    if (!instancerId.IsEmpty()) {
        auto *instancer = static_cast<HdVkInstancer *>(delegate->GetRenderIndex().GetInstancer(instancerId));
        instances = instancer ? instancer->ComputeInstanceTransforms(id, at) : VtMatrix4dArray();
    }
    std::vector<float> transforms(instances.size() * 16);
    for (size_t i = 0; i < instances.size(); ++i) {
        const GfMatrix4f world(xf * instances[i]);  // prototype first, then instance
        std::copy(world.data(), world.data() + 16, transforms.data() + i * 16);
    }
    return transforms;
}

void HdVkSetTransforms(HdSceneDelegate *delegate, SdfPath const &id, SdfPath const &instancerId, vkr::Mesh &m)
{
    std::vector<float> transforms = HdVkWorldTransforms(delegate, id, instancerId);
    // Animated skinned prototypes arrive with their instancer dirty every
    // frame; re-upload thousands of instances only when they moved.
    // (New geometry may start where the default transform is: it still
    // needs its motion.)
    if (transforms == m.transforms && !m.geometryDirty) {
        return;
    }
    m.prevTransforms = std::move(m.transforms);  // motion vectors
    m.transforms = std::move(transforms);
    m.transformsDirty = true;
    float open, close;
    auto *vk = static_cast<HdVkRenderDelegate *>(delegate->GetRenderIndex().GetRenderDelegate());
    if (!vk->SampledMotionShutter(&open, &close)) {
        return;
    }
    const HdVkShutterTime atOpen{open, open, close}, atClose{close, open, close};
    const std::vector<float> a = HdVkWorldTransforms(delegate, id, instancerId, &atOpen);
    const std::vector<float> b = HdVkWorldTransforms(delegate, id, instancerId, &atClose);
    if (a == b || a.size() != m.transforms.size() || b.size() != a.size()) {
        return;  // still through the shutter: the motion is since the last transforms
    }
    // ponytail: rigid transforms only; deforming points (and joints) keep
    // last frame's motion.
    m.prevTransforms.resize(a.size());
    for (size_t i = 0; i < a.size(); ++i) {
        m.prevTransforms[i] = m.transforms[i] - (b[i] - a[i]) / (close - open);
    }
    m.shutterMotion = true;
}

PXR_NAMESPACE_CLOSE_SCOPE
