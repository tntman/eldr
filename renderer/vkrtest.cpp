// Standalone check of the renderer layer, no USD needed: renders small
// scenes and checks pixels, depth, row order, PBR normalization, textures,
// instancing, image-based lighting, shadows and the sky orientation.
#include "renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <vector>

static int failures = 0;

static void Expect(bool ok, const char *what)
{
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

static float HalfToFloat(uint16_t h)
{
    const int exponent = (h >> 10) & 0x1F, mantissa = h & 0x3FF;
    const float sign = (h & 0x8000) ? -1.0f : 1.0f;
    if (exponent == 0) {
        return sign * std::ldexp(float(mantissa), -24);
    }
    return sign * std::ldexp(float(mantissa | 0x400), exponent - 25);
}

struct Image {
    std::vector<uint16_t> color = std::vector<uint16_t>(64 * 64 * 4);  // RGBA16F
    std::vector<float> depth = std::vector<float>(64 * 64);
    std::vector<int32_t> primId = std::vector<int32_t>(64 * 64);
    std::vector<int32_t> instanceId = std::vector<int32_t>(64 * 64);
    float At(int x, int y, int c) const { return HalfToFloat(color[(y * 64 + x) * 4 + c]); }
    int32_t Prim(int x, int y) const { return primId[y * 64 + x]; }
    int32_t Inst(int x, int y) const { return instanceId[y * 64 + x]; }
};

// Identity camera: world == GL clip space, looking down +Z from z = -2.
static vkr::Renderer::Frame IdentityFrame()
{
    vkr::Renderer::Frame frame;
    frame.width = frame.height = 64;
    const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    std::copy(identity, identity + 16, frame.viewProj);
    std::copy(identity, identity + 16, frame.invViewProj);
    frame.cameraPos[0] = frame.cameraPos[1] = 0;
    frame.cameraPos[2] = -2;
    frame.clearColor[2] = frame.clearColor[3] = 1;  // opaque blue
    frame.postProcess = false;  // exact linear values; post has its own tests
    return frame;
}

// 90 degree perspective at the origin looking down -Z (GL projection,
// row-vector order) and its exact inverse.
static vkr::Renderer::Frame PerspectiveFrame()
{
    vkr::Renderer::Frame frame = IdentityFrame();
    const float n = 0.1f, f = 100.0f;
    const float c = -(f + n) / (f - n), d = -2 * f * n / (f - n);
    const float proj[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, c, -1, 0, 0, d, 0};
    const float inv[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1 / d, 0, 0, -1, c / d};
    std::copy(proj, proj + 16, frame.viewProj);
    std::copy(inv, inv + 16, frame.invViewProj);
    frame.cameraPos[2] = 0;
    return frame;
}

static Image Render(vkr::Renderer &renderer, const vkr::Scene &scene,
                    const vkr::Renderer::Frame &frame = IdentityFrame())
{
    Image img;
    vkr::Renderer::Targets out;
    out.color = img.color.data();
    out.depth = img.depth.data();
    out.primId = img.primId.data();
    out.instanceId = img.instanceId.data();
    renderer.Render(scene, frame, out);
    return img;
}

static vkr::Light *AddLight(vkr::Scene &scene, float x, float y, float z)
{
    vkr::Light *light = scene.AddLight();
    const float len = std::sqrt(x * x + y * y + z * z);
    light->direction[0] = x / len;
    light->direction[1] = y / len;
    light->direction[2] = z / len;
    std::fill(light->irradiance, light->irradiance + 3, 3.14159265f);  // Lambert returns albedo
    return light;
}

static vkr::Mesh *AddQuad(vkr::Scene &scene, float x0, float y0, float x1, float y1, float z)
{
    vkr::Mesh *quad = scene.AddMesh();
    quad->points = {x0, y0, z, x1, y0, z, x1, y1, z, x0, y0, z, x1, y1, z, x0, y1, z};
    quad->uvs = {0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1};
    return quad;
}

// Sphere light of surface radiance L, emitting along +z (towards the
// quads the tests put behind it).
static vkr::Light *AddSphereLight(vkr::Scene &scene, float x, float y, float z, float radius, float L)
{
    vkr::Light *light = scene.AddLight();
    light->type = vkr::Light::Type::Sphere;
    light->position[0] = x;
    light->position[1] = y;
    light->position[2] = z;
    light->radius = radius;
    std::fill(light->radiance, light->radiance + 3, L);
    return light;
}

// World x (or y) of a pixel center under IdentityFrame.
static float PixelNdc(int p)
{
    return (float(p) + 0.5f) / 32.0f - 1.0f;
}

// Black, unlit quad that only emits `radiance`.
static vkr::Mesh *AddEmitter(vkr::Scene &scene, float x0, float y0, float x1, float y1, float z,
                             float radiance)
{
    vkr::Mesh *quad = AddQuad(scene, x0, y0, x1, y1, z);
    auto material = std::make_shared<vkr::Material>();
    std::fill(material->baseColor, material->baseColor + 3, 0.0f);
    std::fill(material->emissive, material->emissive + 3, radiance);
    quad->material = material;
    return quad;
}

// Post-processed render through a fresh renderer, repeated until converged
// like usdrecord does: exposure snaps on the first frame, TAA accumulates
// 16 jittered frames of the still camera.
static Image RenderPost(vkr::Device &device, const vkr::Scene &scene, float bloom = 0.04f,
                        bool taa = true, bool ssr = true)
{
    vkr::Renderer renderer(device);
    vkr::Renderer::Frame frame = IdentityFrame();
    frame.postProcess = true;
    frame.taa = taa;
    frame.ssr = ssr;
    frame.post.bloomIntensity = bloom;
    Image img;
    int frames = 0;
    do {
        img = Render(renderer, scene, frame);
        ++frames;
    } while (!renderer.IsConverged() && frames < 64);
    Expect(frames == (taa ? 16 : 1), taa ? "TAA converges after 16 still frames"
                                         : "without TAA the first frame is final");
    return img;
}

int main()
{
    const vkr::DeviceInfo info = vkr::ProbeDevice();
    if (!info.supported) {
        std::printf("unsupported: %s\n", info.reasonWhyNot.c_str());
        return 1;
    }
    std::printf("%s, Vulkan %u.%u.%u\n", info.name.c_str(),
                VK_API_VERSION_MAJOR(info.apiVersion),
                VK_API_VERSION_MINOR(info.apiVersion),
                VK_API_VERSION_PATCH(info.apiVersion));

    try {
        vkr::Device device;
        vkr::Renderer renderer(device);

        {
            vkr::Scene scene(&device);
            AddLight(scene, 0, 0, -1);  // towards the camera

            // Triangle with its apex up (+y), z = 0, no normals (flat).
            vkr::Mesh *tri = scene.AddMesh();
            tri->points = {-0.5f, -0.5f, 0, 0.5f, -0.5f, 0, 0, 0.5f, 0};
            tri->color[0] = 1;
            tri->color[1] = tri->color[2] = 0;
            tri->primId = 7;

            // Arrays that disagree in size must be dropped, not drawn.
            vkr::Mesh *bad = scene.AddMesh();
            bad->points = {0, 0, 0, 1, 0, 0, 0, 1, 0};
            bad->uvs = {0, 0};
            scene.Commit();

            const Image img = Render(renderer, scene);
            Expect(renderer.VisibleInstances() == 1, "inconsistent mesh skipped");
            // Default material (roughness 0.5, f0 0.04) at normal incidence:
            // diffuse = 1, specular = D*V*F*E = (1/(pi*a^2)) * 0.25 * 0.04 * pi = 0.16.
            Expect(std::abs(img.At(32, 32, 0) - 1.16f) < 0.02f, "red = albedo + specular");
            Expect(std::abs(img.At(32, 32, 1) - 0.16f) < 0.02f, "green = specular only");
            Expect(img.At(0, 0, 0) == 0 && img.At(0, 0, 2) == 1, "corner is clear color");
            Expect(std::abs(img.depth[32 * 64 + 32] - 0.5f) < 0.01f, "center depth 0.5");
            Expect(img.depth[0] == 1.0f, "corner depth is clear depth");
            // Row 20 is NDC y = -0.36 (inside the wide base), row 56 is y = +0.77
            // (above the apex). Swapped results mean the image is upside down.
            Expect(img.At(32, 20, 0) > 0.5f, "row 20 (lower half) is inside");
            Expect(img.At(32, 56, 2) == 1, "row 56 (upper half) is outside");
            Expect(img.Prim(32, 32) == 7 && img.Inst(32, 32) == 0, "ids where the triangle is");
            Expect(img.Prim(0, 0) == -1 && img.Inst(0, 0) == -1, "ids are -1 where nothing is");

            // Two instances of the triangle, shifted left and right: one draw.
            tri->transforms = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -0.5f, 0, 0, 1,
                               1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0.5f, 0, 0, 1};
            tri->transformsDirty = true;
            scene.Commit();
            const Image twice = Render(renderer, scene);
            Expect(renderer.VisibleInstances() == 2, "both instances survive culling");
            Expect(twice.At(16, 32, 0) > 0.5f && twice.At(48, 32, 0) > 0.5f, "both instances drawn");
            Expect(twice.At(32, 50, 2) == 1, "gap between instances");
            Expect(twice.Inst(16, 32) == 0 && twice.Inst(48, 32) == 1, "instanceId follows transforms order");
            Expect(twice.Prim(16, 32) == 7 && twice.Prim(48, 32) == 7, "both instances keep the primId");

            // A third instance far off to the side is culled, not drawn.
            tri->transforms.insert(tri->transforms.end(),
                                   {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 50.0f, 0, 0, 1});
            tri->transformsDirty = true;
            scene.Commit();
            Render(renderer, scene);
            Expect(renderer.VisibleInstances() == 2, "off-screen instance culled");

            // Editing geometry in place: the same mesh now covers the top.
            tri->transforms = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            tri->transformsDirty = true;
            tri->points = {-1, 0.5f, 0, 1, 0.5f, 0, 0, 1, 0};
            tri->geometryDirty = true;
            scene.Commit();
            const Image edited = Render(renderer, scene);
            Expect(edited.Prim(32, 56) == 7 && edited.Prim(32, 20) == -1, "geometry edit uploaded");

            scene.RemoveMesh(tri);
            scene.Commit();
            Render(renderer, scene);
            Expect(renderer.VisibleInstances() == 0, "removed mesh not drawn");

            // A new mesh reuses the freed pool ranges and draws correctly.
            vkr::Mesh *again = scene.AddMesh();
            again->points = {-0.5f, -0.5f, 0, 0.5f, -0.5f, 0, 0, 0.5f, 0};
            again->primId = 9;
            scene.Commit();
            const Image reused = Render(renderer, scene);
            Expect(reused.Prim(32, 32) == 9 && renderer.VisibleInstances() == 1, "freed ranges reused");
        }

        {
            // GPU skinning: a quad whose top corners follow joint 1 and bottom
            // corners are split 50/50 between joints 0 and 1. Joint 0 stays,
            // joint 1 lifts by 0.4: the top edge rises 0.4, the bottom 0.2.
            vkr::Scene scene(&device);
            AddLight(scene, 0, 0, -1);
            vkr::Mesh *quad = AddQuad(scene, -0.5f, -0.5f, 0.5f, 0.5f, 0);
            quad->primId = 3;
            quad->jointCount = 2;
            for (size_t c = 0; c < quad->points.size() / 3; ++c) {
                const bool top = quad->points[c * 3 + 1] > 0;
                quad->jointIndices.insert(quad->jointIndices.end(), {0, 1, 0, 0});
                quad->jointWeights.insert(quad->jointWeights.end(), {top ? 0.0f : 0.5f, top ? 1.0f : 0.5f, 0, 0});
            }
            auto pose = [](float lift) {  // joint 0 stays, joint 1 lifts
                return std::vector<float>{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1,
                                          1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, lift, 0, 1};
            };
            auto at = [](float x, float scale = 1.0f) {
                return std::vector<float>{scale, 0, 0, 0, 0, scale, 0, 0, 0, 0, scale, 0, x, 0, 0, 1};
            };
            quad->skinningTransforms = pose(0.4f);
            scene.Commit();
            Image img = Render(renderer, scene);
            // Rows: y = -0.3 + 0.2 (bottom edge) .. 0.5 + 0.4 (top edge).
            Expect(img.Prim(32, 58) == 3 && img.Prim(32, 62) == -1, "skinned top edge follows joint 1 (y 0.9)");
            Expect(img.Prim(32, 17) == -1 && img.Prim(32, 22) == 3, "skinned bottom edge halfway (y -0.3)");

            // Two agents sharing the pose, one far off screen: culled by the
            // bounds its joints imply.
            quad->transforms = at(-0.5f);
            const std::vector<float> far = at(50.0f);
            quad->transforms.insert(quad->transforms.end(), far.begin(), far.end());
            quad->transformsDirty = true;
            scene.Commit();
            img = Render(renderer, scene);
            Expect(img.Prim(4, 58) == 3 && img.Inst(4, 58) == 0 && img.Prim(40, 32) == -1, "agent 0 moved left");
            Expect(renderer.VisibleInstances() == 1, "agent 1 culled from its joint bounds");

            // Each agent with its own pose (per-instance skinning transforms):
            // the second, now next to the first, does not lift.
            quad->transforms = at(-0.5f);
            const std::vector<float> right = at(0.5f), still = pose(0.0f);
            quad->transforms.insert(quad->transforms.end(), right.begin(), right.end());
            quad->transformsDirty = true;
            quad->skinningTransforms.insert(quad->skinningTransforms.end(), still.begin(), still.end());
            scene.Commit();
            img = Render(renderer, scene);
            Expect(img.Prim(16, 58) == 3 && img.Prim(48, 58) == -1 && img.Prim(48, 40) == 3,
                   "per-instance poses: left agent lifted, right one not");

            // An agent scaled down to a fraction of a pixel is culled too.
            quad->transforms = at(0.0f, 0.005f);
            quad->transformsDirty = true;
            quad->skinningTransforms = pose(0.4f);
            scene.Commit();
            Render(renderer, scene);
            Expect(renderer.VisibleInstances() == 0, "sub-pixel agent culled");

            // Blend shapes on the rest points before skinning: one shape lifts
            // the top corners by 0.4 (weight 1) while the joints stay put.
            quad->transforms = at(0.0f);
            quad->transformsDirty = true;
            quad->skinningTransforms = pose(0.0f);
            quad->blendOffsets = {0, 0.4f, 0, 0};
            quad->blendRanges.clear();
            for (size_t c = 0; c < quad->points.size() / 3; ++c) {
                const bool top = quad->points[c * 3 + 1] > 0;
                quad->blendRanges.insert(quad->blendRanges.end(), {0u, top ? 1u : 0u});
            }
            quad->blendShapeCount = 1;
            quad->blendWeights = {1.0f};
            quad->geometryDirty = true;
            scene.Commit();
            img = Render(renderer, scene);
            Expect(img.Prim(32, 58) == 3, "blend shape at weight 1 lifts the top edge");
            quad->blendWeights = {0.0f};
            quad->jointsDirty = true;
            scene.Commit();
            img = Render(renderer, scene);
            Expect(img.Prim(32, 58) == -1 && img.Prim(32, 40) == 3, "blend shape at weight 0 leaves it");
        }

        {
            // Many meshes and instances: forces the pools to grow past their
            // first capacity (1024) while earlier contents must survive.
            vkr::Scene scene(&device);
            AddLight(scene, 0, 0, -1);
            for (int i = 0; i < 40; ++i) {
                vkr::Mesh *m = AddQuad(scene, -0.06f, -0.06f, 0.06f, 0.06f, 0);
                m->primId = i;
                m->transforms.clear();
                for (int k = 0; k < 64; ++k) {  // 40 * 64 = 2560 instances
                    const float x = -0.9f + 1.8f * float(k % 8) / 7.0f, y = -0.9f + 1.8f * float(k / 8) / 7.0f;
                    m->transforms.insert(m->transforms.end(), {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0,
                                                               x, y, float(i) * 0.001f, 1});
                }
            }
            scene.Commit();
            const Image img = Render(renderer, scene);
            Expect(renderer.VisibleInstances() == 2560, "pool growth keeps every instance");
            // Pixel 3 is centered at NDC -0.89, inside the first grid quad.
            Expect(img.Prim(3, 3) == 0, "nearest mesh wins the depth test after growth");
        }

        {
            vkr::Scene scene(&device);
            AddLight(scene, 0, 0, -1);

            // Full-screen quad, st (0,0) at the bottom left.
            vkr::Mesh *quad = AddQuad(scene, -1, -1, 1, 1, 0);

            // 2x2 texture, rows bottom-up: bottom row red, top row green.
            auto tex = std::make_shared<vkr::Texture>();
            tex->width = tex->height = 2;
            tex->pixels = {255, 0, 0, 255, 255, 0, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255};
            auto material = std::make_shared<vkr::Material>();
            material->roughness = 1;  // specular ~0.01 at normal incidence
            material->baseColorTex.texture = tex;
            quad->material = material;
            scene.Commit();
            Expect(scene.GetGpu().textures.size() == 1, "one texture bound");

            const Image img = Render(renderer, scene);
            // Rows 16 and 48 sample texel centers exactly (v = 0.25, 0.75).
            Expect(img.At(32, 16, 0) > 0.95f && img.At(32, 16, 1) < 0.05f, "bottom of quad is red");
            Expect(img.At(32, 48, 1) > 0.95f && img.At(32, 48, 0) < 0.05f, "top of quad is green");
        }

        {
            // Per-vertex displayColor: bottom corners red, top corners green.
            vkr::Scene scene(&device);
            AddLight(scene, 0, 0, -1);
            vkr::Mesh *quad = AddQuad(scene, -1, -1, 1, 1, 0);
            for (size_t c = 0; c < quad->points.size() / 3; ++c) {
                const bool top = quad->points[c * 3 + 1] > 0;
                quad->colors.insert(quad->colors.end(), {top ? 0.0f : 1.0f, top ? 1.0f : 0.0f, 0.0f});
            }
            scene.Commit();
            const Image img = Render(renderer, scene);
            Expect(img.At(32, 2, 0) > 4 * img.At(32, 2, 1) && img.At(32, 61, 1) > 4 * img.At(32, 61, 0),
                   "per-vertex displayColor interpolates bottom red to top green");
        }

        {
            // White furnace: a uniform dome of radiance 1 and a white surface
            // must return exactly 1 (energy conservation incl. multi-scatter).
            vkr::Scene scene(&device);
            scene.AddDome();
            vkr::Mesh *quad = AddQuad(scene, -1, -1, 1, 1, 0);
            auto material = std::make_shared<vkr::Material>();
            std::fill(material->baseColor, material->baseColor + 3, 1.0f);
            material->roughness = 0.7f;
            quad->material = material;
            scene.Commit();
            Expect(scene.GetGpu().environment != VK_NULL_HANDLE, "dome builds an environment");
            float v = Render(renderer, scene).At(32, 32, 0);
            Expect(std::abs(v - 1.0f) < 0.03f, "furnace: white dielectric returns 1");
            material->metallic = 1;
            scene.Commit();
            v = Render(renderer, scene).At(32, 32, 0);
            Expect(std::abs(v - 1.0f) < 0.03f, "furnace: white metal returns 1");
        }

        {
            // GTAO: a wall (z = 0.5) whose left half is covered by a ledge
            // 0.2 in front of it, under a uniform dome. The wall right next
            // to the ledge is occluded; beyond the 0.5 radius it is not, and
            // neither is the ledge's own flat face.
            vkr::Scene scene(&device);
            scene.AddDome();
            auto material = std::make_shared<vkr::Material>();
            std::fill(material->baseColor, material->baseColor + 3, 0.5f);
            material->roughness = 1;
            AddQuad(scene, -1, -1, 1, 1, 0.5f)->material = material;
            AddQuad(scene, -1, -1, 0, 1, 0.3f)->material = material;
            scene.Commit();
            vkr::Renderer::Frame frame = IdentityFrame();
            const Image on = Render(renderer, scene, frame);
            frame.ambientOcclusion = false;
            const Image off = Render(renderer, scene, frame);
            auto ratio = [&](int x) { return on.At(x, 32, 0) / off.At(x, 32, 0); };
            Expect(ratio(34) < 0.9f, "GTAO darkens the wall next to the ledge");
            Expect(ratio(60) > 0.98f, "GTAO leaves the open wall alone");
            Expect(ratio(16) > 0.98f, "GTAO leaves the flat ledge alone");
        }

        {
            // Local lights 0.5 in front of a white, fully rough wall (z = 0.5);
            // no dome, no post. Specular adds ~1% at roughness 1.
            vkr::Scene scene(&device);
            auto white = std::make_shared<vkr::Material>();
            std::fill(white->baseColor, white->baseColor + 3, 1.0f);
            white->roughness = 1;
            AddQuad(scene, -1, -1, 1, 1, 0.5f)->material = white;

            // Sphere: reflected radiance L R^2 / d^2 * cos.
            vkr::Light *sphere = AddSphereLight(scene, 0, 0, 0, 0.1f, 10.0f);
            scene.Commit();
            Image img = Render(renderer, scene);
            const float x = PixelNdc(48), d2 = 0.25f + x * x;
            const float offAxis = 10.0f * 0.01f / d2 * (0.5f / std::sqrt(d2));
            Expect(std::abs(img.At(32, 32, 0) - 0.4f) < 0.01f, "sphere light: L R^2 / d^2 head-on");
            Expect(std::abs(img.At(48, 32, 0) / offAxis - 1.0f) < 0.03f, "sphere light: inverse square and cosine");

            // Spot: 30 degree cone, hard edge, pointing at the wall.
            sphere->coneAngle = 30;
            scene.Commit();
            img = Render(renderer, scene);
            Expect(img.At(38, 32, 0) > 0.2f, "spot: lit inside the cone (22 degrees)");
            Expect(img.At(48, 32, 0) == 0.0f, "spot: dark outside the cone (46 degrees)");
            scene.RemoveLight(sphere);

            // Rect: 0.5 x 0.5 square facing the wall. Form factor of a square
            // of half-size a at distance h: 4 * 1/(2 pi) * 2 x/sqrt(1+x^2) atan(x/sqrt(1+x^2)).
            vkr::Light *rect = scene.AddLight();
            rect->type = vkr::Light::Type::Rect;
            rect->halfWidth[0] = 0.25f;
            rect->halfHeight[1] = 0.25f;
            std::fill(rect->radiance, rect->radiance + 3, 2.0f);
            scene.Commit();
            const float t = 0.5f / std::sqrt(1.25f);
            const float formFactor = 4.0f / (2.0f * 3.14159265f) * 2.0f * t * std::atan(t);
            Expect(std::abs(Render(renderer, scene).At(32, 32, 0) / (2.0f * formFactor) - 1.0f) < 0.03f,
                   "rect light: exact form factor head-on");
            rect->direction[2] = -1;  // now faces away from the wall
            scene.Commit();
            Expect(Render(renderer, scene).At(32, 32, 0) == 0.0f, "rect light: dark behind its back");

            // Disk of radius r head-on: reflected radiance L r^2 / (r^2 + h^2).
            rect->type = vkr::Light::Type::Disk;
            rect->direction[2] = 1;
            scene.Commit();
            Expect(std::abs(Render(renderer, scene).At(32, 32, 0) / (2.0f * 0.0625f / 0.3125f) - 1.0f) < 0.03f,
                   "disk light: exact illuminance head-on");
            scene.RemoveLight(rect);

            // 256 small lights 0.1 in front of the wall, each reaching ~0.2:
            // every pixel must get the sum over all lights, so clustering
            // dropped none. Range as in lights.glsl (cutoff 0.0018 without post).
            const float R = 0.01f, L = 0.72f, range = R * std::sqrt(L / 0.0018f);
            for (int i = 0; i < 256; ++i) {
                AddSphereLight(scene, -0.9375f + 0.125f * float(i % 16), -0.9375f + 0.125f * float(i / 16),
                               0.4f, R, L);
            }
            scene.Commit();
            img = Render(renderer, scene);
            float worst = 0.0f;
            for (int py : {5, 20, 33, 47, 60}) {
                for (int px : {3, 18, 31, 44, 62}) {
                    float expected = 0.0f;
                    for (int i = 0; i < 256; ++i) {
                        const float dx = PixelNdc(px) - (-0.9375f + 0.125f * float(i % 16));
                        const float dy = PixelNdc(py) - (-0.9375f + 0.125f * float(i / 16));
                        const float dd = dx * dx + dy * dy + 0.01f, dist = std::sqrt(dd);
                        const float window = std::max(1.0f - std::pow(dist / range, 4.0f), 0.0f);
                        expected += L * R * R / dd * (0.1f / dist) * window * window;
                    }
                    worst = std::max(worst, std::abs(img.At(px, py, 0) / expected - 1.0f));
                }
            }
            Expect(worst < 0.03f, "clustered lights: every pixel gets all lights in range");
        }

        {
            // Local light shadows: light at (-0.6, 0, 0), a small occluder at
            // z = 0.25 around x = -0.3, the wall at z = 0.5. The shadow lands
            // around x = 0, where the camera sees the wall past the occluder.
            vkr::Scene scene(&device);
            auto white = std::make_shared<vkr::Material>();
            std::fill(white->baseColor, white->baseColor + 3, 1.0f);
            white->roughness = 1;
            AddQuad(scene, -1, -1, 1, 1, 0.5f)->material = white;
            AddQuad(scene, -0.35f, -0.05f, -0.25f, 0.05f, 0.25f)->material = white;
            for (const float cone : {45.0f, 180.0f}) {  // one spot tile, then cube faces
                vkr::Light *light = AddSphereLight(scene, -0.6f, 0, 0, 0.02f, 400.0f);
                const float len = std::sqrt(0.61f);
                light->direction[0] = 0.6f / len;
                light->direction[1] = 0;
                light->direction[2] = 0.5f / len;
                light->coneAngle = cone;
                light->castShadows = true;
                scene.Commit();
                const Image shadowed = Render(renderer, scene);
                light->castShadows = false;
                scene.Commit();
                const Image open = Render(renderer, scene);
                Expect(shadowed.At(32, 32, 0) < 0.2f * shadowed.At(41, 32, 0),
                       cone < 90 ? "spot light shadow (atlas tile)" : "point light shadow (cube faces)");
                Expect(open.At(32, 32, 0) > 0.5f * open.At(41, 32, 0) &&
                           std::abs(open.At(41, 32, 0) - shadowed.At(41, 32, 0)) < 0.01f * open.At(41, 32, 0),
                       "outside the shadow, lighting is unchanged");
                scene.RemoveLight(light);
            }

            // A big sphere light (same power) softens the shadow's edges:
            // many more partly lit pixels along the row than from a point.
            auto penumbra = [&](float radius) {
                vkr::Light *light = AddSphereLight(scene, -0.6f, 0, 0, radius, 0.16f / (radius * radius));
                light->point = radius < 0.05f;
                light->castShadows = true;
                scene.Commit();
                const Image img = Render(renderer, scene);
                const float open = img.At(41, 32, 0);
                int partial = 0;
                for (int x = 22; x < 42; ++x) {
                    const float v = img.At(x, 32, 0) / open;
                    partial += v > 0.1f && v < 0.9f;
                }
                scene.RemoveLight(light);
                return partial;
            };
            const int hard = penumbra(0.02f), soft = penumbra(0.15f);
            Expect(soft >= hard + 3, "area light shadows have a penumbra (PCSS)");
        }

        {
            // Shadow: occluder at z = -0.5 in front of a ground quad at z = 0.5,
            // light from the camera side tilted towards +x, so the shadow lands
            // one unit to the left (-x) of the occluder.
            vkr::Scene scene(&device);
            AddLight(scene, 1, 0, -1)->castShadows = true;
            AddQuad(scene, -1, -1, 1, 1, 0.5f)->color[0] = 0.8f;
            AddQuad(scene, -0.25f, -0.25f, 0.25f, 0.25f, -0.5f);
            AddQuad(scene, 0.95f, 0.95f, 0.99f, 0.99f, 0.9f);  // keeps the ground off the far edge
            scene.Commit();
            const Image img = Render(renderer, scene);
            const float shadowed = img.At(3, 32, 0), lit = img.At(51, 32, 0);
            Expect(lit > 0.3f, "ground outside the shadow is lit");
            Expect(shadowed < 0.1f * lit, "ground behind the occluder is shadowed");
        }

        {
            // Sky orientation (Storm's latlong mapping), 4x2 texture rows
            // bottom-up: bottom row red; top row green for u < 0.5, cyan above.
            vkr::Scene scene(&device);
            auto tex = std::make_shared<vkr::Texture>();
            tex->width = 4;
            tex->height = 2;
            tex->pixels = {255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255,
                           0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 255, 255, 0, 255, 255, 255};
            scene.AddDome()->texture = tex;
            scene.Commit();
            const Image img = Render(renderer, scene, PerspectiveFrame());
            Expect(img.At(32, 4, 0) > 0.8f && img.At(32, 4, 1) < 0.2f, "below the horizon is the bottom row");
            // Looking down -Z: the right of the view is u ~ 0.12, the left u ~ 0.88.
            // (Bilinear filtering still mixes in ~12% of the red bottom row up here.)
            Expect(img.At(60, 60, 1) > 0.8f && img.At(60, 60, 2) < 0.1f, "upper right is u < 0.5");
            Expect(img.At(3, 60, 1) > 0.8f && img.At(3, 60, 2) > 0.75f, "upper left is u > 0.5");
        }

        {
            // Auto-exposure maps a uniform screen to middle gray whatever its
            // brightness: ACES(0.18) = 0.106 for both.
            for (float radiance : {1.0f, 100.0f}) {
                vkr::Scene scene(&device);
                AddEmitter(scene, -1, -1, 1, 1, 0, radiance);
                scene.Commit();
                const float v = RenderPost(device, scene).At(32, 32, 1);
                Expect(std::abs(v - 0.106f) < 0.01f, radiance > 1 ? "exposure: bright screen -> 0.106"
                                                                   : "exposure: dim screen -> 0.106");
            }
        }

        {
            // The app's background colour passes through and doesn't steer
            // exposure: geometry covers the lower half only.
            vkr::Scene scene(&device);
            AddEmitter(scene, -1, -1, 1, 0, 0, 50.0f);
            scene.Commit();
            const Image img = RenderPost(device, scene);
            Expect(img.At(32, 60, 2) == 1.0f && img.At(32, 60, 0) == 0.0f, "background passes through");
            Expect(std::abs(img.At(32, 8, 1) - 0.106f) < 0.01f, "exposure from geometry only");
        }

        {
            // TAA anti-aliases edges: a slanted emitter edge gets coverage
            // values between background (G = 0) and geometry (G = 0.106).
            vkr::Scene scene(&device);
            vkr::Mesh *tri = AddEmitter(scene, 0, 0, 0, 0, 0, 1.0f);
            tri->points = {-1, -1, 0, 1, -1, 0, -1, 0.6f, 0};
            tri->uvs.clear();
            scene.Commit();
            auto partial = [](const Image &img) {
                int n = 0;
                for (int y = 0; y < 64; ++y) {
                    for (int x = 0; x < 64; ++x) {
                        const float g = img.At(x, y, 1);
                        n += g > 0.01f && g < 0.095f;
                    }
                }
                return n;
            };
            const int smooth = partial(RenderPost(device, scene, 0.0f, true));
            const int hard = partial(RenderPost(device, scene, 0.0f, false));
            Expect(smooth > 30 && hard < 5, "TAA turns the hard edge into partial coverage");
        }

        {
            // SSR: a mirror tilted towards -y (normal (0, -1, -1)) above an
            // emitter tilted up towards it. View rays (+z) reflect straight
            // down and hit the emitter, which is on screen. No dome, so the
            // mirror is black without SSR.
            vkr::Scene scene(&device);
            vkr::Mesh *mirror = AddQuad(scene, 0, 0, 0, 0, 0);
            mirror->points = {-0.8f, 0, 0, 0.8f, 0, 0, 0.8f, 0.8f, -0.8f,
                              -0.8f, 0, 0, 0.8f, 0.8f, -0.8f, -0.8f, 0.8f, -0.8f};
            auto chrome = std::make_shared<vkr::Material>();
            std::fill(chrome->baseColor, chrome->baseColor + 3, 1.0f);
            chrome->metallic = 1;
            chrome->roughness = 0;
            mirror->material = chrome;
            vkr::Mesh *emitter = AddEmitter(scene, 0, 0, 0, 0, 0, 1.0f);
            emitter->points = {-0.8f, -0.8f, -0.6f, 0.8f, -0.8f, -0.6f, 0.8f, -0.1f, 0.1f,
                               -0.8f, -0.8f, -0.6f, 0.8f, -0.1f, 0.1f, -0.8f, -0.1f, 0.1f};
            scene.Commit();
            // Row 41 is y = 0.3 on the mirror; its reflection lands on the
            // emitter at y = -0.5.
            const float on = RenderPost(device, scene, 0.0f).At(32, 41, 1);
            const float off = RenderPost(device, scene, 0.0f, true, false).At(32, 41, 1);
            const float emitterValue = RenderPost(device, scene, 0.0f).At(32, 16, 1);
            Expect(off < 0.01f, "mirror without SSR reflects only the black (absent) dome");
            Expect(on > 0.8f * emitterValue, "SSR reflects the emitter in the mirror");
        }

        {
            // Bloom: a tiny very bright spot on a dim screen spreads light.
            vkr::Scene scene(&device);
            AddEmitter(scene, -1, -1, 1, 1, 0.5f, 0.01f);
            AddEmitter(scene, -0.06f, -0.06f, 0.06f, 0.06f, 0, 1000.0f);
            scene.Commit();
            const float off = RenderPost(device, scene, 0.0f).At(32, 44, 1);
            const float on = RenderPost(device, scene, 0.04f).At(32, 44, 1);
            Expect(on > 1.5f * off, "bloom brightens pixels around a bright spot");
        }
    } catch (const std::exception &e) {
        std::printf("FAIL: %s\n", e.what());
        return 1;
    }

    std::puts(failures ? "FAILED" : "OK");
    return failures ? 1 : 0;
}
