#pragma once

#include "renderer.h"

#include "pxr/pxr.h"
#include "pxr/imaging/hd/renderSettings.h"

#include <string>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

// The render settings prim (UsdRenderSettings); HdRenderSettings' own Sync
// flattens its products and render vars.
class HdVkRenderSettings final : public HdRenderSettings
{
public:
    explicit HdVkRenderSettings(SdfPath const &id) : HdRenderSettings(id) {}

protected:
    // Nothing of its own (the base's isn't exported).
    void _Sync(HdSceneDelegate *, HdRenderParam *, const HdDirtyBits *) override {}
};

// UsdRender products of the active render settings (usdrecord
// --renderSettingsPrimPath, husk): each raster product's render vars that
// Eldr can fill go into one EXR (any file OpenImageIO writes), written when
// the frame has converged; again after it changed and converged anew.
// Render vars name their AOV as UsdRender does (sourceType "lpe": "lpe:" +
// sourceName, "primvar": "primvars:" + sourceName, else sourceName): Hydra's
// color, depth, primId, instanceId and Eldr's extra AOVs (HdVkExtraAovs,
// Karma's names). Cryptomatte vars become layers <name>00..02 (ranks 0-5, soft edges under the film)
// with the spec's metadata (cryptomatte/<key>/name, hash, conversion,
// manifest).
// With eldr:productResolution the frame renders at the first raster
// product's Framing (renderPass.cpp), and the EXR's display window is the
// product's resolution, its data window the part rendered.
// ponytail: one size for all products (the first's); an extra AOV Hydra
// binds itself as well isn't in the product (one target per kind); vars
// Eldr can't fill are left out with a warning.
class HdVkRenderProducts
{
public:
    // The image of the first raster product of `settings` (active, with a
    // resolution): its resolution, pixel aspect ratio, conform policy, and
    // the pixels of its data window (dataWindowNDC), x0 y0 from the
    // bottom left, x1 y1 exclusive. False without one.
    struct Framing {
        GfVec2i resolution{0, 0};
        float pixelAspectRatio = 1.0f;
        TfToken conformPolicy;
        int window[4] = {0, 0, 0, 0};
        uint32_t Width() const { return uint32_t(window[2] - window[0]); }
        uint32_t Height() const { return uint32_t(window[3] - window[1]); }
    };
    // `size`, if given, replaces the product's resolution (husk --res: the
    // buffers' size is the render's; the camera conforms to its aspect).
    static bool GetFraming(const HdRenderSettings *settings, Framing *framing, const GfVec2i *size = nullptr);
    // Products render at `framing` (null: at the frame's size, as an image
    // of its own).
    void SetFraming(const Framing *framing) { _framing = framing ? *framing : Framing(); }

    // This frame's products (`settings` null or inactive: none): asks `out`
    // for the extra AOVs they need that no Hydra buffer asked for.
    void Prepare(const HdRenderSettings *settings, uint32_t width, uint32_t height, vkr::Renderer::Targets *out);
    bool Needs(vkr::Aovs::Kind kind) const { return _need[kind]; }
    // Whether a product has a beauty var (Prepare's).
    bool HasBeauty() const { return _beauty; }
    // After Render: writes the products once `renderer` converged on what
    // `frame` (its scene version and camera) shows. `linear`: the beauty is
    // the frame's linear one (Frame::linearColor).
    void Write(vkr::Renderer &renderer, const vkr::Renderer::Frame &frame, const std::string &objectManifest,
               const std::string &materialManifest, bool linear);

private:
    enum class Source { Color, Depth, PrimId, InstanceId, Aov };
    struct Var {
        std::string name;  // the render var prim's: the EXR layer
        Source source;
        uint32_t kind;     // vkr::Aovs::Kind with Source::Aov
    };
    struct Product {
        std::string path;
        std::vector<Var> vars;
    };
    std::vector<Product> _products;
    bool _beauty = false;
    bool _need[vkr::Aovs::kCount] = {};
    std::vector<float> _aovs[vkr::Aovs::kCount];  // the extra AOVs the products asked for
    uint32_t _width = 0, _height = 0;
    Framing _framing;  // resolution 0: the frame's size
    bool _written = false;
    uint64_t _writtenKey = 0;  // the frame _written is of: a new one (a post blur frame converges at once) writes again
};

PXR_NAMESPACE_CLOSE_SCOPE
