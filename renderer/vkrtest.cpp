// Standalone check of the renderer layer, no USD needed: renders small
// scenes and checks pixels, depth, row order, PBR normalization, textures.
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
    float At(int x, int y, int c) const { return HalfToFloat(color[(y * 64 + x) * 4 + c]); }
};

static Image Render(vkr::Renderer &renderer, const vkr::Scene &scene)
{
    vkr::Renderer::Frame frame;
    frame.width = frame.height = 64;
    const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    std::copy(identity, identity + 16, frame.viewProj);  // world = clip space
    frame.cameraPos[0] = frame.cameraPos[1] = 0;
    frame.cameraPos[2] = 5;
    frame.clearColor[2] = frame.clearColor[3] = 1;  // opaque blue
    Image img;
    renderer.Render(scene, frame, img.color.data(), img.depth.data());
    return img;
}

// Light along +z (towards the camera) with E = pi, so a Lambert surface
// returns exactly its albedo.
static void AddFrontLight(vkr::Scene &scene)
{
    vkr::Light *light = scene.AddLight();
    std::fill(light->irradiance, light->irradiance + 3, 3.14159265f);
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
            AddFrontLight(scene);

            // Triangle with its apex up (+y), z = 0, no normals (flat).
            vkr::Mesh *tri = scene.AddMesh();
            tri->points = {-0.5f, -0.5f, 0, 0.5f, -0.5f, 0, 0, 0.5f, 0};
            tri->color[0] = 1;
            tri->color[1] = tri->color[2] = 0;

            // Arrays that disagree in size must be dropped, not drawn.
            vkr::Mesh *bad = scene.AddMesh();
            bad->points = {0, 0, 0, 1, 0, 0, 0, 1, 0};
            bad->uvs = {0, 0};
            scene.Commit();
            Expect(scene.GetGpu().drawCount == 1, "inconsistent mesh skipped");

            const Image img = Render(renderer, scene);
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

            scene.RemoveMesh(tri);
            scene.Commit();
            Expect(scene.GetGpu().drawCount == 0, "removed mesh not drawn");
        }

        {
            vkr::Scene scene(&device);
            AddFrontLight(scene);

            // Full-screen quad, st (0,0) at the bottom left.
            vkr::Mesh *quad = scene.AddMesh();
            quad->points = {-1, -1, 0, 1, -1, 0, 1, 1, 0, -1, -1, 0, 1, 1, 0, -1, 1, 0};
            quad->uvs = {0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1};

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
    } catch (const std::exception &e) {
        std::printf("FAIL: %s\n", e.what());
        return 1;
    }

    std::puts(failures ? "FAILED" : "OK");
    return failures ? 1 : 0;
}
