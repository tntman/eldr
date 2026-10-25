#include "light.h"

#include "material.h"
#include "scene.h"

#include "pxr/base/gf/color.h"
#include "pxr/base/gf/colorSpace.h"
#include "pxr/base/gf/math.h"
#include "pxr/base/gf/matrix4d.h"
#include "pxr/base/gf/matrix4f.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/imaging/hd/sceneDelegate.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/usd/sdf/assetPath.h"

#include <cmath>

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
// ponytail: ignores dome poleAxis/portals, shadow color/distance and IES
// profiles.
void HdVkLight::Sync(HdSceneDelegate *sceneDelegate, HdRenderParam *, HdDirtyBits *dirtyBits)
{
    if (!(*dirtyBits & (DirtyTransform | DirtyParams | DirtyResource))) {
        *dirtyBits = Clean;
        return;
    }
    const SdfPath &id = GetId();
    auto param = [&](TfToken const &name, auto fallback) {
        return sceneDelegate->GetLightParamValue(id, name).GetWithDefault(fallback);
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
        *dirtyBits = Clean;
        return;
    }

    vkr::Light &light = *_light;
    light.visible = visible;
    // Karma's "Render Light Geometry" (inputs:karma:light:renderlightgeo,
    // bool): false hides the light's shape from the camera. UsdLux has no
    // such attribute. ponytail: it can only hide; the shapes still need
    // hdvk:lightShapesVisible.
    light.cameraVisible = param(TfToken("inputs:karma:light:renderlightgeo"), true);
    light.diffuse = param(HdLightTokens->diffuse, 1.0f);
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
    light.coneAngle = param(HdLightTokens->shapingConeAngle, 180.0f);
    light.coneSoftness = param(HdLightTokens->shapingConeSoftness, 0.0f);
    light.focus = param(HdLightTokens->shapingFocus, 0.0f);
    const GfVec3f focusTint = param(HdLightTokens->shapingFocusTint, GfVec3f(0.0f));
    std::copy(focusTint.data(), focusTint.data() + 3, light.focusTint);
    *dirtyBits = Clean;
}

PXR_NAMESPACE_CLOSE_SCOPE
