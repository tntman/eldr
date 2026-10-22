// Standalone check of the renderer layer, no USD needed: draws one triangle
// and checks pixels, depth and the bottom-up row order Hydra expects.
#include "renderer.h"

#include <algorithm>
#include <cstdio>
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
        vkr::Scene scene(&device);
        vkr::Renderer renderer(device);

        // Triangle with its apex up (+y), z = 0, in clip space.
        vkr::Mesh *tri = scene.AddMesh();
        tri->points = {-0.5f, -0.5f, 0, 0.5f, -0.5f, 0, 0, 0.5f, 0};
        tri->indices = {0, 1, 2};
        tri->color[0] = 1;
        tri->color[1] = tri->color[2] = 0;

        // A mesh with an out-of-range index must be dropped, not drawn.
        vkr::Mesh *bad = scene.AddMesh();
        bad->points = {0, 0, 0};
        bad->indices = {0, 0, 7};
        scene.Commit();
        Expect(scene.GetGpu().drawCount == 1, "invalid mesh skipped");

        vkr::Renderer::Frame frame;
        frame.width = frame.height = 64;
        const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        std::copy(identity, identity + 16, frame.viewProj);
        frame.cameraPos[0] = frame.cameraPos[1] = 0;
        frame.cameraPos[2] = 5;
        frame.clearColor[2] = frame.clearColor[3] = 1;  // opaque blue

        std::vector<uint16_t> color(64 * 64 * 4);  // RGBA16F
        std::vector<float> depth(64 * 64);
        renderer.Render(scene, frame, color.data(), depth.data());

        auto px = [&](int x, int y) { return &color[(y * 64 + x) * 4]; };
        const uint16_t one = 0x3C00, half = 0x3800;  // 1.0 and 0.5 as half floats

        Expect(px(32, 32)[0] > half && px(32, 32)[1] == 0, "center is red");
        Expect(px(0, 0)[0] == 0 && px(0, 0)[2] == one, "corner is clear color");
        Expect(depth[32 * 64 + 32] > 0.49f && depth[32 * 64 + 32] < 0.51f,
               "center depth is 0.5 (z=0 in GL NDC)");
        Expect(depth[0] == 1.0f, "corner depth is clear depth");
        // Row 20 is NDC y = -0.36 (inside the wide base), row 56 is y = +0.77
        // (above the apex). Swapped results mean the image is upside down.
        Expect(px(32, 20)[0] > half, "row 20 (lower half) is inside");
        Expect(px(32, 56)[2] == one, "row 56 (upper half) is outside");

        // Removing a mesh and re-committing must leave nothing to draw.
        scene.RemoveMesh(tri);
        scene.Commit();
        Expect(scene.GetGpu().drawCount == 0, "removed mesh not drawn");
    } catch (const std::exception &e) {
        std::printf("FAIL: %s\n", e.what());
        return 1;
    }

    std::puts(failures ? "FAILED" : "OK");
    return failures ? 1 : 0;
}
