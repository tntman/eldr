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
    (in2)
    (in)
    (value)
    (emission)
    (kma_volume));

// A coefficient of a volume material: a constant, a field reader (MaterialX
// geompropvalue, UsdPrimvarReader: the field by its binding name, 1 per
// unit of it), or a product of those (multiply nodes; constant, convert and
// dot nodes pass through). A float field is the density, a vector or colour
// one the colour field. Any other node: `unread` names it.
struct Coefficient {
    GfVec3f value{1.0f};
    std::string density, color;
    std::string unread;
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
        // MaterialX's defaults: in1 0, in2 1.
        const Coefficient a = Read(net, up, _tokens->in1, 0.0f), b = Read(net, up, _tokens->in2, 1.0f);
        c.value = GfCompMult(a.value, b.value);
        c.density = a.density.empty() ? b.density : a.density;
        c.color = a.color.empty() ? b.color : a.color;
        c.unread = a.unread.empty() ? b.unread : a.unread;
    } else if (id.rfind("ND_constant_", 0) == 0) {
        c = Read(net, up, _tokens->value, 0.0f);
    } else if (id.rfind("ND_convert_", 0) == 0 || id.rfind("ND_dot_", 0) == 0) {
        c = Read(net, up, _tokens->in, 0.0f);  // a float splats to all channels
    } else {
        c.unread = id;
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

void Note(std::string *unread, Coefficient const &c)
{
    if (unread->empty()) {
        *unread = c.unread;
    }
}

// The volume's absorption, scattering and anisotropy from `node`'s inputs
// (anisotropic_vdf, absorption_vdf and kma_volume name them alike).
void ReadMedium(HdMaterialNetwork2 const &net, HdMaterialNode2 const &node, bool scatters, vkr::VolumeShading *s,
                std::string *unread)
{
    const Coefficient absorption = Read(net, node, _tokens->absorption, 0.0f);
    const Coefficient scattering = scatters ? Read(net, node, _tokens->scattering, 0.0f) : Coefficient{};
    const Coefficient anisotropy = scatters ? Read(net, node, _tokens->anisotropy, 0.0f) : Coefficient{};
    for (int c = 0; c < 3; ++c) {
        s->absorption[c] = absorption.value[c];
        s->scattering[c] = scatters ? scattering.value[c] : 0.0f;
    }
    const std::string &field = !scattering.density.empty() ? scattering.density : absorption.density;
    if (!field.empty()) {
        s->densityField = field;
    }
    s->colorField = scattering.color;
    s->anisotropy = scatters ? anisotropy.value[0] : 0.0f;
    Note(unread, absorption);
    Note(unread, scattering);
    Note(unread, anisotropy);
}

// The emission of `node`'s `input` (uniform_edf's colour, kma_volume's
// emission): a constant per unit of density, a field other than the
// density times a colour, or blackbody(temperature field times Kelvin)
// times an intensity.
void ReadEmission(HdMaterialNetwork2 const &net, HdMaterialNode2 const &node, TfToken const &input,
                  vkr::VolumeShading *s, std::string *unread)
{
    const HdMaterialNode2 *blackbody = Upstream(net, node, input);
    float intensity = 1.0f;
    if (blackbody && blackbody->nodeTypeId.GetString().rfind("ND_multiply_", 0) == 0) {
        const HdMaterialNode2 *in1 = Upstream(net, *blackbody, _tokens->in1);
        const HdMaterialNode2 *in2 = Upstream(net, *blackbody, _tokens->in2);
        const bool first = in1 && in1->nodeTypeId == _tokens->ND_blackbody;
        if (first || (in2 && in2->nodeTypeId == _tokens->ND_blackbody)) {
            intensity = Read(net, *blackbody, first ? _tokens->in2 : _tokens->in1, 1.0f).value[0];
            blackbody = first ? in1 : in2;
        }
    }
    if (blackbody && blackbody->nodeTypeId == _tokens->ND_blackbody) {
        const Coefficient kelvin = Read(net, *blackbody, _tokens->temperature, 5000.0f);
        s->temperatureField = kelvin.density;
        s->kelvinScale = kelvin.value[0];
        s->blackbodyIntensity = intensity;
        Note(unread, kelvin);
        if (s->temperatureField.empty()) {
            TF_WARN("HdVk: a volume's blackbody emission needs a temperature field");
        }
        return;
    }
    const Coefficient color = Read(net, node, input, 0.0f);
    Note(unread, color);
    const std::string &field = !color.color.empty() ? color.color : color.density;
    if (!field.empty() && field != s->densityField) {
        s->emissionField = field;  // radiance per unit length: the field times the colour
        for (int c = 0; c < 3; ++c) {
            s->emissionTint[c] = color.value[c];
        }
    } else {
        for (int c = 0; c < 3; ++c) {
            s->emission[c] = color.value[c];
        }
    }
}

} // namespace

// The volume terminal (MaterialX's, else Karma's kma context; Storm's
// volumes are glslfx code HdVk can't read): MaterialX's volume node, its
// vdf an anisotropic_vdf (absorption, scattering, anisotropy) or
// absorption_vdf, its edf a uniform_edf; or Karma's kma_volume (the same
// inputs and its emission). Coefficients are per unit of the density field
// (UsdVol: field:density), as MaterialX's are per unit length: constants,
// or a field reader times constants, which also names the density field
// (and a colour field scaling the scattering). Emission: see ReadEmission.
// A graph with any other node isn't read at all: the volume renders as
// without a material (warned), never with a part of the graph.
// ponytail: no other graphs (layered vdfs, ramps, switches: Karma's Pyro
// Shader), and Arnold's standard_volume and RenderMan's PxrVolume live in
// render contexts HdVk doesn't request (asking would switch their surfaces too).
void HdVkReadVolumeShading(HdMaterialNetwork2 const &net, vkr::VolumeShading *v)
{
    const auto terminal = net.terminals.find(HdMaterialTerminalTokens->volume);
    const auto it = terminal == net.terminals.end() ? net.nodes.end() : net.nodes.find(terminal->second.upstreamNode);
    if (it == net.nodes.end()) {
        return;
    }
    const HdMaterialNode2 &volume = it->second;
    vkr::VolumeShading s;  // MaterialX's defaults: no medium, no emission
    s.scattering[0] = s.scattering[1] = s.scattering[2] = 0.0f;
    s.absorption[0] = s.absorption[1] = s.absorption[2] = 0.0f;
    s.anisotropy = 0.0f;
    s.temperatureField = s.colorField = s.emissionField = "";
    s.physical = false;  // MaterialX's blackbody is luminance 1
    s.material = true;
    std::string unread;
    if (volume.nodeTypeId == _tokens->kma_volume) {
        ReadMedium(net, volume, true, &s, &unread);
        ReadEmission(net, volume, _tokens->emission, &s, &unread);
    } else if (volume.nodeTypeId == _tokens->ND_volume) {
        if (const HdMaterialNode2 *vdf = Upstream(net, volume, _tokens->vdf)) {
            const bool anisotropic = vdf->nodeTypeId == _tokens->ND_anisotropic_vdf;
            if (anisotropic || vdf->nodeTypeId == _tokens->ND_absorption_vdf) {
                ReadMedium(net, *vdf, anisotropic, &s, &unread);
            } else {
                unread = vdf->nodeTypeId.GetString();
            }
        }
        if (const HdMaterialNode2 *edf = Upstream(net, volume, _tokens->edf)) {
            if (edf->nodeTypeId == _tokens->ND_uniform_edf) {
                ReadEmission(net, *edf, _tokens->color, &s, &unread);
            } else if (unread.empty()) {
                unread = edf->nodeTypeId.GetString();
            }
        }
    } else {
        unread = volume.nodeTypeId.GetString();
    }
    if (!unread.empty()) {
        TF_WARN("HdVk: a volume material with %s renders as without one (volume graphs read constants, fields, "
                "their products and blackbody)",
                unread.c_str());
        return;
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
    HdVkRenderDelegate::NoteSceneEdit(sceneDelegate);
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
