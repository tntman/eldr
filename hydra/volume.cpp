#include "volume.h"

#include "field.h"
#include "instancer.h"
#include "material.h"
#include "renderDelegate.h"
#include "scene.h"

#include "pxr/base/gf/vec3f.h"
#include "pxr/imaging/hd/material.h"
#include "pxr/imaging/hd/renderIndex.h"
#include "pxr/imaging/hd/sceneDelegate.h"
#include "pxr/imaging/hd/tokens.h"

#include <cmath>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

TF_DEFINE_PRIVATE_TOKENS(
    _tokens,
    // MaterialX (pbrlib, stdlib) and UsdPreviewSurface nodes and inputs.
    (ND_volume)
    (ND_anisotropic_vdf)
    (ND_absorption_vdf)
    (ND_uniform_edf)
    (ND_blackbody)
    (vdf)
    (edf)
    (absorption)
    (scattering)
    (anisotropy)
    (color)
    (temperature)
    (geomprop)
    (varname)
    (in1)
    (in2));

// A coefficient of a volume material: a constant, a field reader (MaterialX
// geompropvalue, UsdPrimvarReader: the field by its binding name, 1 per
// unit of it), or a product of those (multiply nodes). A float field is the
// density, a vector or colour one tints (Volume's colour field).
struct Coefficient {
    GfVec3f value{1.0f};
    std::string density, color;
};

Coefficient Read(HdMaterialNetwork2 const &net, HdMaterialNode2 const &node, TfToken const &input, float fallback)
{
    Coefficient c;
    c.value = GfVec3f(fallback);
    const auto conn = node.inputConnections.find(input);
    if (conn == node.inputConnections.end() || conn->second.empty()) {
        const auto p = node.parameters.find(input);
        if (p != node.parameters.end()) {
            if (p->second.IsHolding<GfVec3f>()) {
                c.value = p->second.UncheckedGet<GfVec3f>();
            } else if (p->second.IsHolding<float>()) {
                c.value = GfVec3f(p->second.UncheckedGet<float>());
            } else if (p->second.IsHolding<double>()) {
                c.value = GfVec3f(float(p->second.UncheckedGet<double>()));
            }
        }
        return c;
    }
    const auto it = net.nodes.find(conn->second.front().upstreamNode);
    if (it == net.nodes.end()) {
        return c;
    }
    const HdMaterialNode2 &up = it->second;
    const std::string &id = up.nodeTypeId.GetString();
    c.value = GfVec3f(1.0f);
    if (id.rfind("ND_geompropvalue_", 0) == 0 || id.rfind("UsdPrimvarReader_", 0) == 0) {
        const bool mtlx = id[0] == 'N';
        std::string name;
        const auto p = up.parameters.find(mtlx ? _tokens->geomprop : _tokens->varname);
        if (p != up.parameters.end()) {
            name = p->second.IsHolding<TfToken>() ? p->second.UncheckedGet<TfToken>().GetString()
                   : p->second.IsHolding<std::string>() ? p->second.UncheckedGet<std::string>() : name;
        }
        const bool scalar = id.find("_float") != std::string::npos;
        (scalar ? c.density : c.color) = name;
    } else if (id.rfind("ND_multiply_", 0) == 0) {
        const Coefficient a = Read(net, up, _tokens->in1, 1.0f), b = Read(net, up, _tokens->in2, 1.0f);
        c.value = GfCompMult(a.value, b.value);
        c.density = a.density.empty() ? b.density : a.density;
        c.color = a.color.empty() ? b.color : a.color;
    } else {
        TF_WARN("HdVk: volume materials read constants, fields and their products, not %s", id.c_str());
    }
    return c;
}

HdMaterialNode2 const *Upstream(HdMaterialNetwork2 const &net, HdMaterialNode2 const &node, TfToken const &input)
{
    const auto conn = node.inputConnections.find(input);
    if (conn == node.inputConnections.end() || conn->second.empty()) {
        return nullptr;
    }
    const auto it = net.nodes.find(conn->second.front().upstreamNode);
    return it == net.nodes.end() ? nullptr : &it->second;
}

} // namespace

// MaterialX's volume shading (the standard; Storm's volumes are glslfx code
// HdVk can't read): the volume terminal's volume node, its vdf an
// anisotropic_vdf (absorption, scattering, anisotropy) or absorption_vdf,
// its edf a uniform_edf whose colour is a constant or a blackbody node of a
// temperature (a field times Kelvin per unit). Coefficients are per unit of
// the density field (UsdVol: field:density), as MaterialX's are per unit
// length: constants, or a field reader times constants, which also names
// the density field (and a colour field tinting the scattering).
// ponytail: no other graphs (layered vdfs, textures, ramps), and Arnold's
// standard_volume and RenderMan's PxrVolume live in render contexts HdVk
// doesn't request (asking would switch their surfaces too).
void HdVkReadVolumeShading(HdMaterialNetwork2 const &net, vkr::VolumeShading *v)
{
    const auto terminal = net.terminals.find(HdMaterialTerminalTokens->volume);
    const auto it = terminal == net.terminals.end() ? net.nodes.end() : net.nodes.find(terminal->second.upstreamNode);
    if (it == net.nodes.end() || it->second.nodeTypeId != _tokens->ND_volume) {
        return;
    }
    const HdMaterialNode2 &volume = it->second;
    vkr::VolumeShading s;  // MaterialX's defaults: no medium, no emission
    s.scattering[0] = s.scattering[1] = s.scattering[2] = 0.0f;
    s.absorption[0] = s.absorption[1] = s.absorption[2] = 0.0f;
    s.anisotropy = 0.0f;
    s.temperatureField = s.colorField = s.emissionField = "";
    s.physical = false;  // MaterialX's blackbody is luminance 1
    if (const HdMaterialNode2 *vdf = Upstream(net, volume, _tokens->vdf)) {
        const bool anisotropic = vdf->nodeTypeId == _tokens->ND_anisotropic_vdf;
        if (anisotropic || vdf->nodeTypeId == _tokens->ND_absorption_vdf) {
            const Coefficient absorption = Read(net, *vdf, _tokens->absorption, 0.0f);
            const Coefficient scattering = anisotropic ? Read(net, *vdf, _tokens->scattering, 0.0f) : Coefficient{};
            for (int c = 0; c < 3; ++c) {
                s.absorption[c] = absorption.value[c];
                s.scattering[c] = anisotropic ? scattering.value[c] : 0.0f;
            }
            const std::string &field = !scattering.density.empty() ? scattering.density : absorption.density;
            if (!field.empty()) {
                s.densityField = field;
            }
            s.colorField = scattering.color;
            s.anisotropy = anisotropic ? Read(net, *vdf, _tokens->anisotropy, 0.0f).value[0] : 0.0f;
        } else {
            TF_WARN("HdVk: volume vdf %s isn't read (anisotropic_vdf, absorption_vdf)", vdf->nodeTypeId.GetText());
        }
    }
    if (const HdMaterialNode2 *edf = Upstream(net, volume, _tokens->edf)) {
        if (edf->nodeTypeId == _tokens->ND_uniform_edf) {
            // The colour: a constant (per unit of density, emitted where the
            // medium is), or blackbody(temperature) times constants.
            const HdMaterialNode2 *blackbody = Upstream(net, *edf, _tokens->color);
            float intensity = 1.0f;
            if (blackbody && blackbody->nodeTypeId != _tokens->ND_blackbody &&
                blackbody->nodeTypeId.GetString().rfind("ND_multiply_", 0) == 0) {
                const HdMaterialNode2 *in1 = Upstream(net, *blackbody, _tokens->in1);
                const bool first = in1 && in1->nodeTypeId == _tokens->ND_blackbody;
                intensity = Read(net, *blackbody, first ? _tokens->in2 : _tokens->in1, 1.0f).value[0];
                blackbody = Upstream(net, *blackbody, first ? _tokens->in1 : _tokens->in2);
            }
            if (blackbody && blackbody->nodeTypeId == _tokens->ND_blackbody) {
                const Coefficient kelvin = Read(net, *blackbody, _tokens->temperature, 5000.0f);
                s.temperatureField = kelvin.density;
                s.kelvinScale = kelvin.value[0];
                s.blackbodyIntensity = intensity;
                if (s.temperatureField.empty()) {
                    TF_WARN("HdVk: a volume's blackbody emission needs a temperature field");
                }
            } else {
                const Coefficient color = Read(net, *edf, _tokens->color, 0.0f);
                for (int c = 0; c < 3; ++c) {
                    s.emission[c] = color.value[c];
                }
            }
        } else {
            TF_WARN("HdVk: volume edf %s isn't read (uniform_edf)", edf->nodeTypeId.GetText());
        }
    }
    *v = s;
}

HdVkVolume::HdVkVolume(SdfPath const &id, vkr::Scene *scene)
    : HdVolume(id)
    , _scene(scene)
    , _volume(scene->AddVolume())
{
}

HdVkVolume::~HdVkVolume()
{
    _scene->RemoveVolume(_volume);
}

HdDirtyBits HdVkVolume::GetInitialDirtyBitsMask() const
{
    return HdChangeTracker::InitRepr | HdChangeTracker::DirtyVolumeField | HdChangeTracker::DirtyTransform |
           HdChangeTracker::DirtyVisibility | HdChangeTracker::DirtyInstancer | HdChangeTracker::DirtyInstanceIndex |
           HdChangeTracker::DirtyMaterialId;
}

void HdVkVolume::UpdateRenderTag(HdSceneDelegate *delegate, HdRenderParam *renderParam)
{
    HdVolume::UpdateRenderTag(delegate, renderParam);
    _volume->tag = HdVkRenderTagBit(GetRenderTag());
}

void HdVkVolume::Sync(HdSceneDelegate *sceneDelegate, HdRenderParam *, HdDirtyBits *dirtyBits, TfToken const &)
{
    const SdfPath &id = GetId();
    vkr::Volume &v = *_volume;
    // Fields sync before rprims (bprims first), so their grids are there.
    if (*dirtyBits & HdChangeTracker::DirtyVolumeField) {
        v.fields.clear();
        for (const HdVolumeFieldDescriptor &d : sceneDelegate->GetVolumeFieldDescriptors(id)) {
            // The field types the delegate makes HdVkFields of: openvdbAsset,
            // and houdiniFieldAsset in the Houdini build (no Field3D).
            if (!sceneDelegate->GetRenderIndex().IsBprimTypeSupported(d.fieldPrimType)) {
                continue;
            }
            if (const auto *field = static_cast<const HdVkField *>(
                    sceneDelegate->GetRenderIndex().GetBprim(d.fieldPrimType, d.fieldId))) {
                v.fields[d.fieldName.GetString()] = field->Grid();
            }
        }
    }
    // Materials sync before rprims too; the shading follows theirs in place.
    if (*dirtyBits & HdChangeTracker::DirtyMaterialId) {
        const auto *material = static_cast<const HdVkMaterial *>(sceneDelegate->GetRenderIndex().GetSprim(
            HdPrimTypeTokens->material, sceneDelegate->GetMaterialId(id)));
        v.shading = material ? material->Volume() : nullptr;
    }

    // As HdVkMesh: instancers are synced by the rprims that use them.
    _UpdateInstancer(sceneDelegate, dirtyBits);
    HdInstancer::_SyncInstancerAndParents(sceneDelegate->GetRenderIndex(), GetInstancerId());
    if (HdChangeTracker::IsTransformDirty(*dirtyBits, id) || HdChangeTracker::IsInstancerDirty(*dirtyBits, id) ||
        HdChangeTracker::IsInstanceIndexDirty(*dirtyBits, id)) {
        v.transforms = HdVkWorldTransforms(sceneDelegate, id, GetInstancerId());
    }
    if (HdChangeTracker::IsVisibilityDirty(*dirtyBits, id)) {
        _UpdateVisibility(sceneDelegate, dirtyBits);
        v.visible = IsVisible();
    }
    *dirtyBits &= ~HdChangeTracker::AllSceneDirtyBits;
}

PXR_NAMESPACE_CLOSE_SCOPE
