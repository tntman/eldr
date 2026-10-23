#include "material.h"

#include "scene.h"

#include "pxr/base/gf/color.h"
#include "pxr/base/gf/colorSpace.h"
#include "pxr/base/gf/half.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/base/gf/vec4f.h"
#include "pxr/base/tf/diagnostic.h"
#include "pxr/imaging/hd/sceneDelegate.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/imaging/hio/image.h"
#include "pxr/usd/ar/resolver.h"
#include "pxr/usd/sdf/assetPath.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <set>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

TF_DEFINE_PRIVATE_TOKENS(
    _tokens,
    (UsdPreviewSurface)
    (UsdUVTexture)
    (UsdPrimvarReader_float3)
    (ND_UsdPreviewSurface_surfaceshader)
    (ND_standard_surface_surfaceshader)
    (ND_open_pbr_surface_surfaceshader)
    (base)
    (base_color)
    (base_weight)
    (base_metalness)
    (emission)
    (emission_color)
    (emission_luminance)
    (metalness)
    (specular_roughness)
    (geometry_normal)
    (geometry_opacity)
    (opacity)
    (opacityThreshold)
    (geomprop)
    (in)
    (index)
    ((colorSpaceFile, "colorSpace:file"))
    (diffuseColor)
    (emissiveColor)
    (metallic)
    (roughness)
    (occlusion)
    (normal)
    (file)
    (scale)
    (bias)
    (varname)
    (displayColor)
    (r)(g)(b)(a)
);

// Expands `channels`-wide texels to RGBA, filling missing channels the way
// UsdUVTexture reads them: gray -> rgb, missing alpha -> one.
template <typename T>
void ExpandToRgba(const T *src, size_t texels, int channels, T one, T *dst)
{
    for (size_t i = 0; i < texels; ++i, src += channels, dst += 4) {
        const T gray = src[0];
        dst[0] = gray;
        dst[1] = channels >= 3 ? src[1] : gray;
        dst[2] = channels >= 3 ? src[2] : gray;
        dst[3] = channels == 2 ? src[1] : channels == 4 ? src[3] : one;
    }
}

// Gray and gray+alpha stay one and two channels (the texture view spreads
// the gray), rgb gains an alpha of `one` (no GPU blits rgb formats for mips).
template <typename T>
std::vector<uint8_t> Pack(const T *src, size_t texels, int channels, T one)
{
    std::vector<uint8_t> out(texels * (channels == 3 ? 4 : channels) * sizeof(T));
    if (channels == 3) {
        ExpandToRgba<T>(src, texels, 3, one, reinterpret_cast<T *>(out.data()));
    } else {
        std::memcpy(out.data(), src, out.size());
    }
    return out;
}

// A GfColorSpace name for a colour space name in a file: UsdShade and
// GfColorSpace names as they are, MaterialX's and the OCIO ACES configs'
// common ones mapped. "raw" (and data) is no conversion; empty: none given or
// "auto" (Hio decides: 8-bit sRGB, the rest linear).
TfToken CanonicalColorSpace(std::string name)
{
    static const std::map<std::string, std::string> aliases = {
        {"auto", ""},
        {"srgb", "srgb_rec709_scene"}, {"srgb_texture", "srgb_rec709_scene"},
        {"srgb - texture", "srgb_rec709_scene"}, {"utility - srgb - texture", "srgb_rec709_scene"},
        {"srgb_tx", "srgb_rec709_scene"},
        {"lin_rec709", "lin_rec709_scene"}, {"lin_srgb", "lin_rec709_scene"}, {"linear", "lin_rec709_scene"},
        {"linear rec.709 (srgb)", "lin_rec709_scene"}, {"scene-linear rec.709-srgb", "lin_rec709_scene"},
        {"utility - linear - srgb", "lin_rec709_scene"},
        {"acescg", "lin_ap1_scene"}, {"lin_ap1", "lin_ap1_scene"}, {"aces - acescg", "lin_ap1_scene"},
        {"aces2065-1", "lin_ap0_scene"}, {"lin_ap0", "lin_ap0_scene"}, {"aces - aces2065-1", "lin_ap0_scene"},
        {"utility - raw", "raw"}, {"data", "raw"}, {"identity", "raw"}, {"unknown", "raw"},
    };
    if (GfColorSpace::IsValid(TfToken(name))) {
        return TfToken(name == "data" || name == "identity" || name == "unknown" ? "raw" : name);
    }
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::tolower(c); });
    const auto it = aliases.find(name);
    if (it != aliases.end()) {
        return TfToken(it->second);
    }
    static std::mutex mutex;
    static std::set<std::string> warned;
    std::lock_guard<std::mutex> lock(mutex);
    if (warned.insert(name).second) {
        TF_WARN("HdVk: unknown color space '%s', read as if none was given", name.c_str());
    }
    return TfToken();
}

// The colour space Hydra gives parameter `name` ("colorSpace:<name>": a
// UsdUVTexture's sourceColorSpace, or the UsdColorSpaceAPI's), canonical.
TfToken ParameterColorSpace(std::map<TfToken, VtValue> const &params, TfToken const &name)
{
    const auto it = params.find(TfToken(SdfPath::JoinIdentifier(TfToken("colorSpace"), name)));
    if (it == params.end()) {
        return TfToken();
    }
    return CanonicalColorSpace(it->second.IsHolding<TfToken>() ? it->second.UncheckedGet<TfToken>().GetString()
                               : it->second.IsHolding<std::string>() ? it->second.UncheckedGet<std::string>()
                                                                       : std::string());
}

// HdVk shades in linear Rec.709: whether values in `colorSpace` need
// converting (anything but that, raw and "auto").
bool NeedsConversion(TfToken const &colorSpace)
{
    return !colorSpace.IsEmpty() && colorSpace != GfColorSpaceNames->LinearRec709 && colorSpace != "raw";
}

std::shared_ptr<vkr::Texture> LoadTexture(std::string const &path, TfToken const &colorSpace)
{
    // 8-bit sRGB stays sRGB-encoded (the GPU decodes it); every other colour
    // space but linear Rec.709 is converted to it on the CPU, below.
    const bool srgb = colorSpace == GfColorSpaceNames->SRGBRec709;
    const HioImage::SourceColorSpace cs = srgb ? HioImage::SRGB : colorSpace.IsEmpty() ? HioImage::Auto : HioImage::Raw;
    HioImageSharedPtr image = HioImage::OpenForReading(path, 0, 0, cs);
    if (!image) {
        TF_WARN("HdVk: cannot read texture '%s'", path.c_str());
        return nullptr;
    }
    const HioFormat format = image->GetFormat();
    HioType type = HioGetHioType(format);
    const int channels = HioGetComponentCount(format);
    const int w = image->GetWidth(), h = image->GetHeight();
    size_t componentBytes = 0;
    switch (type) {
    case HioTypeUnsignedByte:
    case HioTypeUnsignedByteSRGB: componentBytes = 1; break;
    case HioTypeUnsignedShort:
    case HioTypeHalfFloat:        componentBytes = 2; break;
    case HioTypeFloat:            componentBytes = 4; break;
    default: break;
    }
    if (w <= 0 || h <= 0 || channels < 1 || channels > 4 || componentBytes == 0) {
        TF_WARN("HdVk: unsupported texture format in '%s'", path.c_str());
        return nullptr;
    }

    std::vector<uint8_t> src(size_t(w) * h * channels * componentBytes);
    HioImage::StorageSpec spec;
    spec.width = w;
    spec.height = h;
    spec.depth = 1;
    spec.format = format;
    spec.flipped = true;  // bottom row first, so st (0,0) is the first texel
    spec.data = src.data();
    if (!image->Read(spec)) {
        TF_WARN("HdVk: failed to decode texture '%s'", path.c_str());
        return nullptr;
    }

    // Other colour spaces: to linear Rec.709 through floats (then stored as
    // floats are, below). Gray stays gray: gamuts share their white.
    // ponytail: gray textures in gamma-encoded spaces aren't decoded.
    const bool convert = NeedsConversion(colorSpace) && channels >= 3 &&
                         !(srgb && (type == HioTypeUnsignedByte || type == HioTypeUnsignedByteSRGB));
    if (convert) {
        const size_t n = size_t(w) * h * channels;
        std::vector<float> f(n);
        for (size_t i = 0; i < n; ++i) {
            switch (type) {
            case HioTypeUnsignedByte:
            case HioTypeUnsignedByteSRGB: f[i] = src[i] / 255.0f; break;
            case HioTypeUnsignedShort: f[i] = reinterpret_cast<const uint16_t *>(src.data())[i] / 65535.0f; break;
            case HioTypeHalfFloat: {
                GfHalf hv;
                hv.setBits(reinterpret_cast<const uint16_t *>(src.data())[i]);
                f[i] = float(hv);
                break;
            }
            default: f[i] = reinterpret_cast<const float *>(src.data())[i]; break;
            }
        }
        // Convert*Span convert from the object's space to the argument's,
        // whatever their documentation says (gf/colorSpace.cpp, 26.08).
        const GfColorSpace linear(GfColorSpaceNames->LinearRec709), source(colorSpace);
        if (channels == 4) {
            source.ConvertRGBASpan(linear, TfSpan<float>(f));
        } else {
            source.ConvertRGBSpan(linear, TfSpan<float>(f));
        }
        src.resize(n * sizeof(float));
        std::memcpy(src.data(), f.data(), src.size());
        type = HioTypeFloat;
    }

    // Stored as compactly as the GPU samples them: 8 and 16 bit and half stay
    // as they are, floats become halves unless they hold values past the
    // half range (bright HDR suns keep their energy). 8-bit sRGB stays RGBA:
    // one- and two-channel sRGB formats aren't universally supported.
    using F = vkr::Texture::Format;
    auto tex = std::make_shared<vkr::Texture>();
    tex->width = uint32_t(w);
    tex->height = uint32_t(h);
    const size_t texels = size_t(w) * h;
    auto byChannels = [&](F one, F two, F four) { return channels == 1 ? one : channels == 2 ? two : four; };
    std::vector<uint16_t> halves;
    if (type == HioTypeFloat) {
        const auto *f = reinterpret_cast<const float *>(src.data());
        const size_t n = texels * channels;
        if (std::all_of(f, f + n, [](float v) { return !std::isfinite(v) || std::abs(v) <= 65504.0f; })) {
            halves.resize(n);
            for (size_t i = 0; i < n; ++i) {
                halves[i] = GfHalf(std::isfinite(f[i]) ? f[i] : 0.0f).bits();
            }
        }
    }
    switch (type) {
    case HioTypeUnsignedByte:
    case HioTypeUnsignedByteSRGB:
        if (type == HioTypeUnsignedByteSRGB || (image->IsColorSpaceSRGB() && !convert)) {
            tex->format = F::RGBA8_SRGB;
            tex->pixels.resize(texels * 4);
            ExpandToRgba<uint8_t>(src.data(), texels, channels, 255, tex->pixels.data());
        } else {
            tex->format = byChannels(F::R8, F::RG8, F::RGBA8);
            tex->pixels = Pack<uint8_t>(src.data(), texels, channels, 255);
        }
        break;
    case HioTypeUnsignedShort:
        tex->format = byChannels(F::R16, F::RG16, F::RGBA16);
        tex->pixels = Pack<uint16_t>(reinterpret_cast<const uint16_t *>(src.data()), texels, channels, 0xFFFF);
        break;
    case HioTypeHalfFloat:
        tex->format = byChannels(F::R16F, F::RG16F, F::RGBA16F);
        tex->pixels = Pack<uint16_t>(reinterpret_cast<const uint16_t *>(src.data()), texels, channels, 0x3C00);
        break;
    default:  // HioTypeFloat
        if (!halves.empty()) {
            tex->format = byChannels(F::R16F, F::RG16F, F::RGBA16F);
            tex->pixels = Pack<uint16_t>(halves.data(), texels, channels, 0x3C00);
        } else {
            tex->format = F::RGBA32F;
            tex->pixels.resize(texels * 16);
            ExpandToRgba<float>(reinterpret_cast<const float *>(src.data()), texels, channels, 1.0f,
                                reinterpret_cast<float *>(tex->pixels.data()));
        }
        break;
    }
    return tex;
}

template <typename T>
bool Read(std::map<TfToken, VtValue> const &params, TfToken const &name, T *out)
{
    const auto it = params.find(name);
    if (it == params.end()) {
        return false;
    }
    const VtValue v = VtValue::Cast<T>(it->second);
    if (v.IsEmpty()) {
        return false;
    }
    *out = v.UncheckedGet<T>();
    return true;
}

std::string AssetPath(VtValue const &v)
{
    if (v.IsHolding<SdfAssetPath>()) {
        const SdfAssetPath &p = v.UncheckedGet<SdfAssetPath>();
        return p.GetResolvedPath().empty() ? p.GetAssetPath() : p.GetResolvedPath();
    }
    return v.IsHolding<std::string>() ? v.UncheckedGet<std::string>() : std::string();
}

// Where a surface shader keeps what HdVk reads (empty: not there), and the
// shader's defaults for it: Hydra passes only authored inputs. Base colour
// and emission are colour times weight.
struct SurfaceInputs {
    TfToken id;
    TfToken baseColor, baseWeight, emission, emissionWeight, metallic, roughness, occlusion, normal, opacity;
    float baseColorDefault, baseWeightDefault, emissionDefault, emissionWeightDefault, roughnessDefault;
};

const std::vector<SurfaceInputs> &Surfaces()
{
    static const std::vector<SurfaceInputs> surfaces = [] {
        const SurfaceInputs preview{_tokens->UsdPreviewSurface, _tokens->diffuseColor, TfToken(),
                                    _tokens->emissiveColor, TfToken(), _tokens->metallic, _tokens->roughness,
                                    _tokens->occlusion, _tokens->normal, _tokens->opacity, 0.18f, 1.0f, 0.0f,
                                    1.0f, 0.5f};
        SurfaceInputs mtlxPreview = preview;
        mtlxPreview.id = _tokens->ND_UsdPreviewSurface_surfaceshader;
        return std::vector<SurfaceInputs>{
            preview,
            mtlxPreview,
            {_tokens->ND_standard_surface_surfaceshader, _tokens->base_color, _tokens->base,
             _tokens->emission_color, _tokens->emission, _tokens->metalness, _tokens->specular_roughness,
             TfToken(), _tokens->normal, _tokens->opacity, 1.0f, 0.8f, 1.0f, 0.0f, 0.2f},
            // emission_luminance is in nits, the radiance unit UsdLux uses too.
            {_tokens->ND_open_pbr_surface_surfaceshader, _tokens->base_color, _tokens->base_weight,
             _tokens->emission_color, _tokens->emission_luminance, _tokens->base_metalness,
             _tokens->specular_roughness, TfToken(), _tokens->geometry_normal, _tokens->geometry_opacity, 0.8f,
             1.0f, 1.0f, 0.0f, 0.3f},
        };
    }();
    return surfaces;
}

bool StartsWith(TfToken const &t, const char *prefix)
{
    return t.GetString().rfind(prefix, 0) == 0;
}

// Follows the connection of `name` to a texture, through MaterialX nodes
// that only convert, extract a channel or decode a normal map. A primvar
// reader of displayColor on the base colour makes the mesh's colour the base.
// ponytail: texture coordinates are always "st"; UsdTransform2d, MaterialX
// texcoord/uvtiling and image defaults are ignored.
void ReadTexture(HdMaterialNetwork2 const &net, HdMaterialNode2 const *node, TfToken name,
                 HdVkTextureCache *textures, vkr::Material *m, vkr::TextureRef *ref)
{
    int channel = -1;
    bool normalMap = false;
    for (int depth = 0; depth < 8; ++depth) {
        const auto conn = node->inputConnections.find(name);
        if (conn == node->inputConnections.end() || conn->second.empty()) {
            return;
        }
        const HdMaterialConnection2 &c = conn->second[0];
        const auto up = net.nodes.find(c.upstreamNode);
        if (up == net.nodes.end()) {
            return;
        }
        node = &up->second;
        const TfToken &type = node->nodeTypeId;
        name = _tokens->in;
        if (StartsWith(type, "ND_convert_") || StartsWith(type, "ND_dot_")) {
            continue;
        }
        if (StartsWith(type, "ND_normalmap")) {
            normalMap = true;
            continue;
        }
        if (StartsWith(type, "ND_extract_")) {
            Read(node->parameters, _tokens->index, &channel);
            continue;
        }
        if (type == _tokens->UsdPrimvarReader_float3 || StartsWith(type, "ND_geompropvalue_")) {
            TfToken varname;
            std::string varnameStr;
            const TfToken &param = type == _tokens->UsdPrimvarReader_float3 ? _tokens->varname : _tokens->geomprop;
            if (ref == &m->baseColorTex &&
                ((Read(node->parameters, param, &varname) && varname == _tokens->displayColor) ||
                 (Read(node->parameters, param, &varnameStr) && varnameStr == "displayColor"))) {
                m->baseColorFromDisplayColor = true;
            }
            return;
        }
        const bool preview = type == _tokens->UsdUVTexture;
        if (!preview && !StartsWith(type, "ND_image_") && !StartsWith(type, "ND_tiledimage_")) {
            return;
        }
        const auto file = node->parameters.find(_tokens->file);
        const std::string path = file == node->parameters.end() ? std::string() : AssetPath(file->second);
        if (path.empty()) {
            return;
        }
        // A MaterialX file without a colour space is in the document's
        // working space, linear (as MaterialX and Storm's MaterialX path read
        // it); a UsdUVTexture's is "auto".
        TfToken colorSpace = ParameterColorSpace(node->parameters, _tokens->file);
        if (colorSpace.IsEmpty() && !preview) {
            colorSpace = GfColorSpaceNames->LinearRec709;
        }
        if (path.find("<UDIM>") != std::string::npos) {
            ref->udim = textures->GetUdim(path, colorSpace);
            const auto tile = std::find_if(ref->udim.begin(), ref->udim.end(), [](auto const &t) { return t; });
            ref->texture = tile == ref->udim.end() ? nullptr : *tile;
        } else {
            ref->texture = textures->Get(path, colorSpace);
        }
        if (preview) {
            const TfToken &out = c.upstreamOutputName;
            channel = out == _tokens->r ? 0 : out == _tokens->g ? 1 : out == _tokens->b ? 2 : out == _tokens->a ? 3 : -1;
            GfVec4f v;
            if (Read(node->parameters, _tokens->scale, &v)) {
                std::copy(v.data(), v.data() + 4, ref->scale);
            }
            if (Read(node->parameters, _tokens->bias, &v)) {
                std::copy(v.data(), v.data() + 4, ref->bias);
            }
        } else if (normalMap) {
            std::fill(ref->scale, ref->scale + 3, 2.0f);
            std::fill(ref->bias, ref->bias + 3, -1.0f);
        }
        ref->channel = channel;
        return;
    }
}

// ponytail: metallic workflow only. Ignored for now: specular workflow,
// coat, sheen, transmission, subsurface, ior and displacement.
void ReadSurface(HdMaterialNetwork2 const &net, HdVkTextureCache *textures, vkr::Material *m)
{
    const auto terminal = net.terminals.find(HdMaterialTerminalTokens->surface);
    if (terminal == net.terminals.end()) {
        return;
    }
    const auto surfaceIt = net.nodes.find(terminal->second.upstreamNode);
    if (surfaceIt == net.nodes.end()) {
        return;
    }
    const HdMaterialNode2 &surface = surfaceIt->second;
    const auto kind = std::find_if(Surfaces().begin(), Surfaces().end(),
                                   [&](SurfaceInputs const &k) { return k.id == surface.nodeTypeId; });
    if (kind == Surfaces().end()) {
        return;
    }

    // One input: its constant (authored, else the default) times the weight,
    // or a texture whose scale and bias carry the weight.
    auto input = [&](TfToken const &name, TfToken const &weightName, float weightDefault, float valueDefault,
                     float *constant, int size, vkr::TextureRef *ref) {
        if (name.IsEmpty()) {
            return;
        }
        float weight = weightDefault;
        if (!weightName.IsEmpty()) {
            Read(surface.parameters, weightName, &weight);
        }
        std::fill(constant, constant + size, valueDefault);
        GfVec3f v;
        if (size == 1) {
            // A colour read as a scalar (standard_surface's opacity): its
            // first channel.
            if (!Read(surface.parameters, name, constant) && Read(surface.parameters, name, &v)) {
                constant[0] = v[0];
            }
        } else if (Read(surface.parameters, name, &v)) {
            const TfToken colorSpace = ParameterColorSpace(surface.parameters, name);
            if (NeedsConversion(colorSpace)) {
                v = GfColor(GfColor(v, GfColorSpace(colorSpace)), GfColorSpace(GfColorSpaceNames->LinearRec709))
                        .GetRGB();
            }
            std::copy(v.data(), v.data() + 3, constant);
        }
        for (int i = 0; i < size; ++i) {
            constant[i] *= weight;
        }
        ReadTexture(net, &surface, name, textures, m, ref);
        for (int i = 0; ref->texture && i < 3; ++i) {
            ref->scale[i] *= weight;
            ref->bias[i] *= weight;
        }
    };

    input(kind->baseColor, kind->baseWeight, kind->baseWeightDefault, kind->baseColorDefault, m->baseColor, 3,
          &m->baseColorTex);
    input(kind->emission, kind->emissionWeight, kind->emissionWeightDefault, kind->emissionDefault, m->emissive,
          3, &m->emissiveTex);
    input(kind->metallic, TfToken(), 1.0f, 0.0f, &m->metallic, 1, &m->metallicTex);
    input(kind->roughness, TfToken(), 1.0f, kind->roughnessDefault, &m->roughness, 1, &m->roughnessTex);
    input(kind->occlusion, TfToken(), 1.0f, 1.0f, &m->occlusion, 1, &m->occlusionTex);
    float unusedNormal[3];
    input(kind->normal, TfToken(), 1.0f, 0.0f, unusedNormal, 3, &m->normalTex);
    // standard_surface's opacity is a colour: its constant stays 1, a
    // texture is read through its first channel.
    input(kind->opacity, TfToken(), 1.0f, 1.0f, &m->opacity, 1, &m->opacityTex);
    if (!Read(surface.parameters, _tokens->opacityThreshold, &m->opacityThreshold) && m->opacityTex.texture &&
        StartsWith(surface.nodeTypeId, "ND_")) {
        // MaterialX has no threshold: an opacity texture is a cutout, as
        // realtime engines draw foliage; constant opacity stays translucent.
        m->opacityThreshold = 0.5f;
    }
}

} // namespace

std::shared_ptr<vkr::Texture> HdVkTextureCache::Get(std::string const &path,
                                                    TfToken const &sourceColorSpace)
{
    std::lock_guard<std::mutex> lock(_mutex);
    std::weak_ptr<vkr::Texture> &slot = _textures[{path, sourceColorSpace}];
    std::shared_ptr<vkr::Texture> texture = slot.lock();
    if (!texture) {
        texture = LoadTexture(path, sourceColorSpace);
        slot = texture;
    }
    return texture;
}

// As Storm: the tiles 1001..1100 the resolver finds (it also looks into
// packages such as usdz).
std::vector<std::shared_ptr<vkr::Texture>> HdVkTextureCache::GetUdim(std::string const &path,
                                                                     TfToken const &sourceColorSpace)
{
    const size_t at = path.find("<UDIM>");
    std::vector<std::shared_ptr<vkr::Texture>> tiles(vkr::kUdimTiles);
    for (size_t i = 0; i < tiles.size(); ++i) {
        const ArResolvedPath tile = ArGetResolver().Resolve(path.substr(0, at) + std::to_string(1001 + i) +
                                                            path.substr(at + 6));
        if (!tile.empty()) {
            tiles[i] = Get(tile.GetPathString(), sourceColorSpace);
        }
    }
    return tiles;
}

HdVkMaterial::HdVkMaterial(SdfPath const &id, HdVkTextureCache *textures)
    : HdMaterial(id)
    , _textures(textures)
    , _material(std::make_shared<vkr::Material>())
{
}

void HdVkMaterial::Sync(HdSceneDelegate *sceneDelegate, HdRenderParam *,
                        HdDirtyBits *dirtyBits)
{
    if (*dirtyBits & (DirtyResource | DirtyParams)) {
        vkr::Material m;  // UsdPreviewSurface defaults
        const VtValue resource = sceneDelegate->GetMaterialResource(GetId());
        if (resource.IsHolding<HdMaterialNetworkMap>()) {
            ReadSurface(HdConvertToHdMaterialNetwork2(resource.UncheckedGet<HdMaterialNetworkMap>()),
                        _textures, &m);
        }
        *_material = std::move(m);
    }
    *dirtyBits = Clean;
}

PXR_NAMESPACE_CLOSE_SCOPE
