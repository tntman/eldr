#include "light.h"

#include "material.h"
#include "scene.h"

#include "pxr/base/gf/math.h"
#include "pxr/base/gf/matrix4d.h"
#include "pxr/base/gf/matrix4f.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/imaging/hd/sceneDelegate.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/usd/sdf/assetPath.h"

#include <cmath>

PXR_NAMESPACE_OPEN_SCOPE

HdVkLight::HdVkLight(SdfPath const &id, TfToken const &type, vkr::Scene *scene,
                     HdVkTextureCache *textures)
    : HdLight(id)
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

// ponytail: ignores colorTemperature, diffuse/specular multipliers, shaping,
// dome poleAxis/portals and shadow color/distance.
void HdVkLight::Sync(HdSceneDelegate *sceneDelegate, HdRenderParam *, HdDirtyBits *dirtyBits)
{
    const SdfPath &id = GetId();
    auto param = [&](TfToken const &name, auto fallback) {
        return sceneDelegate->GetLightParamValue(id, name).GetWithDefault(fallback);
    };

    if (*dirtyBits & DirtyTransform) {
        const GfMatrix4d xf = sceneDelegate->GetTransform(id);
        if (_dome) {
            const GfMatrix4f toLocal(xf.GetInverse());
            std::copy(toLocal.data(), toLocal.data() + 16, _dome->worldToLocal);
        } else {
            // A distant light shines down its local -Z; store the direction to it.
            const GfVec3d d = xf.TransformDir(GfVec3d(0, 0, 1)).GetNormalized();
            _light->direction[0] = float(d[0]);
            _light->direction[1] = float(d[1]);
            _light->direction[2] = float(d[2]);
        }
    }

    if (*dirtyBits & (DirtyParams | DirtyResource)) {
        float scale = param(HdLightTokens->intensity, 1.0f) *
                      std::exp2(param(HdLightTokens->exposure, 0.0f));
        const GfVec3f color = param(HdLightTokens->color, GfVec3f(1.0f));
        const bool visible = sceneDelegate->GetVisible(id);
        if (_dome) {
            for (int i = 0; i < 3; ++i) {
                _dome->radiance[i] = color[i] * scale;
            }
            const VtValue file = param(HdLightTokens->textureFile, VtValue());
            std::string path;
            if (file.IsHolding<SdfAssetPath>()) {
                const SdfAssetPath &p = file.UncheckedGet<SdfAssetPath>();
                path = p.GetResolvedPath().empty() ? p.GetAssetPath() : p.GetResolvedPath();
            }
            _dome->texture = path.empty() ? nullptr : _textures->Get(path, TfToken());
            _dome->visible = visible;
        } else {
            // UsdLux intensity is the sun disk's radiance: integrate over its
            // solid angle unless normalized (same math as hdEmbree).
            const float halfAngle = float(GfDegreesToRadians(param(HdLightTokens->angle, 0.53f))) * 0.5f;
            if (halfAngle > 0.0f && !param(HdLightTokens->normalize, false)) {
                scale *= 6.28318531f * (1.0f - std::cos(halfAngle));  // 2*pi*(1 - cos)
            }
            for (int i = 0; i < 3; ++i) {
                _light->irradiance[i] = color[i] * scale;
            }
            _light->castShadows = param(HdLightTokens->shadowEnable, true);
            _light->visible = visible;
        }
    }

    *dirtyBits = Clean;
}

PXR_NAMESPACE_CLOSE_SCOPE
