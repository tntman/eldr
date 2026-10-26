#include "material.h"
#include "renderDelegate.h"

#include "instancer.h"
#include "scene.h"
#ifdef HDVK_VOLUMES
#include "volume.h"
#endif

#include "pxr/base/gf/color.h"
#include "pxr/base/gf/colorSpace.h"
#include "pxr/base/gf/half.h"
#include "pxr/base/gf/matrix3d.h"
#include "pxr/base/gf/matrix3f.h"
#include "pxr/base/gf/matrix4d.h"
#include "pxr/base/gf/matrix4f.h"
#include "pxr/base/gf/vec2f.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/base/gf/vec4f.h"
#include "pxr/base/tf/diagnostic.h"
#include "pxr/imaging/hd/sceneDelegate.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/imaging/hdMtlx/hdMtlx.h"
#include "pxr/imaging/hio/image.h"
#include "pxr/usd/ar/resolver.h"
#include "pxr/usd/sdf/assetPath.h"
#include "pxr/usd/sdr/registry.h"
#include "pxr/usd/sdr/shaderNode.h"
#include "pxr/usd/sdr/shaderProperty.h"

#include <MaterialXCore/Document.h>
#include <MaterialXFormat/Util.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <set>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

namespace mx = MaterialX;

// MaterialX's libraries: USD's (HdMtlxStdLibraries) and, with $HFS set,
// Houdini's own nodes (houdini/materialx/libraries/houdini: hmtlx*, kma_*),
// read as files: no Houdini link. Read-only once built.
mx::DocumentPtr const &MxLibraries()
{
    static const mx::DocumentPtr doc = [] {
        mx::DocumentPtr d = mx::createDocument();
        d->importLibrary(HdMtlxStdLibraries());
        const char *hfs = std::getenv("HFS");
        const mx::FilePath dir = mx::FilePath(hfs ? hfs : "") / "houdini" / "materialx" / "libraries";
        if (hfs && (dir / "houdini").exists()) {
            try {
                mx::loadLibraries({"houdini"}, mx::FileSearchPath(dir), d);  // duplicates are skipped
            } catch (std::exception const &e) {
                TF_WARN("HdVk: can't read Houdini's MaterialX library in '%s': %s", dir.asString().c_str(), e.what());
            }
        }
        return d;
    }();
    return doc;
}

// A node id's MaterialX nodedef: the libraries', else a document's own
// (HdMtlxGetNodeDef finds it through Sdr's implementation URI).
mx::NodeDefPtr MxNodeDef(TfToken const &id)
{
    static std::mutex mutex;
    static std::map<TfToken, mx::NodeDefPtr> cache;
    std::lock_guard<std::mutex> lock(mutex);
    const auto it = cache.find(id);
    if (it != cache.end()) {
        return it->second;
    }
    mx::NodeDefPtr def = MxLibraries()->getNodeDef(id.GetString());
    if (!def && !id.IsEmpty() && SdrRegistry::GetInstance().GetShaderNodeByIdentifier(id)) {
        def = HdMtlxGetNodeDef(id);
    }
    return cache[id] = def;
}

// A nodegraph node's nodedef: its document's, else the libraries' by
// category and types (a document's own nodegraphs may not import them).
mx::NodeDefPtr MxNodeDefOf(mx::NodePtr const &node)
{
    if (mx::NodeDefPtr def = node->getNodeDef()) {
        return def;
    }
    for (const mx::NodeDefPtr &def : MxLibraries()->getMatchingNodeDefs(node->getCategory())) {
        bool match = def->getType() == node->getType();
        for (const mx::InputPtr &in : node->getInputs()) {
            const mx::InputPtr d = def->getActiveInput(in->getName());
            match = match && d && d->getType() == in->getType();
        }
        if (match) {
            return def;
        }
    }
    return nullptr;
}

// A MaterialX value as Hydra holds it (GfVec*f, float, int, bool, string).
VtValue MxValue(mx::ValuePtr const &v)
{
    if (!v) {
        return VtValue();
    }
    if (v->isA<float>()) {
        return VtValue(v->asA<float>());
    }
    if (v->isA<int>()) {
        return VtValue(v->asA<int>());
    }
    if (v->isA<bool>()) {
        return VtValue(v->asA<bool>());
    }
    if (v->isA<mx::Color3>() || v->isA<mx::Vector3>()) {
        const auto c = v->isA<mx::Color3>() ? v->asA<mx::Color3>().data() : v->asA<mx::Vector3>().data();
        return VtValue(GfVec3f(c[0], c[1], c[2]));
    }
    if (v->isA<mx::Color4>() || v->isA<mx::Vector4>()) {
        const auto c = v->isA<mx::Color4>() ? v->asA<mx::Color4>().data() : v->asA<mx::Vector4>().data();
        return VtValue(GfVec4f(c[0], c[1], c[2], c[3]));
    }
    if (v->isA<mx::Vector2>()) {
        return VtValue(GfVec2f(v->asA<mx::Vector2>()[0], v->asA<mx::Vector2>()[1]));
    }
    if (v->isA<std::string>()) {
        return VtValue(v->asA<std::string>());
    }
    return VtValue();
}

TF_DEFINE_PRIVATE_TOKENS(
    _tokens,
    (UsdPreviewSurface)
    (UsdUVTexture)
    (UsdTransform2d)
    (UsdPrimvarReader_float2)
    (UsdPrimvarReader_float3)
    (fallback)
    (wrapS)
    (wrapT)
    (uaddressmode)
    (vaddressmode)
    (specular)
    (specular_color)
    (specular_weight)
    (st)
    (texcoord)
    (rotation)
    (translation)
    (pivot)
    (rotate)
    (offset)
    (operationorder)
    (uvtiling)
    (uvoffset)
    (realworldimagesize)
    (realworldtilesize)
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
    (useSpecularWorkflow)
    (specularColor)
    (clearcoat)
    (clearcoatRoughness)
    ((clearcoatColor, "eldr:clearcoatColor"))
    (ior)
    (coat)
    (coat_color)
    (coat_roughness)
    (coat_IOR)
    (coat_weight)
    (coat_ior)
    (sheen)
    (sheen_color)
    (sheen_roughness)
    (fuzz_weight)
    (fuzz_color)
    (fuzz_roughness)
    (thin_film_thickness)
    (thin_film_IOR)
    (thin_film_weight)
    (thin_film_ior)
    (specular_anisotropy)
    (specular_rotation)
    (specular_roughness_anisotropy)
    ((toonBands, "eldr:toonBands"))
    ((toonSoftness, "eldr:toonSoftness"))
    (transmission)
    (transmission_weight)
    (transmission_color)
    (transmission_depth)
    (specular_IOR)
    (specular_ior)
    (thin_walled)
    (geometry_thin_walled)
    (subsurface)
    (subsurface_weight)
    (subsurface_color)
    (subsurface_radius)
    (subsurface_scale)
    (subsurface_radius_scale)
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
    // Wrap modes in the file (e.g. a .tx's wrapmodes), mapped as Storm's
    // assetUvTextureCpuData.cpp maps them.
    for (int axis = 0; axis < 2; ++axis) {
        HioAddressMode mode;
        if (image->GetSamplerMetadata(axis ? HioAddressDimensionV : HioAddressDimensionU, &mode)) {
            tex->fileWrap[axis] = mode == HioAddressModeClampToEdge    ? vkr::Wrap::Clamp
                                  : mode == HioAddressModeMirrorRepeat ? vkr::Wrap::Mirror
                                  : mode == HioAddressModeClampToBorderColor ? vkr::Wrap::Black
                                                                            : vkr::Wrap::Repeat;
        }
    }
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

// A file input's path, resolved: Hydra's, else the resolver's (nodedef
// defaults inside expanded nodegraphs are plain strings), else as written.
std::string AssetPath(VtValue const &v)
{
    std::string path = v.IsHolding<std::string>() ? v.UncheckedGet<std::string>() : std::string();
    if (v.IsHolding<SdfAssetPath>()) {
        const SdfAssetPath &p = v.UncheckedGet<SdfAssetPath>();
        if (!p.GetResolvedPath().empty()) {
            return p.GetResolvedPath();
        }
        path = p.GetAssetPath();
    }
    const ArResolvedPath resolved = path.empty() || path.find('<') != std::string::npos
                                        ? ArResolvedPath()
                                        : ArGetResolver().Resolve(path);
    return resolved.empty() ? path : resolved.GetPathString();
}

// Where a surface shader keeps what HdVk reads (empty: not there), and the
// shader's defaults for it: Hydra passes only authored inputs. Base colour
// and emission are colour times weight.
struct SurfaceInputs {
    TfToken id;
    TfToken baseColor, baseWeight, emission, emissionWeight, metallic, roughness, occlusion, normal, opacity;
    float baseColorDefault, baseWeightDefault, emissionDefault, emissionWeightDefault, roughnessDefault;
    TfToken specular, specularColor;  // dielectric specular weight and colour (constants only)
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
             TfToken(), _tokens->normal, _tokens->opacity, 1.0f, 0.8f, 1.0f, 0.0f, 0.2f, _tokens->specular,
             _tokens->specular_color},
            // emission_luminance is in nits, the radiance unit UsdLux uses too.
            {_tokens->ND_open_pbr_surface_surfaceshader, _tokens->base_color, _tokens->base_weight,
             _tokens->emission_color, _tokens->emission_luminance, _tokens->base_metalness,
             _tokens->specular_roughness, TfToken(), _tokens->geometry_normal, _tokens->geometry_opacity, 0.8f,
             1.0f, 1.0f, 0.0f, 0.3f, _tokens->specular_weight, _tokens->specular_color},
        };
    }();
    return surfaces;
}

bool StartsWith(TfToken const &t, const char *prefix)
{
    return t.GetString().rfind(prefix, 0) == 0;
}

// m1 * m2 for 2x3 affine matrices (row-major, implicit last row 0 0 1).
void Compose(float *m1, const float *m2)
{
    const float r[6] = {m1[0] * m2[0] + m1[1] * m2[3], m1[0] * m2[1] + m1[1] * m2[4],
                        m1[0] * m2[2] + m1[1] * m2[5] + m1[2], m1[3] * m2[0] + m1[4] * m2[3],
                        m1[3] * m2[1] + m1[4] * m2[4], m1[3] * m2[2] + m1[4] * m2[5] + m1[5]};
    std::copy(r, r + 6, m1);
}

template <typename T>
T ReadOr(std::map<TfToken, VtValue> const &params, TfToken const &name, T fallback)
{
    Read(params, name, &fallback);
    return fallback;
}

// UsdTransform2d: T + R (S uv), R counter-clockwise (Storm's codeGen.cpp).
void Transform2dMatrix(std::map<TfToken, VtValue> const &p, float m[6])
{
    const float r = ReadOr(p, _tokens->rotation, 0.0f) * 0.0174532925f, c = std::cos(r), s = std::sin(r);
    const GfVec2f scale = ReadOr(p, _tokens->scale, GfVec2f(1)), t = ReadOr(p, _tokens->translation, GfVec2f(0));
    const float out[6] = {c * scale[0], -s * scale[1], t[0], s * scale[0], c * scale[1], t[1]};
    std::copy(out, out + 6, m);
}

// MaterialX's NG_place2d: SRT  pivot + R ((uv - pivot) / scale) - offset,
// TRS  pivot + R (uv - pivot - offset) / scale, R by -rotate (mx_rotate_vector2).
void Place2dMatrix(std::map<TfToken, VtValue> const &p, float m[6])
{
    const float r = ReadOr(p, _tokens->rotate, 0.0f) * 0.0174532925f, c = std::cos(r), s = std::sin(r);
    const GfVec2f scale = ReadOr(p, _tokens->scale, GfVec2f(1)), pivot = ReadOr(p, _tokens->pivot, GfVec2f(0)),
                  offset = ReadOr(p, _tokens->offset, GfVec2f(0));
    const float ix = 1.0f / scale[0], iy = 1.0f / scale[1];
    const bool trs = ReadOr(p, _tokens->operationorder, 0) == 1;
    // A = R diag(1/s) (SRT) or diag(1/s) R (TRS), R = (c s; -s c).
    const float a[4] = {trs ? c * ix : c * ix, trs ? s * ix : s * iy, trs ? -s * iy : -s * ix, trs ? c * iy : c * iy};
    const GfVec2f shift = trs ? pivot + offset : pivot;
    const float out[6] = {a[0], a[1], pivot[0] - a[0] * shift[0] - a[1] * shift[1] - (trs ? 0.0f : offset[0]),
                          a[2], a[3], pivot[1] - a[2] * shift[0] - a[3] * shift[1] - (trs ? 0.0f : offset[1])};
    std::copy(out, out + 6, m);
}

// tiledimage: its own tiling after the incoming coordinates
// (NG_tiledimage: (uv uvtiling - uvoffset) realworldtilesize / realworldimagesize).
void TiledImageMatrix(std::map<TfToken, VtValue> const &p, float m[6])
{
    const GfVec2f tiling = ReadOr(p, _tokens->uvtiling, GfVec2f(1)), offset = ReadOr(p, _tokens->uvoffset, GfVec2f(0));
    const GfVec2f k = GfCompDiv(ReadOr(p, _tokens->realworldtilesize, GfVec2f(1)),
                                ReadOr(p, _tokens->realworldimagesize, GfVec2f(1)));
    const float out[6] = {tiling[0] * k[0], 0, -offset[0] * k[0], 0, tiling[1] * k[1], -offset[1] * k[1]};
    std::copy(out, out + 6, m);
}

// A texture node's wrap modes, as Storm reads them (hd/material.cpp,
// hdSt/materialXFilter.cpp): UsdUVTexture wrapS/wrapT, where "useMetadata"
// (also unauthored or unknown) is the file's own, else black; MaterialX
// u/vaddressmode, "periodic" by default.
// ponytail: MaterialX "constant" reads black outside [0, 1] as in Storm, not
// the image's `default`; test uv against [0, 1] in Fetch if one is authored.
void ReadWrap(HdMaterialNode2 const &node, bool preview, vkr::TextureRef *ref)
{
    for (int axis = 0; axis < 2; ++axis) {
        const TfToken &name = preview ? (axis ? _tokens->wrapT : _tokens->wrapS)
                                      : (axis ? _tokens->vaddressmode : _tokens->uaddressmode);
        TfToken token;
        std::string mode;
        if (Read(node.parameters, name, &token)) {
            mode = token.GetString();
        } else {
            Read(node.parameters, name, &mode);
        }
        ref->wrap[axis] = mode == "mirror"                        ? vkr::Wrap::Mirror
                          : mode == "clamp"                       ? vkr::Wrap::Clamp
                          : mode == "black" || mode == "constant" ? vkr::Wrap::Black
                          : !preview || mode == "repeat"          ? vkr::Wrap::Repeat
                          : ref->texture                          ? ref->texture->fileWrap[axis]
                                                                  : vkr::Wrap::Black;
    }
}

// A texture node's file into ref (UDIMs as tiles) with its wrap modes. A
// MaterialX file without a colour space is in the document's working space,
// linear (as MaterialX and Storm's MaterialX path read it); a UsdUVTexture's
// is "auto". False without a file.
bool LoadFile(HdMaterialNode2 const &node, bool preview, HdVkTextureCache *textures, vkr::TextureRef *ref)
{
    const auto file = node.parameters.find(_tokens->file);
    const std::string path = file == node.parameters.end() ? std::string() : AssetPath(file->second);
    if (path.empty()) {
        return false;
    }
    TfToken colorSpace = ParameterColorSpace(node.parameters, _tokens->file);
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
    ReadWrap(node, preview, ref);
    return true;
}

// Follows a texture's coordinates (`input` of `node`) to the primvar they
// come from (a reader's varname; MaterialX texcoord index n -> "st" / "st<n>",
// or geomprop), composing UsdTransform2d and MaterialX place2d into
// ref->uvTransform. Unconnected: "st".
void ReadTexcoord(HdMaterialNetwork2 const &net, HdMaterialNode2 const *node, TfToken input,
                  vkr::TextureRef *ref, TfToken *primvar)
{
    *primvar = _tokens->st;
    for (int depth = 0; depth < 8; ++depth) {
        const auto conn = node->inputConnections.find(input);
        if (conn == node->inputConnections.end() || conn->second.empty()) {
            return;
        }
        const auto up = net.nodes.find(conn->second[0].upstreamNode);
        if (up == net.nodes.end()) {
            return;
        }
        node = &up->second;
        const TfToken &type = node->nodeTypeId;
        auto const &p = node->parameters;
        if (type == _tokens->UsdTransform2d) {
            float m[6];
            Transform2dMatrix(p, m);
            Compose(ref->uvTransform, m);
            input = _tokens->in;
        } else if (StartsWith(type, "ND_place2d")) {
            float m[6];
            Place2dMatrix(p, m);
            Compose(ref->uvTransform, m);
            input = _tokens->texcoord;
        } else if (StartsWith(type, "ND_convert_") || StartsWith(type, "ND_dot_")) {
            input = _tokens->in;
        } else if (type == _tokens->UsdPrimvarReader_float2) {
            TfToken varname;
            std::string varnameStr;
            if (Read(p, _tokens->varname, &varname) && !varname.IsEmpty()) {
                *primvar = varname;
            } else if (Read(p, _tokens->varname, &varnameStr) && !varnameStr.empty()) {
                *primvar = TfToken(varnameStr);
            }
            return;
        } else if (StartsWith(type, "ND_texcoord_")) {
            const int index = ReadOr(p, _tokens->index, 0);
            *primvar = index == 0 ? _tokens->st : TfToken("st" + std::to_string(index));
            return;
        } else if (StartsWith(type, "ND_geompropvalue_")) {
            std::string geomprop;
            if (Read(p, _tokens->geomprop, &geomprop) && !geomprop.empty()) {
                *primvar = TfToken(geomprop);
            }
            return;
        } else {
            return;
        }
    }
}

// Follows the connection of `name` to a texture, through MaterialX nodes
// that only convert, extract a channel or decode a normal map. A primvar
// reader of displayColor on the base colour makes the mesh's colour the base.
// ponytail: MaterialX image defaults are ignored.
void ReadTexture(HdMaterialNetwork2 const &net, HdMaterialNode2 const *node, TfToken name,
                 HdVkTextureCache *textures, vkr::Material *m, vkr::TextureRef *ref, TfToken *primvar)
{
    int channel = -1;
    bool normalMap = false;
    float normalScale = 1.0f;
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
            Read(node->parameters, _tokens->scale, &normalScale);
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
        if (!LoadFile(*node, preview, textures, ref)) {
            return;
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
            // MaterialX normalmap: texel * 2 - 1, xy times its scale.
            const float scale[3] = {2.0f * normalScale, 2.0f * normalScale, 2.0f};
            const float bias[3] = {-normalScale, -normalScale, -1.0f};
            std::copy(scale, scale + 3, ref->scale);
            std::copy(bias, bias + 3, ref->bias);
        }
        ref->channel = channel;
        // tiledimage: its own tiling after the incoming coordinates
        // (NG_tiledimage: (uv uvtiling - uvoffset) realworldtilesize / realworldimagesize).
        if (StartsWith(type, "ND_tiledimage_")) {
            TiledImageMatrix(node->parameters, ref->uvTransform);
        }
        ReadTexcoord(net, node, preview ? _tokens->st : _tokens->texcoord, ref, primvar);
        return;
    }
}

// A shading node's name and output type: MaterialX ND_<name>_<type>...
// ("ND_multiply_color3FA": multiply, color3; ND_convert_<from>_<to>: the
// to type), UsdShade <name>_<type> (UsdPrimvarReader_float3).
struct NodeKind {
    std::string name, type;
};

NodeKind Kind(TfToken const &id)
{
    const std::string &s = id.GetString();
    const bool mtlx = s.rfind("ND_", 0) == 0;
    const size_t start = mtlx ? 3 : 0, end = s.find('_', start);
    NodeKind k{s.substr(start, end == std::string::npos ? std::string::npos : end - start), std::string()};
    if (end != std::string::npos) {
        const std::string rest = s.substr(end + 1);
        k.type = k.name == "convert" ? rest.substr(rest.rfind('_') + 1) : rest.substr(0, rest.find('_'));
        if (k.type.size() > 2 && k.type.compare(k.type.size() - 2, 2, "FA") == 0) {
            k.type.resize(k.type.size() - 2);
        }
    }
    return k;
}

uint32_t Components(std::string const &type)
{
    if (type == "float" || type == "integer" || type == "boolean" || type == "int") {
        return 1;
    }
    if (type == "vector2" || type == "float2") {
        return 2;
    }
    return type == "color4" || type == "vector4" || type == "float4" ? 4 : 3;
}

// A scalar or vector value as a vec4 (scalars splat, missing components 0).
bool ToVec4(VtValue const &v, GfVec4f *out)
{
    std::vector<float> f;
    uint32_t n = 0;
    if (!HdVkToFloats(v, &f, &n) || f.size() != n || n == 0) {
        return false;
    }
    *out = n == 1 ? GfVec4f(f[0]) : GfVec4f(f[0], f[1], n > 2 ? f[2] : 0.0f, n > 3 ? f[3] : 0.0f);
    return true;
}

// Compiles the shading graphs behind material inputs that the texture and
// constant path can't express (MaterialX math, several textures, primvars,
// noise)
// into m->program, mesh.frag's per-pixel interpreter (scene.h). Values are
// an op's result or a constant; ops collect with one virtual register each
// and get the program's kRegisters in Finish.
// A node HdVk can't compile (e.g. hextiledimage) is bypassed as MaterialX
// bypasses nodes: its nodedef output's defaultinput, else its first input of
// the output's type (colorcorrect, contrast: `in`; image: `default`), else
// the output's default value, else 0. Without a nodedef at all the input
// keeps its constant. Each such input counts in fallbackInputs.
class GraphCompiler
{
public:
    using P = vkr::MaterialProgram;

    GraphCompiler(HdMaterialNetwork2 const &net, HdVkTextureCache *textures, P *program, TfToken *uvPrimvar)
        : _net(net)
        , _textures(textures)
        , _p(*program)
        , _uvPrimvar(uvPrimvar)
    {
    }

    // Compiles input `name` of `node`, times `weight`, into output `target`.
    // False, with nothing kept, if a node on the way isn't supported.
    bool Output(HdMaterialNode2 const &node, TfToken const &name, P::Input target, float weight, int channel = -1)
    {
        const size_t ops = _ops.size(), constants = _p.constants.size(), images = _p.images.size(),
                     primvars = _p.primvars.size();
        const auto memo = _memo;
        const auto tainted = _tainted;
        const TfToken uv = *_uvPrimvar;
        const size_t events = _fallbackEvents;
        _failed.clear();
        std::optional<Value> v = _Input(node, name, 0);
        if (!v || _fallbackEvents != events) {
            ++fallbackInputs;
        }
        // A normal is a tangent-space one only out of a normalmap or a
        // texture (UsdPreviewSurface's, decoded by its scale and bias).
        // ponytail: the tangent frame follows the mesh's uv set, whichever
        // set the normal map reads.
        if (v && target == P::kNormal &&
            !(v->op && (_ops[v->index].op == P::kNormalMap || _ops[v->index].op == P::kImage))) {
            v.reset();
        }
        if (v && channel >= 0) {
            v = _Emit(P::kExtract, {*v}, 0, uint32_t(channel));
        }
        if (v && weight != 1.0f) {
            v = _Emit(P::kMultiply, {*v, _Const(GfVec4f(weight))});
        }
        if (!v) {
            _ops.resize(ops);
            _p.constants.resize(constants);
            _p.images.resize(images);
            _p.primvars.resize(primvars);
            _memo = memo;
            _tainted = tainted;
            *_uvPrimvar = uv;
            return false;
        }
        _Emit(P::kOutput, {*v}, target);
        return true;
    }

    // Material inputs compiled so far.
    int Outputs() const
    {
        return int(std::count_if(_ops.begin(), _ops.end(), [](Ins const &i) { return i.op == P::kOutput; }));
    }

    // The node id that made the last Output fail (empty: a malformed graph).
    std::string const &Failed() const { return _failed; }

    // Inputs compiled with a node bypassed or not at all, and those nodes
    // ("name (id)", once each).
    int fallbackInputs = 0;
    std::vector<std::string> fallbackNodes;

    // Gives each value a register from its op until its last reader; false
    // (program left empty) if more than kRegisters values are live at once
    // or the constants don't fit the operand bytes.
    bool Finish()
    {
        _DropDeadOps();
        std::vector<int> last(_ops.size(), -1), reg(_ops.size(), -1);
        for (size_t i = 0; i < _ops.size(); ++i) {
            for (const Value &a : _ops[i].args) {
                if (a.op) {
                    last[a.index] = int(i);
                }
            }
        }
        std::vector<bool> busy(P::kRegisters, false);
        for (size_t i = 0; i < _ops.size(); ++i) {
            const Ins &ins = _ops[i];
            uint8_t operands[4] = {0, 0, 0, 0};
            for (size_t k = 0; k < ins.args.size(); ++k) {
                const Value &a = ins.args[k];
                if (!a.op && P::kRegisters + a.index > 255) {
                    _p.code.clear();
                    return false;
                }
                operands[k] = uint8_t(a.op ? reg[a.index] : P::kRegisters + a.index);
            }
            for (const Value &a : ins.args) {
                if (a.op && last[a.index] == int(i)) {
                    busy[reg[a.index]] = false;
                }
            }
            uint32_t dst = 0;
            if (ins.op != P::kOutput) {
                const auto free = std::find(busy.begin(), busy.end(), false);
                if (free == busy.end()) {
                    _p.code.clear();
                    return false;
                }
                dst = uint32_t(free - busy.begin());
                busy[dst] = last[i] >= 0;  // a dead value frees its register at once
                reg[i] = int(dst);
            }
            _p.Emit(ins.op, dst, {operands[0], operands[1], operands[2], operands[3]}, ins.imm0, ins.imm1);
        }
        return true;
    }

private:
    struct Value {
        bool op;         // an op's result (index into _ops), else constant `index`
        uint32_t index;
    };
    struct Ins {
        P::Op op;
        std::vector<Value> args;
        uint32_t imm0, imm1;
    };

    Value _Emit(P::Op op, std::vector<Value> args, uint32_t imm0 = 0, uint32_t imm1 = 0)
    {
        _ops.push_back({op, std::move(args), imm0, imm1});
        return {true, uint32_t(_ops.size() - 1)};
    }

    // A constant, shared with an equal one already there (operand bytes
    // reach only 256 - kRegisters of them).
    Value _Const(GfVec4f const &v)
    {
        for (size_t i = 0; i + 4 <= _p.constants.size(); i += 4) {
            if (std::equal(v.data(), v.data() + 4, _p.constants.begin() + i)) {
                return {false, uint32_t(i / 4)};
            }
        }
        return _NewConst(v);
    }

    // A constant of its own: after the last one (ramps read them in a row).
    Value _NewConst(GfVec4f const &v)
    {
        _p.constants.insert(_p.constants.end(), v.data(), v.data() + 4);
        return {false, uint32_t(_p.constants.size() / 4 - 1)};
    }

    // Removes ops no output reads (an expanded nodegraph's unused outputs,
    // the parts of a node compiled before it failed and was bypassed).
    void _DropDeadOps()
    {
        std::vector<bool> live(_ops.size(), false);
        for (size_t i = _ops.size(); i-- > 0;) {
            live[i] = live[i] || _ops[i].op == P::kOutput;
            for (const Value &a : _ops[i].args) {
                if (live[i] && a.op) {
                    live[a.index] = true;
                }
            }
        }
        std::vector<uint32_t> index(_ops.size());
        size_t n = 0;
        for (size_t i = 0; i < _ops.size(); ++i) {
            if (live[i]) {
                index[i] = uint32_t(n);
                _ops[n] = std::move(_ops[i]);
                for (Value &a : _ops[n].args) {
                    a.index = a.op ? index[a.index] : a.index;
                }
                ++n;
            }
        }
        _ops.resize(n);
    }

    // Floats packed four to a constant; the first one's index.
    uint32_t _Packed(std::vector<float> values)
    {
        const uint32_t first = uint32_t(_p.constants.size() / 4);
        values.resize((values.size() + 3) / 4 * 4, 0.0f);
        _p.constants.insert(_p.constants.end(), values.begin(), values.end());
        return first;
    }

    // An unconnected input: authored (colours converted to linear Rec.709),
    // else the node definition's default, else zero.
    GfVec4f _Parameter(HdMaterialNode2 const &node, TfToken const &input) const
    {
        GfVec4f v(0.0f);
        const auto it = node.parameters.find(input);
        if (it != node.parameters.end() && ToVec4(it->second, &v)) {
            const TfToken colorSpace = ParameterColorSpace(node.parameters, input);
            if (NeedsConversion(colorSpace)) {
                const GfVec3f rgb = GfColor(GfColor(GfVec3f(v[0], v[1], v[2]), GfColorSpace(colorSpace)),
                                            GfColorSpace(GfColorSpaceNames->LinearRec709))
                                        .GetRGB();
                v = GfVec4f(rgb[0], rgb[1], rgb[2], v[3]);
            }
            return v;
        }
        if (SdrShaderNodeConstPtr def = SdrRegistry::GetInstance().GetShaderNodeByIdentifier(node.nodeTypeId)) {
            if (SdrShaderPropertyConstPtr p = def->GetShaderInput(input)) {
                ToVec4(p->GetDefaultValue(), &v);
            }
        } else if (mx::NodeDefPtr def = MxNodeDef(node.nodeTypeId)) {  // e.g. Houdini's nodes outside Houdini
            if (mx::InputPtr i = def->getActiveInput(input.GetString())) {
                ToVec4(MxValue(i->getValue()), &v);
            }
        }
        return v;
    }

    // A string input (a space's name, a note): authored, else its nodedef's.
    std::string _String(HdMaterialNode2 const &node, const char *input) const
    {
        TfToken token;
        std::string s;
        if (Read(node.parameters, TfToken(input), &token)) {
            return token.GetString();
        }
        if (Read(node.parameters, TfToken(input), &s)) {
            return s;
        }
        const mx::NodeDefPtr def = MxNodeDef(node.nodeTypeId);
        const mx::InputPtr i = def ? def->getActiveInput(input) : nullptr;
        return i ? i->getValueString() : std::string();
    }

    // Input `mat` as `rows` rows (Gf's, MaterialX's order: v * M), else its
    // nodedef's (identity).
    bool _Matrix(HdMaterialNode2 const &node, uint32_t rows, std::vector<GfVec4f> *out) const
    {
        GfMatrix4d m(1.0);
        const auto it = node.parameters.find(TfToken("mat"));
        if (it != node.parameters.end()) {
            const VtValue &v = it->second;
            if (v.IsHolding<GfMatrix3d>() || v.IsHolding<GfMatrix3f>()) {
                const GfMatrix3d m3 = v.IsHolding<GfMatrix3d>() ? v.UncheckedGet<GfMatrix3d>()
                                                                 : GfMatrix3d(v.UncheckedGet<GfMatrix3f>());
                for (int r = 0; r < 3; ++r) {
                    for (int c = 0; c < 3; ++c) {
                        m[r][c] = m3[r][c];
                    }
                }
            } else if (v.IsHolding<GfMatrix4d>() || v.IsHolding<GfMatrix4f>()) {
                m = v.IsHolding<GfMatrix4d>() ? v.UncheckedGet<GfMatrix4d>() : GfMatrix4d(v.UncheckedGet<GfMatrix4f>());
            } else {
                return false;
            }
        }
        for (uint32_t r = 0; r < rows; ++r) {
            out->push_back(rows == 3 ? GfVec4f(m[r][0], m[r][1], m[r][2], 0.0f)
                                     : GfVec4f(m[r][0], m[r][1], m[r][2], m[r][3]));
        }
        return true;
    }

    bool _Connected(HdMaterialNode2 const &node, TfToken const &input) const
    {
        const auto c = node.inputConnections.find(input);
        return c != node.inputConnections.end() && !c->second.empty();
    }

    // An input the program needs at compile time (a ramp's key count, a
    // channel index): its value, or a connected constant node's.
    std::optional<float> _ConstantInput(HdMaterialNode2 const &node, TfToken const &input) const
    {
        const auto c = node.inputConnections.find(input);
        if (c == node.inputConnections.end() || c->second.empty()) {
            return _Parameter(node, input)[0];
        }
        const HdMaterialNode2 *up = _Find(c->second[0].upstreamNode);
        if (!up || Kind(up->nodeTypeId).name != "constant") {
            return std::nullopt;
        }
        return _Parameter(*up, TfToken("value"))[0];
    }

    // A node of the network or of an expanded nodegraph (_Expand).
    HdMaterialNode2 const *_Find(SdfPath const &path) const
    {
        const auto it = _net.nodes.find(path);
        if (it != _net.nodes.end()) {
            return &it->second;
        }
        const auto e = _extra.find(path);
        return e == _extra.end() ? nullptr : &e->second;
    }

    // Unconnected and unauthored: its nodedef's defaultgeomprop (UV0,
    // Nworld, Pobject, ...) if it has one, else its default value.
    std::optional<Value> _Input(HdMaterialNode2 const &node, TfToken const &input, int depth)
    {
        const auto c = node.inputConnections.find(input);
        if (c != node.inputConnections.end() && !c->second.empty()) {
            return _Node(c->second[0].upstreamNode, c->second[0].upstreamOutputName, depth + 1);
        }
        if (!node.parameters.count(input)) {
            if (const mx::NodeDefPtr def = MxNodeDef(node.nodeTypeId)) {
                const mx::InputPtr i = def->getActiveInput(input.GetString());
                if (i && i->hasDefaultGeomPropString()) {
                    return _Node(_GeomProp(i->getDefaultGeomPropString()), TfToken("out"), depth + 1);
                }
            }
        }
        return _Const(_Parameter(node, input));
    }

    // A node reading MaterialX geomprop `name` (stdlib's geomprops), made
    // once; unknown ones read 0.
    SdfPath _GeomProp(std::string const &name)
    {
        const SdfPath path = SdfPath("/__eldr_geomprop").AppendChild(TfToken(TfMakeValidIdentifier(name)));
        if (_extra.count(path)) {
            return path;
        }
        HdMaterialNode2 &n = _extra[path];
        const char space = name.size() < 2 ? 'w' : name[1];  // Nworld, Pobject, ...
        n.parameters[TfToken("space")] = VtValue(std::string(space == 'o' ? "object" : "world"));
        if (name.rfind("UV", 0) == 0) {
            n.nodeTypeId = TfToken("ND_texcoord_vector2");
            n.parameters[_tokens->index] = VtValue(std::atoi(name.c_str() + 2));
        } else {
            static const std::map<char, const char *> kinds = {{'N', "ND_normal_vector3"}, {'P', "ND_position_vector3"},
                                                               {'T', "ND_tangent_vector3"}, {'B', "ND_bitangent_vector3"}};
            const auto k = kinds.find(name.empty() ? ' ' : name[0]);
            n.nodeTypeId = TfToken(k == kinds.end() ? "ND_constant_float" : k->second);
        }
        return path;
    }

    // A node with a nodegraph implementation (MaterialX's *_ng.mtlx, Houdini's
    // library, a document's own nodedefs) as the nodegraph's nodes, made
    // once as Hydra nodes under its path: interface inputs take the node's
    // connections, values or its nodedef's defaults (geomprops included).
    std::optional<Value> _Expand(HdMaterialNode2 const &n, SdfPath const &path, TfToken const &output, int depth)
    {
        const mx::NodeDefPtr def = MxNodeDef(n.nodeTypeId);
        const mx::InterfaceElementPtr impl = def ? def->getImplementation() : nullptr;
        const mx::NodeGraphPtr graph = impl ? impl->asA<mx::NodeGraph>() : nullptr;
        if (!graph) {
            return std::nullopt;
        }
        auto inner = [&](std::string const &name) { return path.AppendChild(TfToken(TfMakeValidIdentifier(name))); };
        if (_expanded.insert(path).second) {
            for (const mx::NodePtr &node : graph->getNodes()) {
                const mx::NodeDefPtr nodeDef = MxNodeDefOf(node);
                HdMaterialNode2 &h = _extra[inner(node->getName())];
                h.nodeTypeId = TfToken(nodeDef ? nodeDef->getName() : node->getCategory());
                for (const mx::InputPtr &in : node->getInputs()) {
                    const TfToken name(in->getName());
                    if (in->hasNodeName()) {
                        const std::string out = in->getOutputString();
                        h.inputConnections[name] = {{inner(in->getNodeName()), TfToken(out.empty() ? "out" : out)}};
                    } else if (in->hasInterfaceName()) {
                        const TfToken outer(in->getInterfaceName());
                        const auto c = n.inputConnections.find(outer);
                        const auto value = n.parameters.find(outer);
                        const mx::InputPtr d = def->getActiveInput(outer.GetString());
                        if (c != n.inputConnections.end() && !c->second.empty()) {
                            h.inputConnections[name] = c->second;
                        } else if (value != n.parameters.end()) {
                            h.parameters[name] = value->second;
                            const TfToken cs(SdfPath::JoinIdentifier(TfToken("colorSpace"), outer));
                            if (n.parameters.count(cs)) {
                                h.parameters[TfToken(SdfPath::JoinIdentifier(TfToken("colorSpace"), name))] =
                                    n.parameters.at(cs);
                            }
                        } else if (d && d->hasDefaultGeomPropString()) {
                            h.inputConnections[name] = {{_GeomProp(d->getDefaultGeomPropString()), TfToken("out")}};
                        } else if (d && d->getValue()) {
                            h.parameters[name] = MxValue(d->getValue());
                        }
                    } else if (in->getValue()) {  // file names with their [tokens] filled in
                        h.parameters[name] = in->getType() == "filename" ? VtValue(in->getResolvedValueString())
                                                                         : MxValue(in->getValue());
                    }
                }
            }
        }
        mx::OutputPtr out = output.IsEmpty() ? nullptr : graph->getOutput(output.GetString());
        if (!out) {
            const std::vector<mx::OutputPtr> outs = graph->getOutputs();
            if (outs.empty()) {
                return std::nullopt;
            }
            out = outs[0];
        }
        if (out->hasInterfaceName()) {
            return _Input(n, TfToken(out->getInterfaceName()), depth);
        }
        if (!out->hasNodeName()) {
            return std::nullopt;
        }
        const std::string o = out->getOutputString();
        return _Node(inner(out->getNodeName()), TfToken(o.empty() ? "out" : o), depth + 1);
    }

    // Texture coordinates: a connected input, else "st".
    std::optional<Value> _Uv(HdMaterialNode2 const &node, TfToken const &input, int depth)
    {
        return _Connected(node, input) ? _Input(node, input, depth) : std::optional<Value>(_Texcoord(_tokens->st));
    }

    // A uv set: the mesh's (kTexcoord) if it is the material's, the first
    // one read, else a primvar like any other (the mesh packs it per
    // vertex), (0, 0) or `fallback` where missing.
    Value _Texcoord(TfToken const &primvar, std::optional<Value> fallback = std::nullopt)
    {
        if (_uvPrimvar->IsEmpty()) {
            *_uvPrimvar = primvar;
        }
        if (primvar == *_uvPrimvar) {
            return _Emit(P::kTexcoord, {});
        }
        return _Primvar(primvar, fallback ? *fallback : _Const(GfVec4f(0.0f)));
    }

    Value _Primvar(TfToken const &name, Value fallback)
    {
        auto slot = std::find(_p.primvars.begin(), _p.primvars.end(), name.GetString());
        if (slot == _p.primvars.end()) {
            slot = _p.primvars.insert(slot, name.GetString());
        }
        return _Emit(P::kPrimvar, {fallback}, uint32_t(slot - _p.primvars.begin()));
    }

    std::optional<Value> _Node(SdfPath const &path, TfToken const &output, int depth)
    {
        const auto key = std::make_pair(path, output);
        const auto memo = _memo.find(key);
        if (memo != _memo.end()) {
            _fallbackEvents += _tainted.count(key);
            return memo->second;
        }
        const HdMaterialNode2 *found = _Find(path);
        if (depth > 256 || !found) {
            return std::nullopt;
        }
        const HdMaterialNode2 &node = *found;
        const size_t events = _fallbackEvents, hard = _hardFailures;
        std::optional<Value> v = _Compile(node, Kind(node.nodeTypeId), output, depth);
        // Failed itself, or an input of it failed with nothing to stand in.
        const bool own = _hardFailures == hard;
        if (!v) {
            v = _Expand(node, path, output, depth);
        }
        if (!v) {
            const std::string &id = node.nodeTypeId.GetString();
            if (_failed.empty()) {
                _failed = id;
            }
            v = _Bypass(node, output, depth);
            ++_fallbackEvents;
            _hardFailures += !v;
            const std::string entry = path.GetName() + " (" + id + ")";
            if (own && std::find(fallbackNodes.begin(), fallbackNodes.end(), entry) == fallbackNodes.end()) {
                fallbackNodes.push_back(entry);
            }
            static std::mutex mutex;
            static std::set<std::string> warned;
            std::lock_guard<std::mutex> lock(mutex);
            if (own && warned.insert(id).second) {
                TF_WARN("HdVk: shading node '%s' isn't supported; %s", id.c_str(),
                        v ? "bypassed (its pass-through input or default stands in)"
                          : "the material input it feeds keeps its constant");
            }
        }
        if (v) {
            _memo[key] = *v;
            if (_fallbackEvents != events) {
                _tainted.insert(key);
            }
        }
        return v;
    }

    // A node bypassed as MaterialX bypasses it (the class comment); none
    // without a nodedef.
    std::optional<Value> _Bypass(HdMaterialNode2 const &n, TfToken const &output, int depth)
    {
        const mx::NodeDefPtr def = MxNodeDef(n.nodeTypeId);
        if (!def) {
            return std::nullopt;
        }
        mx::OutputPtr out = output.IsEmpty() ? nullptr : def->getActiveOutput(output.GetString());
        if (!out) {
            const std::vector<mx::OutputPtr> outs = def->getActiveOutputs();
            if (outs.empty()) {
                return std::nullopt;
            }
            out = outs[0];
        }
        std::string through = out->getAttribute("defaultinput");
        for (const mx::InputPtr &i : def->getActiveInputs()) {
            if (through.empty() && i->getType() == out->getType()) {
                through = i->getName();
            }
        }
        if (!through.empty()) {
            return _Input(n, TfToken(through), depth);
        }
        GfVec4f v(0.0f);
        if (out->hasAttribute("default")) {
            ToVec4(MxValue(mx::Value::createValueFromStrings(out->getAttribute("default"), out->getType())), &v);
        }
        return _Const(v);
    }

    std::optional<Value> _Compile(HdMaterialNode2 const &n, NodeKind const &k, TfToken const &output, int depth)
    {
        auto in = [&](const char *name) { return _Input(n, TfToken(name), depth); };
        // Ops whose operands are inputs, in order.
        static const std::map<std::string, std::pair<P::Op, std::vector<const char *>>> plain = {
            {"add", {P::kAdd, {"in1", "in2"}}},
            {"subtract", {P::kSubtract, {"in1", "in2"}}},
            {"multiply", {P::kMultiply, {"in1", "in2"}}},
            {"divide", {P::kDivide, {"in1", "in2"}}},
            {"min", {P::kMin, {"in1", "in2"}}},
            {"max", {P::kMax, {"in1", "in2"}}},
            {"power", {P::kPower, {"in1", "in2"}}},
            {"modulo", {P::kModulo, {"in1", "in2"}}},
            {"absval", {P::kAbs, {"in"}}},
            {"floor", {P::kFloor, {"in"}}},
            {"ceil", {P::kCeil, {"in"}}},
            {"sin", {P::kSin, {"in"}}},
            {"cos", {P::kCos, {"in"}}},
            {"sqrt", {P::kSqrt, {"in"}}},
            {"exp", {P::kExp, {"in"}}},
            {"ln", {P::kLog, {"in"}}},
            {"invert", {P::kInvert, {"in", "amount"}}},
            {"mix", {P::kMix, {"bg", "fg", "mix"}}},
            {"overlay", {P::kOverlay, {"fg", "bg", "mix"}}},
            {"clamp", {P::kClamp, {"in", "low", "high"}}},
            {"smoothstep", {P::kSmoothstep, {"in", "low", "high"}}},
            {"dotproduct", {P::kDot, {"in1", "in2"}}},
            {"normalize", {P::kNormalize, {"in"}}},
            {"luminance", {P::kLuminance, {"in", "lumacoeffs"}}},
            {"ifgreater", {P::kIfGreater, {"value1", "value2", "in1", "in2"}}},
            {"ifgreatereq", {P::kIfGreaterEq, {"value1", "value2", "in1", "in2"}}},
            {"ifequal", {P::kIfEqual, {"value1", "value2", "in1", "in2"}}},
            {"hsvadjust", {P::kHsvAdjust, {"in", "amount"}}},
            {"rgbtohsv", {P::kRgbToHsv, {"in"}}},
            {"hsvtorgb", {P::kHsvToRgb, {"in"}}},
            {"normalmap", {P::kNormalMap, {"in", "scale"}}},
            {"heighttonormal", {P::kHeightToNormal, {"in", "scale", "texcoord"}}},  // bump's nodegraph
            {"combine2", {P::kCombine, {"in1", "in2"}}},
            {"combine3", {P::kCombine, {"in1", "in2", "in3"}}},
            {"combine4", {P::kCombine, {"in1", "in2", "in3", "in4"}}},
            {"fract", {P::kFract, {"in"}}},
            {"round", {P::kRound, {"in"}}},
            {"sign", {P::kSign, {"in"}}},
            {"tan", {P::kTan, {"in"}}},
            {"asin", {P::kAsin, {"in"}}},
            {"acos", {P::kAcos, {"in"}}},
            {"atan2", {P::kAtan2, {"iny", "inx"}}},
            {"crossproduct", {P::kCross, {"in1", "in2"}}},
            {"rotate2d", {P::kRotate2d, {"in", "amount"}}},
            {"rotate3d", {P::kRotate3d, {"in", "amount", "axis"}}},
        };
        if (const auto it = plain.find(k.name); it != plain.end()) {
            std::vector<Value> args;
            for (const char *name : it->second.second) {
                const std::optional<Value> v = in(name);
                if (!v) {
                    return std::nullopt;
                }
                args.push_back(*v);
            }
            return _Emit(it->second.first, std::move(args));
        }
        if (k.name == "constant") {
            return in("value");  // connected inside expanded nodegraphs (blur's)
        }
        if (k.name == "dot") {
            // Karma's notes on dot (its kma_camerafalloff nodegraph): the ray's
            // direction and the hit normal, world space.
            const std::string note = _String(n, "note");
            if (note == "ray:direction") {
                return _Emit(P::kViewDirection, {}, 1);
            }
            if (note == "ray:hitN") {
                return _Emit(P::kGeomNormal, {}, 1);
            }
        }
        if (k.name == "convert" || k.name == "dot") {
            return in("in");
        }
        if (k.name == "magnitude") {
            const std::optional<Value> v = in("in");
            return v ? std::optional<Value>(_Emit(P::kMagnitude, {*v}, 0, Components(k.type))) : std::nullopt;
        }
        if (k.name == "logical") {  // booleans are 0 and 1
            const std::optional<Value> a = in(k.type == "not" ? "in" : "in1");
            const std::optional<Value> b = k.type == "not" ? std::optional<Value>(_Const(GfVec4f(1.0f))) : in("in2");
            if (!a || !b) {
                return std::nullopt;
            }
            return k.type == "and"   ? _Emit(P::kMin, {*a, *b})
                   : k.type == "or"  ? _Emit(P::kMax, {*a, *b})
                   : k.type == "not" ? _Emit(P::kInvert, {*a, *b})
                                     : _Emit(P::kAbs, {_Emit(P::kSubtract, {*a, *b})});
        }
        if (k.name == "switch") {
            // in1 .. in10 by floor(which), 0 for in1; a constant `which`
            // compiles only the input it picks.
            const std::optional<float> which = _ConstantInput(n, TfToken("which"));
            if (which) {
                return in(("in" + std::to_string(std::clamp(int(std::floor(*which)), 0, 9) + 1)).c_str());
            }
            std::optional<Value> w = in("which"), r = in("in1");
            for (int i = 1; i < 10 && w && r; ++i) {
                const std::optional<Value> v = in(("in" + std::to_string(i + 1)).c_str());
                r = v ? std::optional<Value>(_Emit(P::kIfGreaterEq, {*w, _Const(GfVec4f(float(i))), *v, *r}))
                      : std::nullopt;
            }
            return r;
        }
        if (k.name == "splitlr" || k.name == "splittb") {
            // mix(valuel, valuer, aastep(center, u)), mix(valueb, valuet, aastep(center, v)).
            const bool lr = k.name == "splitlr";
            const std::optional<Value> uv = _Uv(n, _tokens->texcoord, depth), center = in("center");
            const std::optional<Value> from = in(lr ? "valuel" : "valueb"), to = in(lr ? "valuer" : "valuet");
            if (!uv || !center || !from || !to) {
                return std::nullopt;
            }
            const Value s = _Emit(P::kAaStep, {_Emit(P::kExtract, {*uv}, 0, lr ? 0 : 1), *center});
            return _Emit(P::kMix, {*from, *to, s});
        }
        if (k.name == "frame" || k.name == "time") {
            return _Emit(P::kTime, {}, k.name == "time" ? 1 : 0);
        }
        if (k.name == "geomcolor") {  // the mesh's displayColor (index 0)
            const Value c = _Emit(P::kDisplayColor, {});
            return k.type == "float" ? _Emit(P::kExtract, {c}, 0, 0) : c;
        }
        if (k.name == "tangent" || k.name == "bitangent" || k.name == "viewdirection") {
            const uint32_t world = _String(n, "space") == "world" ? 1 : 0;
            return k.name == "viewdirection" ? _Emit(P::kViewDirection, {}, world)
                                             : _Emit(P::kTangent, {}, world, k.name == "bitangent" ? 1 : 0);
        }
        if (n.nodeTypeId == "ND_artistic_ior") {
            const std::optional<Value> r = in("reflectivity"), e = in("edge_color");
            if (!r || !e) {
                return std::nullopt;
            }
            return _Emit(P::kArtisticIor, {*r, *e}, 0, output == TfToken("extinction") ? 1 : 0);
        }
        if (k.name == "transformpoint" || k.name == "transformvector" || k.name == "transformnormal") {
            auto object = [](std::string const &s) { return s == "object" || s == "model"; };
            const std::string from = _String(n, "fromspace"), to = _String(n, "tospace");
            const uint32_t dir = object(from) && to == "world" ? 1 : from == "world" && object(to) ? 2 : 0;
            const std::optional<Value> v = in("in");
            if (!v) {
                return std::nullopt;
            }
            return _Emit(P::kSpaceTransform, {*v}, k.name == "transformpoint" ? 0 : k.name == "transformvector" ? 1 : 2,
                         dir);
        }
        if (k.name == "transformmatrix") {
            // v * mat, mat a constant: vector2 by matrix33 and vector3 by
            // matrix44 as points (a 1 appended).
            const std::string &id = n.nodeTypeId.GetString();
            const uint32_t d = Components(k.type.substr(0, 7)), rows = id.back() == '4' ? 4 : 3;
            const std::optional<Value> v = in("in");
            std::vector<GfVec4f> m;
            if (!v || _Connected(n, TfToken("mat")) || !_Matrix(n, rows, &m)) {
                return std::nullopt;
            }
            const uint32_t first = uint32_t(_p.constants.size() / 4);
            for (const GfVec4f &row : m) {
                _NewConst(row);
            }
            return _Emit(P::kTransformMatrix, {*v}, first, d | rows << 4);
        }
        if (k.name == "extract" || k.name.rfind("separate", 0) == 0) {
            // separateN's outputs outr/outx .. outa/outw.
            const std::string &o = output.GetString();
            const char c = o.empty() ? 'x' : o.back();
            const std::optional<float> index = k.name == "extract" ? _ConstantInput(n, TfToken("index"))
                                                                   : std::optional<float>(float(c == 'r' || c == 'x' ? 0
                                                                                                : c == 'g' || c == 'y' ? 1
                                                                                                : c == 'b' || c == 'z' ? 2
                                                                                                                       : 3));
            const std::optional<Value> v = in("in");
            if (!v || !index) {
                return std::nullopt;
            }
            return _Emit(P::kExtract, {*v}, 0, uint32_t(std::clamp(*index, 0.0f, 3.0f)));
        }
        if (k.name == "remap") {
            const std::optional<Value> a = in("in"), b = in("inlow"), c = in("inhigh"), d = in("outlow");
            if (!a || !b || !c || !d || _Connected(n, TfToken("outhigh"))) {
                return std::nullopt;
            }
            const Value high = _Const(_Parameter(n, TfToken("outhigh")));
            return _Emit(P::kRemap, {*a, *b, *c, *d}, high.index);
        }
        if (k.name == "ramplr" || k.name == "ramptb") {
            // mix(valuel, valuer, u) and mix(valueb, valuet, v), clamped.
            const bool lr = k.name == "ramplr";
            const std::optional<Value> uv = _Uv(n, _tokens->texcoord, depth);
            const std::optional<Value> from = in(lr ? "valuel" : "valueb"), to = in(lr ? "valuer" : "valuet");
            if (!uv || !from || !to) {
                return std::nullopt;
            }
            const Value x = _Emit(P::kExtract, {*uv}, 0, lr ? 0 : 1);
            const Value c = _Emit(P::kClamp, {x, _Const(GfVec4f(0.0f)), _Const(GfVec4f(1.0f))});
            return _Emit(P::kMix, {*from, *to, c});
        }
        if (k.name == "texcoord") {
            const int index = int(_Parameter(n, _tokens->index)[0]);
            return _Texcoord(index == 0 ? _tokens->st : TfToken("st" + std::to_string(index)));
        }
        if (k.name == "geompropvalue" || k.name == "UsdPrimvarReader") {
            TfToken name;
            std::string str;
            const TfToken &param = k.name == "geompropvalue" ? _tokens->geomprop : _tokens->varname;
            if (!Read(n.parameters, param, &name) && Read(n.parameters, param, &str)) {
                name = TfToken(str);
            }
            if (name.IsEmpty()) {  // no primvar named: its default (an empty geomprop reads nothing)
                return in(k.name == "geompropvalue" ? "default" : "fallback");
            }
            if (name == _tokens->displayColor) {
                return _Emit(P::kDisplayColor, {});
            }
            const std::optional<Value> fallback = in(k.name == "geompropvalue" ? "default" : "fallback");
            if (!fallback) {
                return std::nullopt;
            }
            return Components(k.type) == 2 ? _Texcoord(name, fallback) : _Primvar(name, *fallback);
        }
        if (k.name == "image" || k.name == "tiledimage" || k.name == "UsdUVTexture") {
            const bool preview = k.name == "UsdUVTexture";
            vkr::TextureRef ref;
            LoadFile(n, preview, _textures, &ref);
            std::optional<Value> uv = _Uv(n, preview ? _tokens->st : _tokens->texcoord, depth);
            const std::optional<Value> fallback = in(preview ? "fallback" : "default");
            if (!uv || !fallback) {
                return std::nullopt;
            }
            if (k.name == "tiledimage") {
                float m[6];
                TiledImageMatrix(n.parameters, m);
                uv = _Emit(P::kAffine, {*uv, _Const(GfVec4f(m[0], m[1], m[2], 0)), _Const(GfVec4f(m[3], m[4], m[5], 0))});
            }
            if (preview) {
                const TfToken &o = output;
                ref.channel = o == _tokens->r ? 0 : o == _tokens->g ? 1 : o == _tokens->b ? 2 : o == _tokens->a ? 3 : -1;
                GfVec4f v;
                if (Read(n.parameters, _tokens->scale, &v)) {
                    std::copy(v.data(), v.data() + 4, ref.scale);
                }
                if (Read(n.parameters, _tokens->bias, &v)) {
                    std::copy(v.data(), v.data() + 4, ref.bias);
                }
            } else if (k.type == "float") {
                ref.channel = 0;
            }
            _p.images.push_back(std::move(ref));
            return _Emit(P::kImage, {*uv, *fallback}, uint32_t(_p.images.size() - 1));
        }
        // MaterialX procedurals (shaders/noise.glsl): the 2d ones at texture
        // coordinates, the 3d ones at a position, the surface point in
        // object space unless connected (their nodedefs' defaultgeomprops).
        static const std::set<std::string> noises = {"noise2d",     "noise3d",     "fractal2d",     "fractal3d",
                                                     "cellnoise2d", "cellnoise3d", "worleynoise2d", "worleynoise3d"};
        if (noises.count(k.name)) {
            const uint32_t dims = k.name.back() == 'd' && k.name[k.name.size() - 2] == '2' ? 2 : 3;
            const uint32_t components = k.name.rfind("cellnoise", 0) == 0 ? 1 : Components(k.type);
            const TfToken position("position");
            const std::optional<Value> p = dims == 2              ? _Uv(n, _tokens->texcoord, depth)
                                           : _Connected(n, position) ? _Input(n, position, depth)
                                                                     : std::optional<Value>(_Emit(P::kPosition, {}, 0));
            if (!p) {
                return std::nullopt;
            }
            std::vector<const char *> inputs;
            P::Op op = P::kCellNoise;
            if (k.name.rfind("noise", 0) == 0) {
                op = P::kNoise;
                inputs = {"amplitude", "pivot"};
            } else if (k.name.rfind("fractal", 0) == 0) {
                op = P::kFractal;
                inputs = {"amplitude", "octaves", "lacunarity", "diminish"};
            } else if (k.name.rfind("worley", 0) == 0) {
                op = P::kWorleyNoise;
                inputs = {"jitter", "style"};
            }
            std::vector<Value> args = {*p};
            for (const char *name : inputs) {
                const std::optional<Value> v = in(name);
                if (!v) {
                    return std::nullopt;
                }
                args.push_back(*v);
            }
            if (op == P::kFractal) {  // (octaves, lacunarity, diminish) in one operand
                const Value params = _Emit(P::kCombine, {args[2], args[3], args[4]});
                args.resize(2);
                args.push_back(params);
            }
            return _Emit(op, std::move(args), dims, components);
        }
        if (k.name == "position" || k.name == "normal") {
            TfToken token;
            std::string space;
            if (Read(n.parameters, TfToken("space"), &token)) {
                space = token.GetString();
            } else {
                Read(n.parameters, TfToken("space"), &space);
            }
            return _Emit(k.name == "position" ? P::kPosition : P::kGeomNormal, {}, space == "world" ? 1 : 0);
        }
        if (k.name == "place2d" || k.name == "UsdTransform2d") {
            for (const char *p : {"pivot", "scale", "rotate", "offset", "rotation", "translation"}) {
                if (_Connected(n, TfToken(p))) {
                    return std::nullopt;
                }
            }
            const std::optional<Value> uv = _Uv(n, k.name == "place2d" ? _tokens->texcoord : _tokens->in, depth);
            if (!uv) {
                return std::nullopt;
            }
            float m[6];
            (k.name == "place2d" ? Place2dMatrix : Transform2dMatrix)(n.parameters, m);
            return _Emit(P::kAffine, {*uv, _Const(GfVec4f(m[0], m[1], m[2], 0)), _Const(GfVec4f(m[3], m[4], m[5], 0))});
        }
        // Houdini's ramps (hmtlxramp graphs), as Houdini 22 defines them
        // (houdini/materialx/libraries/houdini; program.glsl has the
        // formulas): hinvlinear places its input among ten keys,
        // huniformramp interpolates ten values. Unauthored keys and values
        // are 0, their nodedefs' defaults.
        if (k.name == "hinvlinear" || k.name == "huniformramp") {
            const bool keys = k.name == "hinvlinear";
            const std::optional<Value> t = in("ramp"), num = in("num");
            if (!t || !num) {
                return std::nullopt;
            }
            std::vector<GfVec4f> values;
            for (int i = 0; i < 10; ++i) {
                values.push_back(_Parameter(n, TfToken((keys ? "key" : "value") + std::to_string(i))));
            }
            if (keys) {
                std::vector<float> packed;
                for (const GfVec4f &v : values) {
                    packed.push_back(v[0]);
                }
                return _Emit(P::kInvLinear, {*t, *num}, _Packed(std::move(packed)));
            }
            const uint32_t first = uint32_t(_p.constants.size() / 4);
            for (const GfVec4f &v : values) {
                _NewConst(v);
            }
            return _Emit(P::kUniformRamp, {*t, *num}, first);
        }
        return std::nullopt;
    }

    HdMaterialNetwork2 const &_net;
    HdVkTextureCache *_textures;
    P &_p;
    TfToken *_uvPrimvar;
    std::vector<Ins> _ops;
    std::map<std::pair<SdfPath, TfToken>, Value> _memo;
    // Memoized values with a bypassed node upstream (they count again where reused).
    std::set<std::pair<SdfPath, TfToken>> _tainted;
    // Nodes of expanded nodegraphs and geomprop readers, and the expanded nodes.
    std::map<SdfPath, HdMaterialNode2> _extra;
    std::set<SdfPath> _expanded;
    size_t _fallbackEvents = 0, _hardFailures = 0;
    std::string _failed;
};

// A colour input in linear Rec.709 (converted from the colour space it was
// authored in), else `color`.
GfVec3f ReadColor(std::map<TfToken, VtValue> const &params, TfToken const &name, GfVec3f color)
{
    if (Read(params, name, &color)) {
        const TfToken colorSpace = ParameterColorSpace(params, name);
        if (NeedsConversion(colorSpace)) {
            color = GfColor(GfColor(color, GfColorSpace(colorSpace)), GfColorSpace(GfColorSpaceNames->LinearRec709))
                        .GetRGB();
        }
    }
    return color;
}

// F0 of a dielectric of index `ior` in air.
float IorToF0(float ior)
{
    const float r = (ior - 1.0f) / (ior + 1.0f);
    return r * r;
}

// The lobes beyond the metallic base (shaders/brdf.glsl): constants
// (authored, else the surface's defaults), and connected inputs (textures,
// graphs) computed per pixel by the material program, as the base's on
// other uv sets. Where a connected input's graph can't be compiled, or
// programs are off, its constant is the default, except specularColor:
// 0.04 grey (a dielectric, as the metallic workflow), not black.
// UsdPreviewSurface has no clearcoat colour (Storm's is white); Eldr reads
// an authored `eldr:clearcoatColor` as standard_surface's coat_color.
// ponytail: textured IORs refract and reflect per pixel, but whether a
// UsdPreviewSurface refracts is decided per material (a connected ior
// does).
void ReadLobes(HdMaterialNode2 const &surface, vkr::Material *m, GraphCompiler &graph)
{
    using P = vkr::MaterialProgram;
    auto const &p = surface.parameters;
    const TfToken &id = surface.nodeTypeId;
    auto color = [&](TfToken const &name, float *out) {
        const GfVec3f c = ReadColor(p, name, GfVec3f(out[0], out[1], out[2]));
        std::copy(c.data(), c.data() + 3, out);
    };
    // Whether `name` is connected; if so the program computes it (times
    // `scale`) where it can; `channel` >= 0 splats that component.
    auto program = [&](TfToken const &name, P::Input target, float scale = 1.0f, int channel = -1) {
        const auto c = surface.inputConnections.find(name);
        if (c == surface.inputConnections.end() || c->second.empty()) {
            return false;
        }
        graph.Output(surface, name, target, scale, channel);  // warns about what it can't compile
        return true;
    };
    if (id == _tokens->UsdPreviewSurface || id == _tokens->ND_UsdPreviewSurface_surfaceshader) {
        if (ReadOr(p, _tokens->useSpecularWorkflow, 0) != 0) {
            m->specularWorkflow = true;
            color(_tokens->specularColor, m->specularColor);
            if (program(_tokens->specularColor, P::kSpecularColor)) {
                std::fill(m->specularColor, m->specularColor + 3, 0.04f);
            }
            m->metallic = 0.0f;  // the workflow ignores it
            m->metallicTex = vkr::TextureRef();
        }
        // The coat's F0 from the surface's ior, as Storm.
        m->coat = ReadOr(p, _tokens->clearcoat, 0.0f);
        m->coatRoughness = ReadOr(p, _tokens->clearcoatRoughness, 0.01f);
        m->coatF0 = IorToF0(ReadOr(p, _tokens->ior, 1.5f));
        color(_tokens->clearcoatColor, m->coatColor);
        program(_tokens->clearcoat, P::kCoat);
        program(_tokens->clearcoatRoughness, P::kCoatRoughness);
        program(_tokens->clearcoatColor, P::kCoatColor);
        if (program(_tokens->ior, P::kCoatIor)) {
            program(_tokens->ior, P::kIor);
            m->opacityRefracts = true;
        }
    } else if (id == _tokens->ND_standard_surface_surfaceshader) {
        m->coat = ReadOr(p, _tokens->coat, 0.0f);
        m->coatRoughness = ReadOr(p, _tokens->coat_roughness, 0.1f);
        m->coatF0 = IorToF0(ReadOr(p, _tokens->coat_IOR, 1.5f));
        color(_tokens->coat_color, m->coatColor);
        m->sheen = ReadOr(p, _tokens->sheen, 0.0f);
        m->sheenRoughness = ReadOr(p, _tokens->sheen_roughness, 0.3f);
        color(_tokens->sheen_color, m->sheenColor);
        // Its thin film is on whenever it has a thickness (nm).
        m->thinFilmThickness = ReadOr(p, _tokens->thin_film_thickness, 0.0f);
        const bool textured = program(_tokens->thin_film_thickness, P::kThinFilmThickness);
        m->thinFilm = m->thinFilmThickness > 0.0f || textured ? 1.0f : 0.0f;
        m->thinFilmIor = ReadOr(p, _tokens->thin_film_IOR, 1.5f);
        m->anisotropy = ReadOr(p, _tokens->specular_anisotropy, 0.0f);
        m->anisotropyRotation = ReadOr(p, _tokens->specular_rotation, 0.0f);
        program(_tokens->coat, P::kCoat);
        program(_tokens->coat_roughness, P::kCoatRoughness);
        program(_tokens->coat_color, P::kCoatColor);
        program(_tokens->sheen, P::kSheen);
        program(_tokens->sheen_color, P::kSheenColor);
        program(_tokens->sheen_roughness, P::kSheenRoughness);
        program(_tokens->specular_anisotropy, P::kAnisotropy);
        program(_tokens->coat_IOR, P::kCoatIor);
        program(_tokens->thin_film_IOR, P::kThinFilmIor);
        program(_tokens->specular_rotation, P::kAnisotropyRotation);
        program(_tokens->specular_IOR, P::kIor);
        program(_tokens->transmission, P::kTransmission);
        program(_tokens->transmission_color, P::kTransmissionColor);
        program(_tokens->subsurface, P::kSubsurface);
        program(_tokens->subsurface_color, P::kSubsurfaceColor);
        program(_tokens->subsurface_radius, P::kSubsurfaceRadius);  // times subsurface_scale
        program(_tokens->subsurface_scale, P::kSubsurfaceScale, 1.0f, 0);
        program(_tokens->transmission_depth, P::kTransmissionDepth);
        program(_tokens->thin_walled, P::kThinWalled);
    } else if (id == _tokens->ND_open_pbr_surface_surfaceshader) {
        // ponytail: coat_darkening isn't modelled (the coat's albedo only);
        // fuzz is standard_surface's sheen (Imageworks, not Zeltner's LTC),
        // under the coat rather than over it.
        m->coat = ReadOr(p, _tokens->coat_weight, 0.0f);
        m->coatRoughness = ReadOr(p, _tokens->coat_roughness, 0.0f);
        m->coatF0 = IorToF0(ReadOr(p, _tokens->coat_ior, 1.6f));
        color(_tokens->coat_color, m->coatColor);
        m->sheen = ReadOr(p, _tokens->fuzz_weight, 0.0f);
        m->sheenRoughness = ReadOr(p, _tokens->fuzz_roughness, 0.5f);
        color(_tokens->fuzz_color, m->sheenColor);
        m->thinFilm = ReadOr(p, _tokens->thin_film_weight, 0.0f);
        m->thinFilmThickness = 1000.0f * ReadOr(p, _tokens->thin_film_thickness, 0.5f);  // micrometres
        m->thinFilmIor = ReadOr(p, _tokens->thin_film_ior, 1.4f);
        m->anisotropy = ReadOr(p, _tokens->specular_roughness_anisotropy, 0.0f);
        m->openPbrAnisotropy = true;
        program(_tokens->coat_weight, P::kCoat);
        program(_tokens->coat_roughness, P::kCoatRoughness);
        program(_tokens->coat_color, P::kCoatColor);
        program(_tokens->fuzz_weight, P::kSheen);
        program(_tokens->fuzz_color, P::kSheenColor);
        program(_tokens->fuzz_roughness, P::kSheenRoughness);
        program(_tokens->thin_film_thickness, P::kThinFilmThickness, 1000.0f);
        program(_tokens->specular_roughness_anisotropy, P::kAnisotropy);
        program(_tokens->coat_ior, P::kCoatIor);
        program(_tokens->thin_film_weight, P::kThinFilm);
        program(_tokens->thin_film_ior, P::kThinFilmIor);
        program(_tokens->specular_ior, P::kIor);
        program(_tokens->transmission_weight, P::kTransmission);
        program(_tokens->transmission_color, P::kTransmissionColor);
        program(_tokens->subsurface_weight, P::kSubsurface);
        program(_tokens->subsurface_color, P::kSubsurfaceColor);
        program(_tokens->subsurface_radius, P::kSubsurfaceRadius);  // times subsurface_radius_scale
        program(_tokens->subsurface_radius_scale, P::kSubsurfaceScale);
        program(_tokens->transmission_depth, P::kTransmissionDepth);
        program(_tokens->geometry_thin_walled, P::kThinWalled);
    }
    // NPR toon has no USD standard: Eldr's own inputs on any surface it
    // reads, e.g. `float inputs:eldr:toonBands = 3` (flat bands per unit of
    // light, 0: none) and `float inputs:eldr:toonSoftness = 0.05` (edge
    // width, in bands). On BasisCurves `bool primvars:eldr:hair = 0` turns
    // hair shading off (hydra/curves.cpp).
    m->toonBands = std::max(ReadOr(p, _tokens->toonBands, 0.0f), 0.0f);
    m->toonSoftness = ReadOr(p, _tokens->toonSoftness, 0.05f);
}

// ponytail: metallic and specular workflows; the lobes ReadLobes reads.
// Transmission and subsurface below. Ignored for now: displacement.
// *uvPrimvar: the uv set in the mesh's vertices, the first one a texture
// reads; textures on other sets are read by the program, through primvars.
// What a material's graphs couldn't compile (HdVkMaterialFallbacks).
struct Fallbacks {
    int inputs = 0;
    std::vector<std::string> nodes;

    void Add(int n, std::vector<std::string> const &more)
    {
        inputs += n;
        for (const std::string &node : more) {
            if (std::find(nodes.begin(), nodes.end(), node) == nodes.end()) {
                nodes.push_back(node);
            }
        }
    }
};

void ReadSurface(HdMaterialNetwork2 const &net, HdVkTextureCache *textures, vkr::Material *m, TfToken *uvPrimvar,
                 Fallbacks *fallbacks)
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
        const std::string &id = surface.nodeTypeId.GetString();
        fallbacks->Add(1, {surfaceIt->first.GetName() + " (" + id + ")"});
        static std::mutex mutex;
        static std::set<std::string> warned;
        std::lock_guard<std::mutex> lock(mutex);
        if (warned.insert(id).second) {
            TF_WARN("HdVk: surface shader '%s' isn't supported; its materials render with UsdPreviewSurface's "
                    "defaults", id.c_str());
        }
        return;
    }

    // One input: its constant (authored, else the default) times the weight,
    // or a texture whose scale and bias carry the weight, or else a program
    // computing it.
    GraphCompiler graph(net, textures, &m->program, uvPrimvar);
    auto input = [&](TfToken const &name, TfToken const &weightName, float weightDefault, float valueDefault,
                     float *constant, int size, vkr::TextureRef *ref, vkr::MaterialProgram::Input target) {
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
        TfToken primvar;
        ReadTexture(net, &surface, name, textures, m, ref, &primvar);
        if (ref->texture && uvPrimvar->IsEmpty()) {
            *uvPrimvar = primvar;
        }
        // A texture on another uv set than the material's goes to the
        // program, which reads that set as a primvar.
        vkr::TextureRef otherUvSet;
        if (ref->texture && primvar != *uvPrimvar) {
            std::swap(otherUvSet, *ref);
        }
        const auto c = surface.inputConnections.find(name);
        if (!ref->texture && c != surface.inputConnections.end() && !c->second.empty() &&
            !(ref == &m->baseColorTex && m->baseColorFromDisplayColor) &&
            !graph.Output(surface, name, target, size == 3 ? weight : 1.0f) && otherUvSet.texture) {
            std::swap(otherUvSet, *ref);
            TF_WARN("HdVk: a material reads uv sets '%s' and '%s' but can't run a graph for '%s'; its "
                    "textures read '%s'", uvPrimvar->GetText(), primvar.GetText(), name.GetText(),
                    uvPrimvar->GetText());
        }
        for (int i = 0; ref->texture && i < 3; ++i) {
            ref->scale[i] *= weight;
            ref->bias[i] *= weight;
        }
    };

    if (!kind->specular.IsEmpty()) {
        float weight = 1.0f;
        GfVec3f color(1.0f);
        Read(surface.parameters, kind->specular, &weight);
        if (Read(surface.parameters, kind->specularColor, &color) &&
            NeedsConversion(ParameterColorSpace(surface.parameters, kind->specularColor))) {
            color = GfColor(GfColor(color, GfColorSpace(ParameterColorSpace(surface.parameters, kind->specularColor))),
                            GfColorSpace(GfColorSpaceNames->LinearRec709))
                        .GetRGB();
        }
        for (int i = 0; i < 3; ++i) {
            m->specular[i] = weight * color[i];
        }
    }
    // Transmission: standard_surface and OpenPBR transmit and refract by
    // their own inputs; UsdPreviewSurface refracts what its opacity lets
    // through once it has an ior ("for translucent objects"; Hydra hands
    // over authored inputs only).
    // Connected weights, colours, transmission_depth, thin_walled, the
    // radius and its scale are computed by the material program
    // (ReadLobes); their constants here are the defaults they fall back to
    // (a connected radius or scale: 1, as the program multiplies it in).
    const bool openPbr = surface.nodeTypeId == _tokens->ND_open_pbr_surface_surfaceshader;
    if (openPbr || surface.nodeTypeId == _tokens->ND_standard_surface_surfaceshader) {
        Read(surface.parameters, openPbr ? _tokens->transmission_weight : _tokens->transmission, &m->transmission);
        GfVec3f tint;
        if (Read(surface.parameters, _tokens->transmission_color, &tint)) {
            const TfToken colorSpace = ParameterColorSpace(surface.parameters, _tokens->transmission_color);
            if (NeedsConversion(colorSpace)) {
                tint = GfColor(GfColor(tint, GfColorSpace(colorSpace)), GfColorSpace(GfColorSpaceNames->LinearRec709))
                           .GetRGB();
            }
            std::copy(tint.data(), tint.data() + 3, m->transmissionColor);
        }
        Read(surface.parameters, _tokens->transmission_depth, &m->transmissionDepth);
        Read(surface.parameters, openPbr ? _tokens->specular_ior : _tokens->specular_IOR, &m->ior);
        Read(surface.parameters, openPbr ? _tokens->geometry_thin_walled : _tokens->thin_walled, &m->thinWalled);
        m->transmission = std::clamp(m->transmission, 0.0f, 1.0f);
        // Subsurface: standard_surface's radius is a colour times a scale,
        // OpenPBR's a distance times a colour (the shaders' defaults
        // apply where nothing is authored).
        Read(surface.parameters, openPbr ? _tokens->subsurface_weight : _tokens->subsurface, &m->subsurface);
        m->subsurface = std::clamp(m->subsurface, 0.0f, 1.0f);
        GfVec3f color = openPbr ? GfVec3f(0.8f) : GfVec3f(1.0f);
        if (Read(surface.parameters, _tokens->subsurface_color, &color)) {
            const TfToken colorSpace = ParameterColorSpace(surface.parameters, _tokens->subsurface_color);
            if (NeedsConversion(colorSpace)) {
                color = GfColor(GfColor(color, GfColorSpace(colorSpace)),
                                GfColorSpace(GfColorSpaceNames->LinearRec709)).GetRGB();
            }
        }
        std::copy(color.data(), color.data() + 3, m->subsurfaceColor);
        GfVec3f radius = openPbr ? GfVec3f(1.0f, 0.5f, 0.25f) : GfVec3f(1.0f);
        float scale = 1.0f;
        if (openPbr) {
            if (surface.inputConnections.count(_tokens->subsurface_radius_scale)) {
                radius = GfVec3f(1.0f);  // the program's
            }
            Read(surface.parameters, _tokens->subsurface_radius_scale, &radius);
            Read(surface.parameters, _tokens->subsurface_radius, &scale);
        } else {
            Read(surface.parameters, _tokens->subsurface_radius, &radius);
            Read(surface.parameters, _tokens->subsurface_scale, &scale);
        }
        for (int i = 0; i < 3; ++i) {
            m->subsurfaceRadius[i] = std::max(radius[i] * scale, 0.0f);
        }
    } else if (Read(surface.parameters, _tokens->ior, &m->ior)) {
        m->opacityRefracts = m->ior != 1.0f;
    }
    using P = vkr::MaterialProgram;
    input(kind->baseColor, kind->baseWeight, kind->baseWeightDefault, kind->baseColorDefault, m->baseColor, 3,
          &m->baseColorTex, P::kBaseColor);
    input(kind->emission, kind->emissionWeight, kind->emissionWeightDefault, kind->emissionDefault, m->emissive,
          3, &m->emissiveTex, P::kEmissive);
    input(kind->metallic, TfToken(), 1.0f, 0.0f, &m->metallic, 1, &m->metallicTex, P::kMetallic);
    input(kind->roughness, TfToken(), 1.0f, kind->roughnessDefault, &m->roughness, 1, &m->roughnessTex,
          P::kRoughness);
    input(kind->occlusion, TfToken(), 1.0f, 1.0f, &m->occlusion, 1, &m->occlusionTex, P::kOcclusion);
    float unusedNormal[3];
    input(kind->normal, TfToken(), 1.0f, 0.0f, unusedNormal, 3, &m->normalTex, P::kNormal);
    // standard_surface's opacity is a colour: its constant stays 1, a
    // texture is read through its first channel.
    input(kind->opacity, TfToken(), 1.0f, 1.0f, &m->opacity, 1, &m->opacityTex, P::kOpacity);
    ReadLobes(surface, m, graph);
    const int outputs = graph.Outputs();
    if (!graph.Finish()) {
        TF_WARN("HdVk: a material's shading graph needs more than %u registers; its inputs keep their constants",
                P::kRegisters);
        graph.fallbackInputs = outputs;
        graph.fallbackNodes.push_back("(registers)");
    }
    fallbacks->Add(graph.fallbackInputs, graph.fallbackNodes);
    if (!Read(surface.parameters, _tokens->opacityThreshold, &m->opacityThreshold) && m->opacityTex.texture &&
        StartsWith(surface.nodeTypeId, "ND_")) {
        // MaterialX has no threshold: an opacity texture is a cutout, as
        // realtime engines draw foliage; constant opacity stays translucent.
        m->opacityThreshold = 0.5f;
    }
}

// The largest |texel * scale + bias| of ref's channel (the red one without
// a channel): 8- and 16-bit textures hold 0..1, float ones are scanned.
float TexelBound(vkr::TextureRef const &ref)
{
    using F = vkr::Texture::Format;
    float largest = 0.0f;
    auto scan = [&](vkr::Texture const *t) {
        if (!t) {
            return;
        }
        if (t->format == F::R16F || t->format == F::RG16F || t->format == F::RGBA16F) {
            for (size_t i = 0; i + 1 < t->pixels.size(); i += 2) {
                uint16_t bits;
                std::memcpy(&bits, &t->pixels[i], 2);
                GfHalf h;
                h.setBits(bits);
                largest = std::isfinite(float(h)) ? std::max(largest, std::abs(float(h))) : largest;
            }
        } else if (t->format == F::RGBA32F) {
            for (size_t i = 0; i + 3 < t->pixels.size(); i += 4) {
                float f;
                std::memcpy(&f, &t->pixels[i], 4);
                largest = std::isfinite(f) ? std::max(largest, std::abs(f)) : largest;
            }
        } else {
            largest = std::max(largest, 1.0f);
        }
    };
    scan(ref.texture.get());
    for (const auto &tile : ref.udim) {
        scan(tile.get());
    }
    const int c = std::max(ref.channel, 0);
    return largest * std::abs(ref.scale[c]) + std::abs(ref.bias[c]);
}

// The displacement terminal: a UsdPreviewSurface's `displacement` input, or
// a MaterialX displacement shader's `displacement` (along the normal; of a
// vector, its normal component z) times its `scale`. A constant, or a
// texture through what ReadTexture follows, read with the material's uv set,
// or else a graph compiled into m->displacementProgram (mesh.vert runs it).
// ponytail: vector displacement drops its tangent components (per-vertex
// tangents); a graph's bound is |scale| (its value taken as within -1..1).
void ReadDisplacement(HdMaterialNetwork2 const &net, HdVkTextureCache *textures, vkr::Material *m,
                      TfToken *uvPrimvar, Fallbacks *fallbacks)
{
    const auto terminal = net.terminals.find(HdMaterialTerminalTokens->displacement);
    const auto it = terminal == net.terminals.end() ? net.nodes.end() : net.nodes.find(terminal->second.upstreamNode);
    if (it == net.nodes.end()) {
        return;
    }
    const HdMaterialNode2 &node = it->second;
    const TfToken &id = node.nodeTypeId, &input = HdMaterialTerminalTokens->displacement;
    const bool vector = id == "ND_displacement_vector3", mtlx = vector || id == "ND_displacement_float";
    if (!mtlx && id != _tokens->UsdPreviewSurface && id != _tokens->ND_UsdPreviewSurface_surfaceshader) {
        return;
    }
    const float scale = mtlx ? ReadOr(node.parameters, _tokens->scale, 1.0f) : 1.0f;
    GfVec3f v(0.0f);
    if (!Read(node.parameters, input, &m->displacement) && Read(node.parameters, input, &v)) {
        m->displacement = v[2];
    }
    m->displacement *= scale;
    m->displacementBound = std::abs(m->displacement);
    TfToken primvar;
    vkr::TextureRef &ref = m->displacementTex;
    ReadTexture(net, &node, input, textures, m, &ref, &primvar);
    const auto c = node.inputConnections.find(input);
    if (!ref.texture && c != node.inputConnections.end() && !c->second.empty()) {
        vkr::MaterialProgram &p = m->displacementProgram;
        GraphCompiler graph(net, textures, &p, uvPrimvar);
        const bool compiled = graph.Output(node, input, vkr::MaterialProgram::kDisplacement, scale, vector ? 2 : -1);
        fallbacks->Add(graph.fallbackInputs, graph.fallbackNodes);
        if (!compiled || !graph.Finish() || !p.Valid()) {
            if (compiled) {
                fallbacks->Add(1, {"(registers)"});
            }
            p = vkr::MaterialProgram();
            TF_WARN("HdVk: can't run the displacement graph (node %s)", graph.Failed().c_str());
            return;
        }
        // Its primvars read the surface program's slots (one table per draw).
        for (size_t op = 0; op < p.code.size() / 4; ++op) {
            if ((p.code[op * 4] & 0xFFu) == vkr::MaterialProgram::kPrimvar) {
                const std::string &name = p.primvars[p.code[op * 4 + 2]];
                std::vector<std::string> &slots = m->program.primvars;
                const auto at = std::find(slots.begin(), slots.end(), name);
                p.code[op * 4 + 2] = uint32_t(at - slots.begin());
                if (at == slots.end()) {
                    slots.push_back(name);
                }
            }
        }
        m->displacementBound = std::abs(scale);
        return;
    }
    if (!ref.texture) {
        return;
    }
    if (vector && ref.channel < 0) {
        ref.channel = 2;  // (dPdu, dPdv, N) space: z is along the normal
    }
    for (int i = 0; i < 4; ++i) {
        ref.scale[i] *= scale;
        ref.bias[i] *= scale;
    }
    if (uvPrimvar->IsEmpty()) {
        *uvPrimvar = primvar;
    } else if (primvar != *uvPrimvar) {
        TF_WARN("HdVk: a material reads uv sets '%s' and '%s'; all textures use '%s'", uvPrimvar->GetText(),
                primvar.GetText(), uvPrimvar->GetText());
    }
    m->displacementBound = TexelBound(ref);
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

void HdVkMaterialFallbacks::Set(SdfPath const &material, int inputs, std::vector<std::string> nodes)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (inputs > 0) {
        _byMaterial[material] = {inputs, std::move(nodes)};
    } else {
        _byMaterial.erase(material);
    }
}

int HdVkMaterialFallbacks::Inputs() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    int n = 0;
    for (const auto &[path, f] : _byMaterial) {
        n += f.first;
    }
    return n;
}

std::string HdVkMaterialFallbacks::Nodes(size_t max) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    std::string out;
    size_t n = 0;
    for (const auto &[path, f] : _byMaterial) {
        for (const std::string &node : f.second) {
            if (n++ < max) {
                out += (out.empty() ? "" : ", ") + path.GetName() + ": " + node;
            }
        }
    }
    return n > max ? out + ", and " + std::to_string(n - max) + " more" : out;
}

HdVkMaterial::HdVkMaterial(SdfPath const &id, HdVkTextureCache *textures, HdVkMaterialFallbacks *fallbacks)
    : HdMaterial(id)
    , _textures(textures)
    , _fallbacks(fallbacks)
    , _material(std::make_shared<vkr::Material>())
    , _volume(std::make_shared<vkr::VolumeShading>())
{
}

HdVkMaterial::~HdVkMaterial()
{
    _fallbacks->Set(GetId(), 0, {});
}

void HdVkMaterial::Sync(HdSceneDelegate *sceneDelegate, HdRenderParam *,
                        HdDirtyBits *dirtyBits)
{
    HdVkRenderDelegate::NoteSceneEdit(sceneDelegate);
    if (*dirtyBits & (DirtyResource | DirtyParams)) {
        vkr::Material m;  // UsdPreviewSurface defaults
        vkr::VolumeShading volume;
        _uvPrimvar = TfToken();
        Fallbacks fallbacks;
        const VtValue resource = sceneDelegate->GetMaterialResource(GetId());
        if (resource.IsHolding<HdMaterialNetworkMap>()) {
            const HdMaterialNetwork2 net = HdConvertToHdMaterialNetwork2(resource.UncheckedGet<HdMaterialNetworkMap>());
            ReadSurface(net, _textures, &m, &_uvPrimvar, &fallbacks);
            ReadDisplacement(net, _textures, &m, &_uvPrimvar, &fallbacks);
#ifdef HDVK_VOLUMES
            HdVkReadVolumeShading(net, &volume);
#endif
        }
        *_volume = std::move(volume);
        _fallbacks->Set(GetId(), fallbacks.inputs, std::move(fallbacks.nodes));
        _primvars.clear();
        for (const std::string &name : m.program.primvars) {
            _primvars.emplace_back(name);
        }
        *_material = std::move(m);
    }
    *dirtyBits = Clean;
}

PXR_NAMESPACE_CLOSE_SCOPE
