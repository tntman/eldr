#include "light.h"

#include "scene.h"

#include "pxr/base/gf/math.h"
#include "pxr/base/gf/matrix4d.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/imaging/hd/sceneDelegate.h"
#include "pxr/imaging/hd/tokens.h"

#include <cmath>

PXR_NAMESPACE_OPEN_SCOPE

HdVkLight::HdVkLight(SdfPath const &id, vkr::Scene *scene)
    : HdLight(id)
    , _scene(scene)
    , _light(scene->AddLight())
{
    // Fallback sprims never sync; they must not light anything.
    _light->visible = false;
}

HdVkLight::~HdVkLight()
{
    _scene->RemoveLight(_light);
}

// ponytail: ignores colorTemperature, diffuse/specular multipliers, shaping
// and shadows (step 4).
void HdVkLight::Sync(HdSceneDelegate *sceneDelegate, HdRenderParam *, HdDirtyBits *dirtyBits)
{
    const SdfPath &id = GetId();
    vkr::Light &l = *_light;

    if (*dirtyBits & DirtyTransform) {
        // A distant light shines down its local -Z; store the direction to it.
        const GfVec3d d = sceneDelegate->GetTransform(id)
                              .TransformDir(GfVec3d(0, 0, 1)).GetNormalized();
        l.direction[0] = float(d[0]);
        l.direction[1] = float(d[1]);
        l.direction[2] = float(d[2]);
    }

    if (*dirtyBits & DirtyParams) {
        auto param = [&](TfToken const &name, auto fallback) {
            return sceneDelegate->GetLightParamValue(id, name).GetWithDefault(fallback);
        };
        float scale = param(HdLightTokens->intensity, 1.0f) *
                      std::exp2(param(HdLightTokens->exposure, 0.0f));
        // UsdLux intensity is the sun disk's radiance: integrate over its
        // solid angle unless normalized (same math as hdEmbree).
        const float halfAngle = float(GfDegreesToRadians(param(HdLightTokens->angle, 0.53f))) * 0.5f;
        if (halfAngle > 0.0f && !param(HdLightTokens->normalize, false)) {
            scale *= 6.28318531f * (1.0f - std::cos(halfAngle));  // 2*pi*(1 - cos)
        }
        const GfVec3f color = param(HdLightTokens->color, GfVec3f(1.0f));
        for (int i = 0; i < 3; ++i) {
            l.irradiance[i] = color[i] * scale;
        }
        l.visible = sceneDelegate->GetVisible(id);
    }

    *dirtyBits = Clean;
}

PXR_NAMESPACE_CLOSE_SCOPE
