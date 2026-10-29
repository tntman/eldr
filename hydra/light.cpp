#include "light.h"
#include "timing.h"

#include "material.h"
#include "renderDelegate.h"
#include "scene.h"

#include "pxr/base/gf/color.h"
#include "pxr/base/gf/half.h"
#include "pxr/base/tf/diagnostic.h"
#include "pxr/base/gf/colorSpace.h"
#include "pxr/base/gf/math.h"
#include "pxr/base/gf/matrix4d.h"
#include "pxr/base/gf/matrix4f.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/imaging/hd/sceneDelegate.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/usd/sdf/assetPath.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

constexpr float kPi = 3.14159265f;

// Blackbody color with Rec709 luminance 1 (as hdEmbree does).
GfVec3f Blackbody(float kelvin)
{
    GfColor color{GfColorSpace(GfColorSpaceNames->LinearRec709)};
    color.SetFromPlanckianLocus(kelvin, 1.0f);
    const GfVec3f rgb = color.GetRGB();
    return rgb / GfDot(rgb, GfVec3f(0.2126f, 0.7152f, 0.0722f));
}

void Copy(const GfVec3d &v, float out[3])
{
    out[0] = float(v[0]);
    out[1] = float(v[1]);
    out[2] = float(v[2]);
}

// An IES profile (UsdLux shaping:ies:file) baked for vkr::Light::ies:
// relative intensity over kIesPhi x kIesTheta texels, its peak and its
// power (mean intensity, for shaping:ies:normalize). Cached per file for
// the process (a few KB each; the GPU may still read a replaced one).
// ponytail: LM-63 type C photometry only (A and B are read as C, with a
// warning); TILT ignored; bilinear in the file's angles, as hdEmbree.
struct IesProfile {
    std::shared_ptr<vkr::Texture> texture;
    float peak = 0.0f, power = 0.0f;
};
constexpr uint32_t kIesPhi = 64, kIesTheta = 256;
// Candela to UsdLux intensity: hdEmbree's (Cycles') factor.
constexpr float kIesCandela = 0.0706650768394f;

IesProfile LoadIes(const std::string &path)
{
    static std::mutex mutex;
    static std::map<std::string, IesProfile> cache;
    std::lock_guard<std::mutex> lock(mutex);
    const auto found = cache.find(path);
    if (found != cache.end()) {
        return found->second;
    }
    IesProfile &profile = cache[path];
    std::ifstream in(path);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const size_t tilt = text.find("TILT=");
    const size_t eol = tilt == std::string::npos ? tilt : text.find('\n', tilt);
    if (eol == std::string::npos) {
        TF_WARN("Eldr: can't read the IES profile '%s'", path.c_str());
        return profile;
    }
    std::istringstream numbers(text.substr(eol + 1));
    std::vector<double> v;
    for (double x; numbers >> x;) {
        v.push_back(x);
    }
    size_t at = 0;
    if (text.compare(tilt, 12, "TILT=INCLUDE") == 0 && v.size() > 1) {  // lamp geometry, pairs, angles, factors
        at = 2 + 2 * size_t(v[1]);
    }
    auto next = [&] { return at < v.size() ? v[at++] : 0.0; };
    next();  // lamps
    next();  // lumens per lamp
    const double multiplier = next();
    const size_t nV = size_t(next()), nH = size_t(next());
    const int type = int(next());
    for (int k = 0; k < 4; ++k) {
        next();  // units, width, length, height
    }
    const double factor = multiplier * next();  // ballast factor
    next();  // future use
    next();  // input watts
    if (nV < 2 || nH < 1 || at + nV + nH + nV * nH > v.size()) {
        TF_WARN("Eldr: can't read the IES profile '%s'", path.c_str());
        return profile;
    }
    if (type != 1) {
        TF_WARN("Eldr: IES profile '%s' has type %s photometry; read as type C", path.c_str(), type == 2 ? "B" : "A");
    }
    const double *vAngles = &v[at], *hAngles = &v[at + nV], *candela = &v[at + nV + nH];
    // Type C symmetry by the last horizontal angle: 0 all around, 90 per
    // quadrant, 180 mirrored across the 0-180 plane, else as given.
    const double lastH = hAngles[nH - 1];
    auto span = [](const double *a, size_t n, double x, size_t &i, double &f) {
        i = std::clamp<size_t>(size_t(std::upper_bound(a, a + n, x) - a), 1, std::max<size_t>(n - 1, 1)) - 1;
        f = n < 2 || a[i + 1] <= a[i] ? 0.0 : std::clamp((x - a[i]) / (a[i + 1] - a[i]), 0.0, 1.0);
    };
    auto lookup = [&](double theta, double phi) {
        if (theta < vAngles[0] || theta > vAngles[nV - 1]) {
            return 0.0;
        }
        if (lastH <= 0.0) {
            phi = 0.0;
        } else if (lastH <= 90.0) {
            phi = phi > 180.0 ? 360.0 - phi : phi;
            phi = phi > 90.0 ? 180.0 - phi : phi;
        } else if (lastH <= 180.0) {
            phi = phi > 180.0 ? 360.0 - phi : phi;
        }
        size_t vi, hi;
        double fv, fh;
        span(vAngles, nV, theta, vi, fv);
        span(hAngles, nH, phi, hi, fh);
        const size_t hj = std::min(hi + 1, nH - 1), vj = std::min(vi + 1, nV - 1);
        auto c = [&](size_t h, size_t k) { return candela[h * nV + k]; };
        const double a = c(hi, vi) + (c(hi, vj) - c(hi, vi)) * fv;
        const double b = c(hj, vi) + (c(hj, vj) - c(hj, vi)) * fv;
        return (a + (b - a) * fh) * factor;
    };
    const uint32_t width = nH == 1 ? 1 : kIesPhi;
    std::vector<float> baked(size_t(width) * kIesTheta);
    double power = 0.0;
    for (uint32_t j = 0; j < kIesTheta; ++j) {
        const double theta = (j + 0.5) * 180.0 / kIesTheta;
        for (uint32_t i = 0; i < width; ++i) {
            const float value = float(std::max(lookup(theta, (i + 0.5) * 360.0 / width), 0.0));
            baked[size_t(j) * width + i] = value;
            profile.peak = std::max(profile.peak, value);
            power += value * std::sin(GfDegreesToRadians(theta));
        }
    }
    if (!(profile.peak > 0.0f)) {
        TF_WARN("Eldr: IES profile '%s' is dark", path.c_str());
        return profile;
    }
    // The mean intensity over the sphere, or over the hemisphere a profile
    // within 90 degrees (and a bit) covers, as hdEmbree (which matches Karma).
    const bool sphere = vAngles[nV - 1] - vAngles[0] > 90.0 + GfRadiansToDegrees(0.1);
    profile.power = float(power * (kPi / kIesTheta) * (2.0 * kPi / width) / ((sphere ? 4.0 : 2.0) * kPi));
    auto texture = std::make_shared<vkr::Texture>();
    texture->width = width;
    texture->height = kIesTheta;
    texture->format = vkr::Texture::Format::R16F;
    texture->pixels.resize(baked.size() * sizeof(GfHalf));
    auto *texels = reinterpret_cast<GfHalf *>(texture->pixels.data());
    for (size_t k = 0; k < baked.size(); ++k) {
        texels[k] = GfHalf(baked[k] / profile.peak);
    }
    profile.texture = std::move(texture);
    return profile;
}

} // namespace

HdVkLight::HdVkLight(SdfPath const &id, TfToken const &type, vkr::Scene *scene,
                     HdVkTextureCache *textures)
    : HdLight(id)
    , _type(type)
    , _scene(scene)
    , _textures(textures)
{
    // Fallback sprims never sync; they must not light anything.
    if (type == HdPrimTypeTokens->domeLight) {
        _dome = scene->AddDome();
        _dome->visible = false;
    } else {
        _light = scene->AddLight();
        _light->visible = false;
    }
}

HdVkLight::~HdVkLight()
{
    if (_dome) {
        _scene->RemoveDome(_dome);
    } else {
        _scene->RemoveLight(_light);
    }
}

// UsdLux semantics as in hdEmbree: emitted radiance is color * intensity *
// 2^exposure (* blackbody), divided by the light's area if normalize.
// ponytail: ignores dome poleAxis/portals and shadow color/distance; domes
// ignore light linking.
void HdVkLight::Sync(HdSceneDelegate *sceneDelegate, HdRenderParam *, HdDirtyBits *dirtyBits)
{
    HdVkTimedCall timed(HdVkCallTimes::kSync);
    if (!(*dirtyBits & (DirtyTransform | DirtyParams | DirtyShadowParams | DirtyCollection | DirtyResource))) {
        *dirtyBits = Clean;
        return;
    }
    HdVkRenderDelegate::NoteSceneEdit(sceneDelegate);
    const SdfPath &id = GetId();
    auto param = [&](TfToken const &name, auto fallback) {
        return sceneDelegate->GetLightParamValue(id, name).GetWithDefault(fallback);
    };
    // "Render Light Geometry": inputs:eldr:light:renderlightgeo when authored,
    // else Karma's inputs:karma:light:renderlightgeo, else shown.
    auto renderLightGeo = [&] {
        const VtValue eldr = sceneDelegate->GetLightParamValue(id, TfToken("inputs:eldr:light:renderlightgeo"));
        return eldr.IsEmpty() ? param(TfToken("inputs:karma:light:renderlightgeo"), true)
                              : eldr.GetWithDefault(true);
    };
    // Its light group: inputs:eldr:light:lpetag when authored, else Karma's
    // LPE tag (inputs:karma:light:lpetag; Karma's C.*<L.'tag'>).
    auto lpeTag = [&] {
        for (const char *name : {"inputs:eldr:light:lpetag", "inputs:karma:light:lpetag"}) {
            const VtValue v = sceneDelegate->GetLightParamValue(id, TfToken(name));
            if (v.IsHolding<std::string>()) {
                return v.UncheckedGet<std::string>();
            }
            if (v.IsHolding<TfToken>()) {
                return v.UncheckedGet<TfToken>().GetString();
            }
        }
        return std::string();
    };
    const GfMatrix4d xf = sceneDelegate->GetTransform(id);
    GfVec3f color = param(HdLightTokens->color, GfVec3f(1.0f)) * param(HdLightTokens->intensity, 1.0f) *
                    std::exp2(param(HdLightTokens->exposure, 0.0f));
    if (param(HdLightTokens->enableColorTemperature, false)) {
        color = GfCompMult(color, Blackbody(param(HdLightTokens->colorTemperature, 6500.0f)));
    }
    const bool visible = sceneDelegate->GetVisible(id);
    // inputs:texture:file (domes, rects, disks).
    auto texture = [&]() -> std::shared_ptr<vkr::Texture> {
        const VtValue file = param(HdLightTokens->textureFile, VtValue());
        std::string path;
        if (file.IsHolding<SdfAssetPath>()) {
            const SdfAssetPath &p = file.UncheckedGet<SdfAssetPath>();
            path = p.GetResolvedPath().empty() ? p.GetAssetPath() : p.GetResolvedPath();
        }
        return path.empty() ? nullptr : _textures->Get(path, TfToken());
    };
    const bool normalize = param(HdLightTokens->normalize, false);

    if (_dome) {
        const GfMatrix4f toLocal(xf.GetInverse());
        std::copy(toLocal.data(), toLocal.data() + 16, _dome->worldToLocal);
        std::copy(color.data(), color.data() + 3, _dome->radiance);
        _dome->texture = texture();
        _dome->visible = visible;
        // Karma's "Render Light Geometry" hides the dome from the camera only.
        // Karma's schema default is off; Eldr shows it unless authored false.
        _dome->cameraVisible = renderLightGeo();
        _dome->lpeTag = lpeTag();
        *dirtyBits = Clean;
        return;
    }

    vkr::Light &light = *_light;
    light.visible = visible;
    // Karma's "Render Light Geometry" (inputs:karma:light:renderlightgeo,
    // bool): false hides the light's shape from the camera. UsdLux has no
    // such attribute. ponytail: it can only hide; the shapes still need
    // eldr:lightShapesVisible.
    light.cameraVisible = renderLightGeo();
    light.lpeTag = lpeTag();
    // Caustic photons: inputs:eldr:light:contributescaustics when authored,
    // else Karma's inputs:karma:light:contributescaustics, else on.
    {
        const VtValue eldr = sceneDelegate->GetLightParamValue(id, TfToken("inputs:eldr:light:contributescaustics"));
        light.caustics = eldr.IsEmpty() ? param(TfToken("inputs:karma:light:contributescaustics"), true)
                                        : eldr.GetWithDefault(true);
    }
    light.diffuse = param(HdLightTokens->diffuse, 1.0f);
    // Light linking: UsdLux collection:lightLink and collection:shadowLink
    // as HdsiLightLinkingSceneIndex's categories (empty: everything).
    auto category = [&](TfToken const &name) {
        const VtValue v = sceneDelegate->GetLightParamValue(id, name);
        return v.IsHolding<TfToken>() ? HdVkLinkCategory(v.UncheckedGet<TfToken>(), true) : 0u;
    };
    light.lightLink = category(HdTokens->lightLink);
    light.shadowLink = category(HdTokens->shadowLink);
    light.specular = param(HdLightTokens->specular, 1.0f);
    if (_type == HdPrimTypeTokens->distantLight) {
        // Shines down its local -Z; store the direction to it. Intensity is
        // the sun disk's radiance: integrate over its solid angle unless
        // normalized.
        light.type = vkr::Light::Type::Distant;
        Copy(xf.TransformDir(GfVec3d(0, 0, 1)).GetNormalized(), light.direction);
        light.angle = param(HdLightTokens->angle, 0.53f);
        const float halfAngle = float(GfDegreesToRadians(light.angle)) * 0.5f;
        if (halfAngle > 0.0f && !normalize) {
            color *= 2.0f * kPi * (1.0f - std::cos(halfAngle));
        }
        std::copy(color.data(), color.data() + 3, light.irradiance);
        light.castShadows = param(HdLightTokens->shadowEnable, true);
        *dirtyBits = Clean;
        return;
    }
    light.castShadows = param(HdLightTokens->shadowEnable, true);

    Copy(xf.Transform(GfVec3d(0.0)), light.position);
    Copy(xf.TransformDir(GfVec3d(0, 0, -1)).GetNormalized(), light.direction);
    float area;
    if (_type == HdPrimTypeTokens->sphereLight) {
        const double scale = (xf.TransformDir(GfVec3d(1, 0, 0)).GetLength() +
                              xf.TransformDir(GfVec3d(0, 1, 0)).GetLength() +
                              xf.TransformDir(GfVec3d(0, 0, 1)).GetLength()) / 3.0;
        light.type = vkr::Light::Type::Sphere;
        light.radius = float(param(HdLightTokens->radius, 0.5f) * scale);
        // Not a light input, so it's looked up as the plain attribute.
        light.point = param(TfToken("treatAsPoint"), false);
        area = 4.0f * kPi * light.radius * light.radius;
    } else if (_type == HdPrimTypeTokens->cylinderLight) {
        // Axis along local X, emitting from its side only (UsdLux).
        const double scale = (xf.TransformDir(GfVec3d(0, 1, 0)).GetLength() +
                              xf.TransformDir(GfVec3d(0, 0, 1)).GetLength()) / 2.0;
        const GfVec3d halfAxis = xf.TransformDir(GfVec3d(0.5 * param(HdLightTokens->length, 1.0f), 0, 0));
        light.type = vkr::Light::Type::Cylinder;
        light.radius = float(param(HdLightTokens->radius, 0.5f) * scale);
        Copy(halfAxis, light.halfWidth);
        light.point = param(TfToken("treatAsLine"), false);
        area = float(2.0 * kPi * light.radius * 2.0 * halfAxis.GetLength());
    } else {
        // Rect: half extents; disk: radius along X and Y.
        const bool disk = _type == HdPrimTypeTokens->diskLight;
        const double radius = param(HdLightTokens->radius, 0.5f);
        const GfVec3d halfWidth = xf.TransformDir(GfVec3d(disk ? radius : 0.5 * param(HdLightTokens->width, 1.0f), 0, 0));
        const GfVec3d halfHeight = xf.TransformDir(GfVec3d(0, disk ? radius : 0.5 * param(HdLightTokens->height, 1.0f), 0));
        light.type = disk ? vkr::Light::Type::Disk : vkr::Light::Type::Rect;
        light.texture = texture();
        Copy(halfWidth, light.halfWidth);
        Copy(halfHeight, light.halfHeight);
        area = float((disk ? kPi : 4.0) * GfCross(halfWidth, halfHeight).GetLength());
    }
    if (normalize && area > 0.0f) {
        color /= area;
    }
    std::copy(color.data(), color.data() + 3, light.radiance);
    // IES profile (UsdLux shaping:ies:*), phi from the light's +X.
    light.ies = nullptr;
    const VtValue iesFile = param(HdLightTokens->shapingIesFile, VtValue());
    if (iesFile.IsHolding<SdfAssetPath>()) {
        const SdfAssetPath &p = iesFile.UncheckedGet<SdfAssetPath>();
        const std::string path = p.GetResolvedPath().empty() ? p.GetAssetPath() : p.GetResolvedPath();
        const IesProfile ies = path.empty() ? IesProfile() : LoadIes(path);
        if (ies.texture) {
            light.ies = ies.texture;
            light.iesScale = param(HdLightTokens->shapingIesNormalize, false) ? ies.peak / ies.power
                                                                               : ies.peak * kIesCandela;
            light.iesAngleScale = param(HdLightTokens->shapingIesAngleScale, 0.0f);
            Copy(xf.TransformDir(GfVec3d(1, 0, 0)).GetNormalized(), light.iesX);
        }
    }
    light.coneAngle = param(HdLightTokens->shapingConeAngle, 180.0f);
    light.coneSoftness = param(HdLightTokens->shapingConeSoftness, 0.0f);
    light.focus = param(HdLightTokens->shapingFocus, 0.0f);
    const GfVec3f focusTint = param(HdLightTokens->shapingFocusTint, GfVec3f(0.0f));
    std::copy(focusTint.data(), focusTint.data() + 3, light.focusTint);
    *dirtyBits = Clean;
}

PXR_NAMESPACE_CLOSE_SCOPE
