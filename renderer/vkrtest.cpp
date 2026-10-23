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
                        bool taa = true)
{
    vkr::Renderer renderer(device);
    vkr::Renderer::Frame frame = IdentityFrame();
    frame.postProcess = true;
    frame.taa = taa;
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
