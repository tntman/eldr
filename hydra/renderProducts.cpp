#include "renderProducts.h"

#include "renderDelegate.h"

#include "pxr/base/gf/half.h"
#include "pxr/base/tf/diagnostic.h"
#include "pxr/base/tf/stringUtils.h"
#include "pxr/imaging/hd/aov.h"
#include "pxr/imaging/hd/tokens.h"

#ifdef HDVK_EXR
#include <OpenImageIO/imageio.h>
#endif

#include <cstdio>
#include <utility>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

// The AOV a render var names, as UsdRender's spec does.
TfToken AovName(const HdRenderSettings::RenderProduct::RenderVar &var)
{
    if (var.sourceType == TfToken("lpe")) {
        return HdAovTokensMakeLpe(TfToken(var.sourceName));
    }
    if (var.sourceType == TfToken("primvar")) {
        return HdAovTokensMakePrimvar(TfToken(var.sourceName));
    }
    return TfToken(var.sourceName);
}

bool IsCryptomatte(uint32_t kind)
{
    return kind == vkr::Aovs::kCryptoObject || kind == vkr::Aovs::kCryptoMaterial;
}

} // namespace

void HdVkRenderProducts::Prepare(const HdRenderSettings *settings, uint32_t width, uint32_t height,
                                 vkr::Renderer::Targets *out)
{
    std::vector<Product> products;
    std::fill(_need, _need + vkr::Aovs::kCount, false);
    if (settings && settings->IsActive()) {
        for (const HdRenderSettings::RenderProduct &rp : settings->GetRenderProducts()) {
            if (rp.type != TfToken("raster") || rp.name.IsEmpty()) {
                continue;
            }
            Product product{rp.name.GetString(), {}};
            for (const HdRenderSettings::RenderProduct::RenderVar &rv : rp.renderVars) {
                const TfToken aov = AovName(rv);
                Var var{rv.varPath.GetName(), Source::Aov, 0};
                if (aov == HdAovTokens->color || aov == TfToken("lpe:C.*[LO]")) {
                    var.source = Source::Color;
                } else if (aov == HdAovTokens->depth) {
                    var.source = Source::Depth;
                } else if (aov == HdAovTokens->primId) {
                    var.source = Source::PrimId;
                } else if (aov == HdAovTokens->instanceId) {
                    var.source = Source::InstanceId;
                } else {
                    bool found = false;
                    for (const HdVkExtraAov &extra : HdVkExtraAovs()) {
                        if (extra.name == aov) {
                            var.kind = extra.kind;
                            found = true;
                        }
                    }
                    if (!found) {
                        TF_WARN("HdVk: render var <%s> (%s) has no Eldr AOV; left out of %s", rv.varPath.GetText(),
                                aov.GetText(), rp.name.GetText());
                        continue;
                    }
                    _need[var.kind] = true;
                }
                product.vars.push_back(var);
            }
            products.push_back(std::move(product));
        }
    }
    if (width != _width || height != _height || products.size() != _products.size()) {
        _written = false;
    }
    _products = std::move(products);
    _width = width;
    _height = height;
    for (uint32_t k = 0; k < vkr::Aovs::kCount; ++k) {
        // A kind a Hydra buffer takes isn't the product's (one target per kind).
        if (!_need[k] || out->aovs[k]) {
            _need[k] = false;
            _aovs[k].clear();
            continue;
        }
        // Cryptomatte: (id, coverage, id, coverage); the rest as they are.
        const uint32_t components = IsCryptomatte(k) ? 4 : vkr::Aovs::Components(vkr::Aovs::Kind(k));
        _aovs[k].assign(size_t(width) * height * components, 0.0f);
        out->aovs[k] = _aovs[k].data();
        out->aovComponents[k] = uint8_t(components);
    }
}

void HdVkRenderProducts::Write(vkr::Renderer &renderer, const vkr::Renderer::Frame &frame,
                               const std::string &objectManifest, const std::string &materialManifest)
{
    // FNV-1a over what the image shows; a time step of usdrecord or a
    // script can converge in its first frame (the post motion blur's).
    uint64_t key = 1469598103934665603ull;
    auto mix = [&](const void *data, size_t size) {
        for (size_t i = 0; i < size; ++i) {
            key = (key ^ static_cast<const unsigned char *>(data)[i]) * 1099511628211ull;
        }
    };
    mix(&frame.sceneVersion, sizeof(frame.sceneVersion));
    mix(frame.viewProj, sizeof(frame.viewProj));
    if (_products.empty() || !renderer.IsConverged()) {
        _written = false;
        return;
    }
    if (_written && key == _writtenKey) {
        return;
    }
    _written = true;
    _writtenKey = key;
#ifdef HDVK_EXR
    const size_t pixels = size_t(_width) * _height;
    // Colour, depth and the ids from the frame's images.
    std::vector<uint16_t> color(pixels * 4);
    std::vector<float> depth(pixels);
    std::vector<int32_t> primId(pixels), instanceId(pixels);
    vkr::Renderer::Targets read;
    read.color = color.data();
    read.depth = depth.data();
    read.primId = primId.data();
    read.instanceId = instanceId.data();
    renderer.Read(read);

    for (const Product &product : _products) {
        std::vector<std::string> channels;
        std::vector<std::pair<std::string, std::string>> metadata;
        // Per channel where its values are: float or half or int, stride.
        struct Channel {
            const void *data;
            int type;  // 0 float, 1 half, 2 int
            uint32_t stride, offset;
        };
        std::vector<Channel> sources;
        bool beauty = false;
        static const char *rgba[4] = {"R", "G", "B", "A"};
        for (const Var &var : product.vars) {
            if (var.source == Source::Color) {
                for (uint32_t c = 0; c < 4; ++c) {
                    channels.push_back(beauty ? var.name + "." + rgba[c] : std::string(rgba[c]));
                    sources.push_back({color.data(), 1, 4, c});
                }
                beauty = true;
            } else if (var.source == Source::Depth) {
                channels.push_back(var.name + ".Z");
                sources.push_back({depth.data(), 0, 1, 0});
            } else if (var.source == Source::PrimId || var.source == Source::InstanceId) {
                channels.push_back(var.name + ".R");
                sources.push_back({var.source == Source::PrimId ? primId.data() : instanceId.data(), 2, 1, 0});
            } else if (!_aovs[var.kind].empty()) {
                const uint32_t components = uint32_t(_aovs[var.kind].size() / pixels);
                const std::string layer = IsCryptomatte(var.kind) ? var.name + "00" : var.name;
                for (uint32_t c = 0; c < components; ++c) {
                    channels.push_back(layer + "." + (components == 1 ? "Z" : rgba[c]));
                    sources.push_back({_aovs[var.kind].data(), 0, components, c});
                }
                if (IsCryptomatte(var.kind)) {
                    char key[16];
                    std::snprintf(key, sizeof(key), "%08x", vkr::CryptomatteHash(var.name));
                    const std::string prefix = "cryptomatte/" + std::string(key, 7) + "/";
                    metadata.push_back({prefix + "name", var.name});
                    metadata.push_back({prefix + "hash", "MurmurHash3_32"});
                    metadata.push_back({prefix + "conversion", "uint32_to_float32"});
                    metadata.push_back({prefix + "manifest", var.kind == vkr::Aovs::kCryptoObject ? objectManifest
                                                                                            : materialManifest});
                }
            }
        }
        if (channels.empty()) {
            continue;
        }
        // Interleaved, top row first (the renderer's are bottom first).
        const size_t n = channels.size();
        std::vector<float> image(pixels * n);
        for (uint32_t y = 0; y < _height; ++y) {
            for (uint32_t x = 0; x < _width; ++x) {
                const size_t from = size_t(_height - 1 - y) * _width + x, to = (size_t(y) * _width + x) * n;
                for (size_t c = 0; c < n; ++c) {
                    const Channel &s = sources[c];
                    const size_t i = from * s.stride + s.offset;
                    float v;
                    if (s.type == 1) {
                        GfHalf h;
                        h.setBits(static_cast<const uint16_t *>(s.data)[i]);
                        v = float(h);
                    } else if (s.type == 2) {
                        v = float(static_cast<const int32_t *>(s.data)[i]);
                    } else {
                        v = static_cast<const float *>(s.data)[i];
                    }
                    image[to + c] = v;
                }
            }
        }
        std::unique_ptr<OIIO::ImageOutput> file = OIIO::ImageOutput::create(product.path);
        OIIO::ImageSpec spec(int(_width), int(_height), int(n), OIIO::TypeDesc::FLOAT);
        spec.channelnames = channels;
        spec.alpha_channel = beauty ? 3 : -1;
        spec.attribute("compression", "zip");
        for (const auto &[key, value] : metadata) {
            spec.attribute(key, value);
        }
        if (!file || !file->open(product.path, spec) || !file->write_image(OIIO::TypeDesc::FLOAT, image.data()) ||
            !file->close()) {
            TF_RUNTIME_ERROR("HdVk: writing %s failed: %s", product.path.c_str(),
                             file ? file->geterror().c_str() : OIIO::geterror().c_str());
        }
    }
#else
    TF_WARN("HdVk: built without OpenImageIO; render products aren't written");
#endif
}

PXR_NAMESPACE_CLOSE_SCOPE
