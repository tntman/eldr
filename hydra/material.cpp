#include "material.h"

#include "scene.h"

#include "pxr/base/gf/vec3f.h"
#include "pxr/base/gf/vec4f.h"
#include "pxr/base/tf/diagnostic.h"
#include "pxr/imaging/hd/sceneDelegate.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/imaging/hio/image.h"
#include "pxr/usd/sdf/assetPath.h"

#include <cstring>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

TF_DEFINE_PRIVATE_TOKENS(
    _tokens,
    (UsdPreviewSurface)
    (UsdUVTexture)
    (UsdPrimvarReader_float3)
    (diffuseColor)
    (emissiveColor)
    (metallic)
    (roughness)
    (occlusion)
    (normal)
    (file)
    (sourceColorSpace)
    (scale)
    (bias)
    (raw)
    (sRGB)
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

std::shared_ptr<vkr::Texture> LoadTexture(std::string const &path, TfToken const &colorSpace)
{
    const HioImage::SourceColorSpace cs =
        colorSpace == _tokens->raw  ? HioImage::Raw :
        colorSpace == _tokens->sRGB ? HioImage::SRGB : HioImage::Auto;
    HioImageSharedPtr image = HioImage::OpenForReading(path, 0, 0, cs);
    if (!image) {
        TF_WARN("HdVk: cannot read texture '%s'", path.c_str());
        return nullptr;
    }
    const HioFormat format = image->GetFormat();
    const HioType type = HioGetHioType(format);
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

    auto tex = std::make_shared<vkr::Texture>();
    tex->width = uint32_t(w);
    tex->height = uint32_t(h);
    const size_t texels = size_t(w) * h;
    switch (type) {
    case HioTypeUnsignedByte:
    case HioTypeUnsignedByteSRGB:
        tex->format = (type == HioTypeUnsignedByteSRGB || image->IsColorSpaceSRGB())
            ? vkr::Texture::Format::RGBA8_SRGB : vkr::Texture::Format::RGBA8;
        tex->pixels.resize(texels * 4);
        ExpandToRgba<uint8_t>(src.data(), texels, channels, 255, tex->pixels.data());
        break;
    case HioTypeHalfFloat:
        tex->format = vkr::Texture::Format::RGBA16F;
        tex->pixels.resize(texels * 8);
        ExpandToRgba<uint16_t>(reinterpret_cast<const uint16_t *>(src.data()), texels,
                               channels, 0x3C00, reinterpret_cast<uint16_t *>(tex->pixels.data()));
        break;
    case HioTypeUnsignedShort: {
        // 16-bit PNG/TIFF: widen to float rather than add another GPU format.
        std::vector<float> f(texels * channels);
        const auto *s = reinterpret_cast<const uint16_t *>(src.data());
        for (size_t i = 0; i < f.size(); ++i) {
            f[i] = s[i] / 65535.0f;
        }
        tex->format = vkr::Texture::Format::RGBA32F;
        tex->pixels.resize(texels * 16);
        ExpandToRgba<float>(f.data(), texels, channels, 1.0f,
                            reinterpret_cast<float *>(tex->pixels.data()));
        break;
    }
    default:  // HioTypeFloat
        tex->format = vkr::Texture::Format::RGBA32F;
        tex->pixels.resize(texels * 16);
        ExpandToRgba<float>(reinterpret_cast<const float *>(src.data()), texels, channels,
                            1.0f, reinterpret_cast<float *>(tex->pixels.data()));
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

// ponytail: metallic workflow only. Ignored for now: opacity/opacityThreshold,
// specular workflow, clearcoat, ior, displacement, UsdTransform2d and the
// primvar reader's varname (meshes always provide "st").
void ReadPreviewSurface(HdMaterialNetwork2 const &net, HdVkTextureCache *textures,
                        vkr::Material *m)
{
    const auto terminal = net.terminals.find(HdMaterialTerminalTokens->surface);
    if (terminal == net.terminals.end()) {
        return;
    }
    const auto surfaceIt = net.nodes.find(terminal->second.upstreamNode);
    if (surfaceIt == net.nodes.end() ||
        surfaceIt->second.nodeTypeId != _tokens->UsdPreviewSurface) {
        return;
    }
    const HdMaterialNode2 &surface = surfaceIt->second;

    // Reads a constant and/or a UsdUVTexture connection for one input.
    auto input = [&](TfToken const &name, float *constant, int size, vkr::TextureRef *ref) {
        if (size == 1) {
            Read(surface.parameters, name, constant);
        } else {
            GfVec3f v;
            if (Read(surface.parameters, name, &v)) {
                std::copy(v.data(), v.data() + 3, constant);
            }
        }
        const auto conn = surface.inputConnections.find(name);
        if (conn == surface.inputConnections.end() || conn->second.empty()) {
            return;
        }
        const HdMaterialConnection2 &c = conn->second[0];
        const auto up = net.nodes.find(c.upstreamNode);
        if (up == net.nodes.end()) {
            return;
        }
        const HdMaterialNode2 &node = up->second;
        if (node.nodeTypeId == _tokens->UsdPrimvarReader_float3 && ref == &m->baseColorTex) {
            TfToken varname;
            std::string varnameStr;
            if ((Read(node.parameters, _tokens->varname, &varname) && varname == _tokens->displayColor) ||
                (Read(node.parameters, _tokens->varname, &varnameStr) && varnameStr == "displayColor")) {
                m->baseColorFromDisplayColor = true;
            }
            return;
        }
        if (node.nodeTypeId != _tokens->UsdUVTexture) {
            return;
        }
        const auto file = node.parameters.find(_tokens->file);
        const std::string path = file == node.parameters.end() ? std::string()
                                                               : AssetPath(file->second);
        if (path.empty()) {
            return;
        }
        TfToken colorSpace;
        Read(node.parameters, _tokens->sourceColorSpace, &colorSpace);
        ref->texture = textures->Get(path, colorSpace);
        const TfToken &out = c.upstreamOutputName;
        ref->channel = out == _tokens->r ? 0 : out == _tokens->g ? 1 :
                       out == _tokens->b ? 2 : out == _tokens->a ? 3 : -1;
        GfVec4f v;
        if (Read(node.parameters, _tokens->scale, &v)) {
            std::copy(v.data(), v.data() + 4, ref->scale);
        }
        if (Read(node.parameters, _tokens->bias, &v)) {
            std::copy(v.data(), v.data() + 4, ref->bias);
        }
    };

    input(_tokens->diffuseColor, m->baseColor, 3, &m->baseColorTex);
    input(_tokens->emissiveColor, m->emissive, 3, &m->emissiveTex);
    input(_tokens->metallic, &m->metallic, 1, &m->metallicTex);
    input(_tokens->roughness, &m->roughness, 1, &m->roughnessTex);
    input(_tokens->occlusion, &m->occlusion, 1, &m->occlusionTex);
    float unusedNormal[3];
    input(_tokens->normal, unusedNormal, 3, &m->normalTex);
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
            ReadPreviewSurface(
                HdConvertToHdMaterialNetwork2(resource.UncheckedGet<HdMaterialNetworkMap>()),
                _textures, &m);
        }
        *_material = std::move(m);
    }
    *dirtyBits = Clean;
}

PXR_NAMESPACE_CLOSE_SCOPE
