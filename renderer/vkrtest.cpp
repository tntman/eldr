// Standalone check of the renderer layer, no USD needed: renders small
// scenes and checks pixels, depth, row order, PBR normalization, textures,
// instancing, image-based lighting, shadows and the sky orientation.
#include "renderer.h"
#include "vkmath.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <memory>
#include <tuple>
#include <vector>

#define PNANOVDB_C
#include "PNanoVDB.h"

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
    std::vector<uint16_t> velocity = std::vector<uint16_t>(64 * 64 * 2);
    float Vel(int x, int y, int c) const { return HalfToFloat(velocity[(y * 64 + x) * 2 + c]); }
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
    const float toDistance[4] = {-1, -2, 0, 1};  // camera distance z + 2
    std::copy(toDistance, toDistance + 4, frame.depthToDistance);
    frame.clearColor[2] = frame.clearColor[3] = 1;  // opaque blue
    frame.postProcess = false;  // exact linear values; post has its own tests
    frame.adaptiveFinalThreshold = 0.0f;  // tests count a film's samples; the adaptive tests set it
    frame.lodFade = false;      // levels switch at once (the crossfade test turns it on)
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
    const float toDistance[4] = {inv[10], inv[14], inv[11], inv[15]};
    std::copy(toDistance, toDistance + 4, frame.depthToDistance);
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
    out.velocity = img.velocity.data();
    renderer.Render(scene, frame, out);
    return img;
}

// Largest colour difference between two images (0: identical colour).
static float MaxDifference(const Image &a, const Image &b)
{
    float worst = 0.0f;
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            for (int c = 0; c < 4; ++c) {
                worst = std::max(worst, std::abs(a.At(x, y, c) - b.At(x, y, c)));
            }
        }
    }
    return worst;
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
// like usdrecord does: exposure snaps on the first frame, the film
// accumulates Frame::filmSamples jittered frames of the still camera.
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
    } while (!renderer.IsConverged() && frames < 200);
    Expect(frames == (taa ? int(frame.filmSamples) : 1), taa ? "the film converges after filmSamples still frames"
                                                              : "without TAA the first frame is final");
    return img;
}

// NanoVDB float grid (laid out as PNanoVDB reads it) of a ball: `value`
// within `radius` voxels of (15.5, 15.5, 15.5), voxels in [0, 32)^3, 32
// per world unit, centred on the origin. One root tile, upper and lower
// node, and leaves only where the ball reaches (the rest are empty tiles).
static std::shared_ptr<vkr::VolumeGrid> BallGrid(float radius, float value)
{
    const pnanovdb_grid_type_constants_t &c = pnanovdb_grid_type_constants[PNANOVDB_GRID_TYPE_FLOAT];
    const size_t tree = PNANOVDB_GRID_SIZE, root = tree + PNANOVDB_TREE_SIZE;
    const size_t upper = root + c.root_size + c.root_tile_size, lower = upper + c.upper_size;
    const size_t leaves = lower + c.lower_size;
    std::vector<std::array<int, 3>> origins;  // leaves the ball reaches
    for (int x = 0; x < 32; x += 8) {
        for (int y = 0; y < 32; y += 8) {
            for (int z = 0; z < 32; z += 8) {
                const float dx = std::max({float(x) - 15.5f, 0.0f, 15.5f - float(x + 7)});
                const float dy = std::max({float(y) - 15.5f, 0.0f, 15.5f - float(y + 7)});
                const float dz = std::max({float(z) - 15.5f, 0.0f, 15.5f - float(z + 7)});
                if (dx * dx + dy * dy + dz * dz <= radius * radius) {
                    origins.push_back({x, y, z});
                }
            }
        }
    }
    const size_t total = leaves + origins.size() * c.leaf_size;
    std::vector<uint8_t> b(total, 0);
    auto put = [&](size_t at, auto v) { std::memcpy(&b[at], &v, sizeof(v)); };
    auto coord = [&](size_t at, int x, int y, int z) {
        put(at, int32_t(x));
        put(at + 4, int32_t(y));
        put(at + 8, int32_t(z));
    };
    put(PNANOVDB_GRID_OFF_MAGIC, uint64_t(PNANOVDB_MAGIC_NUMBER));
    put(PNANOVDB_GRID_OFF_VERSION, uint32_t(PNANOVDB_MAJOR_VERSION_NUMBER << 21 | PNANOVDB_MINOR_VERSION_NUMBER << 10));
    put(PNANOVDB_GRID_OFF_GRID_COUNT, uint32_t(1));
    put(PNANOVDB_GRID_OFF_GRID_SIZE, uint64_t(total));
    const double voxel = 1.0 / 32.0, offset = -15.5 / 32.0;
    for (int i = 0; i < 3; ++i) {
        const size_t map = PNANOVDB_GRID_OFF_MAP;
        put(map + PNANOVDB_MAP_OFF_MATF + 4 * (i * 4), float(voxel));
        put(map + PNANOVDB_MAP_OFF_INVMATF + 4 * (i * 4), float(1 / voxel));
        put(map + PNANOVDB_MAP_OFF_VECF + 4 * i, float(offset));
        put(map + PNANOVDB_MAP_OFF_MATD + 8 * (i * 4), voxel);
        put(map + PNANOVDB_MAP_OFF_INVMATD + 8 * (i * 4), 1 / voxel);
        put(map + PNANOVDB_MAP_OFF_VECD + 8 * i, offset);
        put(PNANOVDB_GRID_OFF_VOXEL_SIZE + 8 * i, voxel);
    }
    put(PNANOVDB_GRID_OFF_GRID_CLASS, uint32_t(PNANOVDB_GRID_CLASS_FOG_VOLUME));
    put(PNANOVDB_GRID_OFF_GRID_TYPE, uint32_t(PNANOVDB_GRID_TYPE_FLOAT));
    put(tree + PNANOVDB_TREE_OFF_NODE_OFFSET_LEAF, uint64_t(leaves - tree));
    put(tree + PNANOVDB_TREE_OFF_NODE_OFFSET_LOWER, uint64_t(lower - tree));
    put(tree + PNANOVDB_TREE_OFF_NODE_OFFSET_UPPER, uint64_t(upper - tree));
    put(tree + PNANOVDB_TREE_OFF_NODE_OFFSET_ROOT, uint64_t(root - tree));
    put(tree + PNANOVDB_TREE_OFF_NODE_COUNT_LEAF, uint32_t(origins.size()));
    put(tree + PNANOVDB_TREE_OFF_NODE_COUNT_LOWER, uint32_t(1));
    put(tree + PNANOVDB_TREE_OFF_NODE_COUNT_UPPER, uint32_t(1));
    coord(root + PNANOVDB_ROOT_OFF_BBOX_MIN, 0, 0, 0);
    coord(root + PNANOVDB_ROOT_OFF_BBOX_MAX, 31, 31, 31);
    put(root + PNANOVDB_ROOT_OFF_TABLE_SIZE, uint32_t(1));
    put(root + c.root_off_max, value);
    put(root + c.root_size + PNANOVDB_ROOT_TILE_OFF_CHILD, int64_t(upper - root));  // key 0: (0, 0, 0)
    coord(upper + PNANOVDB_UPPER_OFF_BBOX_MAX, 31, 31, 31);
    put(upper + PNANOVDB_UPPER_OFF_CHILD_MASK, uint32_t(1));
    put(upper + c.upper_off_table, int64_t(lower - upper));
    coord(lower + PNANOVDB_LOWER_OFF_BBOX_MAX, 31, 31, 31);
    for (size_t l = 0; l < origins.size(); ++l) {
        const auto [x, y, z] = origins[l];
        const size_t leaf = leaves + l * c.leaf_size;
        const uint32_t n = uint32_t((x >> 3) << 8 | (y >> 3) << 4 | (z >> 3));
        const size_t mask = lower + PNANOVDB_LOWER_OFF_CHILD_MASK + 4 * (n >> 5);
        uint32_t bits;
        std::memcpy(&bits, &b[mask], 4);
        put(mask, bits | 1u << (n & 31));
        put(lower + c.lower_off_table + 8 * n, int64_t(leaf - lower));
        coord(leaf + PNANOVDB_LEAF_OFF_BBOX_MIN, x, y, z);
        for (int v = 0; v < 512; ++v) {
            const float dx = float(x + (v >> 6)) - 15.5f, dy = float(y + ((v >> 3) & 7)) - 15.5f,
                        dz = float(z + (v & 7)) - 15.5f;
            put(leaf + c.leaf_off_table + 4 * v, dx * dx + dy * dy + dz * dz <= radius * radius ? value : 0.0f);
        }
    }
    auto grid = std::make_shared<vkr::VolumeGrid>();
    grid->words.resize(total / 4);
    std::memcpy(grid->words.data(), b.data(), total);
    return grid;
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

    // Pipelines cache in a folder of this run's (Device::LoadPipelineCache),
    // its own so runs in parallel don't share it.
    const std::filesystem::path cacheDir = std::filesystem::temp_directory_path() /
        ("eldr_vkrtest_pipelines_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code cacheError;
    std::filesystem::remove_all(cacheDir, cacheError);
#ifdef _WIN32
    _putenv_s("HDVK_PIPELINE_CACHE_DIR", cacheDir.string().c_str());
#else
    setenv("HDVK_PIPELINE_CACHE_DIR", cacheDir.string().c_str(), 1);
#endif

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
            const Image culled = Render(renderer, scene);
            Expect(renderer.VisibleInstances() == 2, "off-screen instance culled");
            vkr::Renderer::Frame everything = IdentityFrame();
            everything.frustumCulling = false;
            const Image unculled = Render(renderer, scene, everything);
            Expect(renderer.VisibleInstances() == 3 && MaxDifference(culled, unculled) == 0.0f,
                   "Frame::frustumCulling off: every instance drawn, the same image");

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
            // Memory accounting (usdview's GPU Stats): a texture counts as
            // device-local texture memory while it lives, a mapped buffer as
            // host memory; freed, both are gone.
            const vkr::Device::MemoryStats before = device.Memory();
            {
                vkr::Scene scene(&device);
                vkr::Mesh *quad = AddQuad(scene, -0.5f, -0.5f, 0.5f, 0.5f, 0);
                auto material = std::make_shared<vkr::Material>();
                material->baseColorTex.texture = std::make_shared<vkr::Texture>();
                vkr::Texture &t = *material->baseColorTex.texture;
                t.width = t.height = 64;
                t.pixels.assign(64 * 64 * 4, 255);
                quad->material = material;
                scene.Commit();
                const vkr::Device::MemoryStats loaded = device.Memory();
                Expect(loaded.textures >= before.textures + 64 * 64 * 4 && loaded.deviceLocal > before.deviceLocal &&
                           loaded.host > before.host,
                       "memory counts a scene's texture and buffers");
            }
            const vkr::Device::MemoryStats after = device.Memory();
            Expect(after.textures == before.textures && after.deviceLocal == before.deviceLocal &&
                       after.host == before.host,
                   "memory returns to where it was when the scene goes");
        }

        {
            // Indexed meshes (curves, points): per-vertex arrays and triangles.
            vkr::Scene scene(&device);
            AddLight(scene, 0, 0, -1);
            vkr::Mesh *quad = scene.AddMesh();
            quad->points = {-0.5f, -0.5f, 0, 0.5f, -0.5f, 0, 0.5f, 0.5f, 0, -0.5f, 0.5f, 0};
            quad->colors = {1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0};
            quad->indices = {0, 1, 2, 0, 2, 3};
            quad->primId = 7;
            scene.Commit();
            const Image img = Render(renderer, scene);
            Expect(img.Prim(20, 20) == 7 && img.Prim(44, 44) == 7 && img.Prim(10, 32) == -1 &&
                       img.At(32, 32, 0) > 2 * img.At(32, 32, 1),
                   "indexed quad");
            quad->indices = {0, 1, 4};  // past the vertices
            quad->geometryDirty = true;
            scene.Commit();
            Expect(Render(renderer, scene).Prim(32, 20) == -1, "indexed mesh with a bad index draws nothing");
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

            // Eight influences: the same weights spread over joints 0..7,
            // where 2, 4, 6 copy joint 0 and 3, 5, 7 joint 1: the same shape.
            {
                vkr::Mesh *wide = AddQuad(scene, -0.5f, -0.5f, 0.5f, 0.5f, 0);
                wide->primId = 4;
                wide->jointCount = 8;
                wide->influences = 8;
                for (size_t c = 0; c < wide->points.size() / 3; ++c) {
                    const float w0 = wide->points[c * 3 + 1] > 0 ? 0.0f : 0.125f, w1 = 0.25f - w0;
                    wide->jointIndices.insert(wide->jointIndices.end(), {0, 1, 2, 3, 4, 5, 6, 7});
                    wide->jointWeights.insert(wide->jointWeights.end(), {w0, w1, w0, w1, w0, w1, w0, w1});
                }
                for (int k = 0; k < 4; ++k) {
                    const std::vector<float> p = pose(0.4f);
                    wide->skinningTransforms.insert(wide->skinningTransforms.end(), p.begin(), p.end());
                }
                quad->visible = false;
                scene.Commit();
                const Image eight = Render(renderer, scene);
                Expect(eight.Prim(32, 58) == 4 && eight.Prim(32, 62) == -1 && eight.Prim(32, 17) == -1 &&
                           eight.Prim(32, 22) == 4,
                       "eight influences skin like the same four");
                scene.RemoveMesh(wide);
                quad->visible = true;
                scene.Commit();
            }

            // Dual quaternions: a strip weighted half to joint 0 (rest) and
            // half to joint 1 (turned 180 degrees about z), with four and
            // eight influences. Linear blending collapses it; dual
            // quaternions turn it 90 degrees at full size.
            for (const uint32_t influences : {4u, 8u}) {
                vkr::Mesh *dq = AddQuad(scene, -0.5f, -0.2f, 0.5f, 0.2f, 0);
                dq->primId = 5;
                dq->jointCount = 2;
                dq->influences = influences;
                dq->dualQuaternion = true;
                for (size_t c = 0; c < dq->points.size() / 3 * influences; ++c) {
                    dq->jointIndices.push_back(uint16_t(c % 2));
                    dq->jointWeights.push_back(1.0f / float(influences));
                }
                dq->skinningTransforms = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1,
                                          -1, 0, 0, 0, 0, -1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
                quad->visible = false;
                scene.Commit();
                const Image turned = Render(renderer, scene);
                Expect(turned.Prim(32, 44) == 5 && turned.Prim(32, 19) == 5 && turned.Prim(44, 32) == -1,
                       influences == 4 ? "dual quaternions turn the strip (4 influences)"
                                       : "dual quaternions turn the strip (8 influences)");
                dq->dualQuaternion = false;
                scene.Commit();
                const Image collapsed = Render(renderer, scene);
                Expect(collapsed.Prim(32, 44) == -1 && collapsed.Prim(44, 32) == -1 && collapsed.Prim(32, 32) == -1,
                       "linear blending collapses the same strip");
                scene.RemoveMesh(dq);
                quad->visible = true;
                scene.Commit();
            }

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

            // Cutout: the same quad with an opacity texture clear on the
            // left half, threshold 0.5: no surface there (not even in the
            // id targets), the right half stays.
            auto alpha = std::make_shared<vkr::Texture>();
            alpha->width = 2;
            alpha->height = 1;
            alpha->pixels = {255, 255, 255, 0, 255, 255, 255, 255};
            material->opacityTex.texture = alpha;
            material->opacityTex.channel = 3;
            material->opacityThreshold = 0.5f;
            quad->primId = 7;
            scene.Commit();
            const Image cut = Render(renderer, scene);
            Expect(cut.Prim(8, 32) == -1 && cut.Prim(56, 32) == quad->primId, "cutout: no surface below the threshold");

            // Compact formats: gray (+ alpha) textures of each kind read as a
            // gray rgb of 0.5.
            material->opacityThreshold = 0;
            material->opacityTex.texture = nullptr;
            using F = vkr::Texture::Format;
            const std::pair<F, std::vector<uint8_t>> grays[] = {
                {F::R8, {128}}, {F::RG8, {128, 255}}, {F::R16, {0x00, 0x80}}, {F::R16F, {0x00, 0x38}}};
            for (const auto &[format, bytes] : grays) {
                auto gray = std::make_shared<vkr::Texture>();
                gray->width = gray->height = 1;
                gray->format = format;
                gray->pixels = bytes;
                material->baseColorTex.texture = gray;
                scene.Commit();
                const Image img = Render(renderer, scene);
                const float r = img.At(32, 32, 0);
                Expect(std::abs(r - 0.5f) < 0.05f && std::abs(img.At(32, 32, 1) - r) < 0.01f &&
                           std::abs(img.At(32, 32, 2) - r) < 0.01f,
                       "compact gray texture formats read as gray rgb");
            }
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
            vkr::Renderer::Frame noIbl = IdentityFrame();
            noIbl.imageBasedLighting = false;
            Expect(Render(renderer, scene, noIbl).At(32, 32, 0) == 0.0f, "Frame::imageBasedLighting off: no dome light");
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
            frame.ambientOcclusion = true;
            frame.aoSlices = 1;
            Expect(Render(renderer, scene, frame).At(34, 32, 0) < 0.9f * off.At(34, 32, 0),
                   "Frame::aoSlices 1: GTAO still darkens next to the ledge");
            // Without the prepass the main pass writes depth: the same image,
            // depth and ids as with it (and no AO).
            frame.aoSlices = 3;
            frame.depthPrepass = false;
            const Image noPrepass = Render(renderer, scene, frame);
            Expect(MaxDifference(noPrepass, off) < 1e-3f && noPrepass.depth == off.depth && noPrepass.primId == off.primId,
                   "Frame::depthPrepass off: the same image, depth and ids, without AO");
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

            // Textured (inputs:texture:file), 2 wide; s = 0 is at +X (UsdLux): black over its +X
            // half (texture u < 0.5), white over its -X half. The wall under each half takes
            // its colour; Frame::lightTextures off: as without one.
            rect->halfWidth[0] = 1.0f;
            scene.Commit();
            const Image plain = Render(renderer, scene);
            auto halves = std::make_shared<vkr::Texture>();
            halves->width = 64;
            halves->height = 2;
            halves->pixels.assign(64 * 2 * 4, 255);
            for (int y = 0; y < 2; ++y) {
                for (int x = 0; x < 32; ++x) {
                    std::fill_n(&halves->pixels[(y * 64 + x) * 4], 3, uint8_t(0));
                }
            }
            rect->texture = halves;
            scene.Commit();
            const Image textured = Render(renderer, scene);
            Expect(textured.At(48, 32, 0) < 0.2f * textured.At(16, 32, 0) &&
                       std::abs(textured.At(16, 32, 0) / plain.At(16, 32, 0) - 1.0f) < 0.2f,
                   "rect light texture: dark under its black half, as bright under its white one");
            vkr::Renderer::Frame untextured = IdentityFrame();
            untextured.lightTextures = false;
            Expect(MaxDifference(Render(renderer, scene, untextured), plain) == 0.0f,
                   "Frame::lightTextures off: as without a texture");
            // As Karma: the image's top row is at +Y. Rows are stored bottom-up, so the last
            // rows are the image's top: white there, black below, and the wall under +Y is lit.
            rect->halfHeight[1] = 1.0f;
            auto tall = std::make_shared<vkr::Texture>();
            tall->width = 2;
            tall->height = 64;
            tall->pixels.assign(2 * 64 * 4, 255);
            for (int y = 0; y < 32; ++y) {
                std::fill_n(&tall->pixels[y * 2 * 4], 2 * 4, uint8_t(0));
            }
            rect->texture = tall;
            scene.Commit();
            const Image upright = Render(renderer, scene);
            Expect(upright.At(32, 16, 0) < 0.2f * upright.At(32, 48, 0) && upright.At(32, 48, 0) > 0.0f,  // +Y is the lower rows here
                   "rect light texture: the image's top row is at +Y (Karma)");
            rect->halfHeight[1] = 0.25f;
            rect->texture = nullptr;
            rect->halfWidth[0] = 0.25f;
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
            vkr::Renderer::Frame frame = IdentityFrame();
            frame.clusteredLights = false;
            Expect(MaxDifference(Render(renderer, scene, frame), img) < 1e-3f,
                   "Frame::clusteredLights off: every light per pixel, the same image");
            frame.clusteredLights = true;
            frame.localLights = false;
            Expect(Render(renderer, scene, frame).At(31, 33, 0) == 0.0f, "Frame::localLights off: no local light");
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
                vkr::Renderer::Frame smallTiles = IdentityFrame();
                smallTiles.localShadowResolution = 128;
                const Image coarse = Render(renderer, scene, smallTiles);
                Expect(coarse.At(32, 32, 0) < 0.2f * coarse.At(41, 32, 0),
                       "Frame::localShadowResolution 128: still shadowed");
                vkr::Renderer::Frame noLocalShadows = IdentityFrame();
                noLocalShadows.localShadows = false;
                const Image disabled = Render(renderer, scene, noLocalShadows);
                light->castShadows = false;
                scene.Commit();
                const Image open = Render(renderer, scene);
                Expect(std::abs(disabled.At(32, 32, 0) - open.At(32, 32, 0)) < 0.01f * open.At(41, 32, 0),
                       "Frame::localShadows off: no local light shadows");
                Expect(shadowed.At(32, 32, 0) < 0.2f * shadowed.At(41, 32, 0),
                       cone < 90 ? "spot light shadow (atlas tile)" : "point light shadow (cube faces)");
                Expect(open.At(32, 32, 0) > 0.5f * open.At(41, 32, 0) &&
                           std::abs(open.At(41, 32, 0) - shadowed.At(41, 32, 0)) < 0.01f * open.At(41, 32, 0),
                       "outside the shadow, lighting is unchanged");
                scene.RemoveLight(light);
            }

            {
                // 20 shadowed point lights (120 cube faces, more than 64
                // tiles of 512): each has a small occluder in front of its
                // own spot on the wall, whose shadow must be there for all.
                vkr::Scene many(&device);
                AddQuad(many, -1, -1, 1, 1, 0.5f)->material = white;
                for (int i = 0; i < 20; ++i) {
                    const float x = -0.8f + 0.4f * float(i % 5), y = -0.6f + 0.4f * float(i / 5);
                    AddQuad(many, x - 0.1f, y - 0.1f, x + 0.1f, y + 0.1f, 0.42f)->material = white;
                    vkr::Light *l = AddSphereLight(many, x, y, 0.35f, 0.02f, 1.0f);
                    l->castShadows = true;
                }
                many.Commit();
                vkr::Renderer::Frame off = IdentityFrame();
                off.localShadows = false;
                const Image shadowed = Render(renderer, many), open = Render(renderer, many, off);
                int dark = 0;
                for (int i = 0; i < 20; ++i) {
                    const int px = int(std::floor((-0.8f + 0.4f * float(i % 5) + 0.15f + 1.0f) * 32.0f));
                    const int py = int(std::floor((-0.6f + 0.4f * float(i / 5) + 1.0f) * 32.0f));
                    dark += shadowed.At(px, py, 0) < 0.4f * open.At(px, py, 0);
                }
                Expect(dark == 20, "20 shadowed point lights: every light's shadow is there");
            }

            // A big sphere light (same power) softens the shadow's edges:
            // many more partly lit pixels along the row than from a point.
            auto penumbra = [&](float radius, bool soft, uint32_t samples = 16) {
                vkr::Light *light = AddSphereLight(scene, -0.6f, 0, 0, radius, 0.16f / (radius * radius));
                light->point = radius < 0.05f;
                light->castShadows = true;
                scene.Commit();
                vkr::Renderer::Frame frame = IdentityFrame();
                frame.softShadows = soft;
                frame.softShadowSamples = samples;
                const Image img = Render(renderer, scene, frame);
                const float open = img.At(41, 32, 0);
                int partial = 0;
                for (int x = 22; x < 42; ++x) {
                    const float v = img.At(x, 32, 0) / open;
                    partial += v > 0.1f && v < 0.9f;
                }
                scene.RemoveLight(light);
                return partial;
            };
            const int hard = penumbra(0.02f, true), soft = penumbra(0.15f, true);
            Expect(soft >= hard + 3, "area light shadows have a penumbra (PCSS)");
            Expect(penumbra(0.15f, false) <= hard + 1, "Frame::softShadows off: hard edges for area lights");
            Expect(penumbra(0.15f, true, 4) >= hard + 3, "Frame::softShadowSamples 4: still a penumbra");

            // Sample dimensions: one PCSS tap per pixel, turned per sample,
            // averaged by the film. The blue noise turned by its own Halton
            // value converges on the 512-sample film at least as well as
            // the per-frame shifted gradient noise, and to the same image.
            vkr::Light *light = AddSphereLight(scene, -0.6f, 0, 0, 0.15f, 0.16f / (0.15f * 0.15f));
            light->castShadows = true;
            scene.Commit();
            auto film = [&](uint32_t samples, bool dims) {
                vkr::Renderer r(device);
                vkr::Renderer::Frame frame = IdentityFrame();
                frame.postProcess = true;
                frame.post.autoExposure = false;
                frame.post.bloomIntensity = 0.0f;
                frame.post.tonemapper = vkr::PostProcess::Settings::Tonemapper::None;
                frame.softShadowSamples = 1;
                frame.shadowJitter = false;  // PCSS's taps, not the light's shape
                frame.filmSamples = samples;
                frame.sampleDimensions = dims;
                Image img;
                do {
                    img = Render(r, scene, frame);
                } while (!r.IsConverged());
                return img;
            };
            auto error = [](const Image &a, const Image &b) {
                double sum = 0;
                for (int y = 16; y < 48; ++y) {
                    for (int x = 12; x < 44; ++x) {
                        sum += std::pow(a.At(x, y, 0) - b.At(x, y, 0), 2.0f);
                    }
                }
                return std::sqrt(sum / (32 * 32));
            };
            const Image reference = film(512, true);
            const double on = error(film(16, true), reference), off = error(film(16, false), reference);
            std::printf("sample dimensions: 16-sample film error %.5f (gradient noise %.5f), vs %.5f\n", on, off,
                        error(film(512, false), reference));
            Expect(on <= off && error(film(1, true), film(1, false)) > 0.0, "sample dimensions: blue noise per effect");
            Expect(error(film(512, false), reference) < 0.5 * off, "sample dimensions: the same converged image");
            scene.RemoveLight(light);
        }

        {
            // Translucency: a blue quad of opacity 0.5 over the left half of
            // a red one blends half and half there, and leaves the depth
            // (prepass) to the red one.
            vkr::Scene scene(&device);
            AddLight(scene, 0, 0, -1);
            auto red = std::make_shared<vkr::Material>(), blue = std::make_shared<vkr::Material>();
            red->baseColor[0] = 1;
            red->baseColor[1] = red->baseColor[2] = 0;
            red->roughness = 1;
            blue->baseColor[2] = 1;
            blue->baseColor[0] = blue->baseColor[1] = 0;
            blue->roughness = 1;
            blue->opacity = 0.5f;
            AddQuad(scene, -1, -1, 1, 1, 0.5f)->material = red;
            AddQuad(scene, -1, -1, 0, 1, 0)->material = blue;
            scene.Commit();
            const Image img = Render(renderer, scene);
            Expect(std::abs(img.At(16, 32, 0) - 0.5f) < 0.06f && std::abs(img.At(16, 32, 2) - 0.5f) < 0.06f,
                   "translucent quad blends half and half over the opaque one");
            Expect(img.At(48, 32, 0) > 0.9f && img.At(48, 32, 2) < 0.05f, "beside it, the opaque quad alone");
            Expect(img.depth[32 * 64 + 16] == img.depth[32 * 64 + 48], "translucency writes no depth");
            vkr::Renderer::Frame frame = IdentityFrame();
            frame.translucency = false;
            const Image opaque = Render(renderer, scene, frame);
            Expect(opaque.At(16, 32, 0) > 0.9f && opaque.At(16, 32, 2) < 0.05f,
                   "Frame::translucency off: only the opaque quad");
        }

        {
            // Motion vectors: a quad moved by 0.25 (NDC x) since the last
            // frame was 0.125 uv to the left; once it stops, it has none.
            vkr::Scene scene(&device);
            AddLight(scene, 0, 0, -1);
            vkr::Mesh *quad = AddQuad(scene, -0.5f, -0.5f, 0.5f, 0.5f, 0);
            scene.Commit();
            Render(renderer, scene);
            quad->prevTransforms = quad->transforms;
            quad->transforms[12] = 0.25f;  // row-vector order: translation is the last row
            quad->transformsDirty = true;
            scene.Commit();
            const Image moved = Render(renderer, scene);
            Expect(std::abs(moved.Vel(40, 32, 0) + 0.125f) < 0.002f && std::abs(moved.Vel(40, 32, 1)) < 0.002f,
                   "motion vector of a moved quad");
            Expect(moved.Vel(4, 32, 0) == 0.0f, "no motion vector where nothing was drawn");
            vkr::Renderer::Frame still = IdentityFrame();
            still.objectMotion = false;
            Expect(Render(renderer, scene, still).Vel(40, 32, 0) == 0.0f, "Frame::objectMotion off: no motion vectors");
            scene.Commit();
            Expect(std::abs(Render(renderer, scene).Vel(40, 32, 0)) < 0.002f, "a quad that stopped has no motion");
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
            vkr::Renderer::Frame noShadows = IdentityFrame();
            noShadows.shadows = false;
            Expect(Render(renderer, scene, noShadows).At(3, 32, 0) > 0.9f * lit, "Frame::shadows off: no sun shadow");
            vkr::Renderer::Frame coarse = IdentityFrame();
            coarse.shadowResolution = 512;
            coarse.shadowCascades = 1;
            const Image one = Render(renderer, scene, coarse);
            Expect(one.At(3, 32, 0) < 0.1f * lit && one.At(51, 32, 0) > 0.9f * lit,
                   "one 512 cascade: still shadowed, still lit beside");
        }

        {
            // Dome sun extraction: the shadow scene lit by a dome whose sun
            // (2x2 texels of radiance 500 towards (1, 0, -1)) the rest
            // (0.02) can't outshine. Off: the sun is image-based light, no
            // shadow. On: a distant light with shadows, as bright.
            vkr::Scene scene(&device);
            auto tex = std::make_shared<vkr::Texture>();
            tex->width = 128;
            tex->height = 64;
            tex->format = vkr::Texture::Format::RGBA32F;
            std::vector<float> texels(128 * 64 * 4, 0.02f);
            for (uint32_t y : {31u, 32u}) {
                for (uint32_t x : {15u, 16u}) {
                    std::fill_n(&texels[(y * 128 + x) * 4], 3, 500.0f);
                }
            }
            tex->pixels.resize(texels.size() * sizeof(float));
            std::memcpy(tex->pixels.data(), texels.data(), tex->pixels.size());
            scene.AddDome()->texture = tex;
            AddQuad(scene, -1, -1, 1, 1, 0.5f)->color[0] = 0.8f;
            AddQuad(scene, -0.25f, -0.25f, 0.25f, 0.25f, -0.5f);
            AddQuad(scene, 0.95f, 0.95f, 0.99f, 0.99f, 0.9f);
            scene.Commit();
            vkr::Renderer::Frame frame = IdentityFrame();
            frame.showEnvironment = false;
            const Image off = Render(renderer, scene, frame);
            Expect(off.At(3, 32, 0) > 0.8f * off.At(51, 32, 0), "dome sun off: the sun lights the shadow too");
            Expect(scene.SetDomeSun(true, 10.0f), "dome sun: setting changed");
            scene.Commit();
            const Image on = Render(renderer, scene, frame);
            const float shadowed = on.At(3, 32, 0), lit = on.At(51, 32, 0);
            Expect(shadowed < 0.2f * lit, "dome sun: extracted, it casts a shadow");
            Expect(std::abs(lit - off.At(51, 32, 0)) < 0.25f * lit, "dome sun: as bright as the dome's");
            Expect(!scene.SetDomeSun(true, 10.0f), "dome sun: unchanged");
            Expect(vkr::Renderer::Frame().domeSun, "dome sun: on by default");
            scene.SetDomeSun(false, 10.0f);
            scene.Commit();
            Expect(Render(renderer, scene, frame).At(51, 32, 0) == off.At(51, 32, 0), "dome sun off again: as before");
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
            // Film accumulation: an emitter over x < 32.3 pixels, exposed
            // linear over black. The converged film is the edge through
            // the Gaussian pixel filter (sigma 0.284 * 1.5 pixels):
            // Phi((32.3 - centre) / 0.426) per pixel; radius 0 is a box,
            // 0.3 at pixel 32. Any change starts it over: at once in final
            // quality, after kFilmStillFrames of TAA otherwise.
            vkr::Scene scene(&device);
            AddEmitter(scene, -1, -1, 32.3f / 32.0f - 1.0f, 1, 0, 1.0f);
            scene.Commit();
            vkr::Renderer::Frame frame = IdentityFrame();
            frame.postProcess = true;
            frame.post.autoExposure = false;
            frame.post.bloomIntensity = 0.0f;
            frame.post.tonemapper = vkr::PostProcess::Settings::Tonemapper::None;
            std::fill(frame.clearColor, frame.clearColor + 4, 0.0f);
            frame.filmSamples = 32;
            auto converge = [&](vkr::Renderer &renderer, int *frames) {
                Image img;
                for (*frames = 0; *frames < 100;) {
                    img = Render(renderer, scene, frame);
                    ++*frames;
                    if (renderer.IsConverged()) {
                        break;
                    }
                }
                return img;
            };
            auto phi = [](float x) { return 0.5f * std::erfc(-x / std::sqrt(2.0f)); };
            vkr::Renderer renderer(device);
            int frames = 0;
            const Image gauss = converge(renderer, &frames);
            Expect(frames == 32 && renderer.GetStats().taaSamples == 32, "the film converges after filmSamples");
            bool filtered = true;
            for (int x = 30; x <= 34; ++x) {
                filtered = filtered && std::abs(gauss.At(x, 32, 1) - phi((32.3f - (x + 0.5f)) / 0.426f)) < 0.02f;
            }
            Expect(filtered, "the film reconstructs the edge with its Gaussian");
            frame.sceneVersion = 1;  // the app changed something
            Render(renderer, scene, frame);
            Expect(!renderer.IsConverged() && renderer.GetStats().taaTarget == frame.taaSamples,
                   "a change hands the image back to TAA");
            converge(renderer, &frames);
            Expect(frames == 32 + int(vkr::Renderer::kFilmStillFrames) - 1, "the film takes over after still frames");
            frame.finalQuality = true;
            frame.sceneVersion = 2;
            Render(renderer, scene, frame);
            Expect(renderer.GetStats().taaSamples == 1, "final quality starts the film over at once");
            frame.finalQuality = false;
            frame.filmFilterRadius = 0.0f;
            vkr::Renderer box(device);
            Expect(std::abs(converge(box, &frames).At(32, 32, 1) - 0.3f) < 0.02f, "radius 0: a box filter");
            frame.filmAccumulation = false;
            vkr::Renderer taa(device);
            converge(taa, &frames);
            Expect(frames == int(frame.taaSamples), "film off: TAA converges after taaSamples");

            // Interactive sample loop: while the camera moves the sample
            // dimensions loop over their first interactiveSamples; still,
            // the film's samples run on.
            frame.filmAccumulation = true;
            frame.filmFilterRadius = 1.5f;
            frame.interactiveSamples = 4;
            vkr::Renderer moving(device);
            uint32_t seen = 0, most = 0;
            for (int i = 0; i < 12; ++i) {
                frame.viewProj[12] = (i % 2 ? 1.0f : -1.0f) * 0.01f;
                frame.invViewProj[12] = -frame.viewProj[12];
                Render(moving, scene, frame);
                seen |= 1u << moving.GetStats().sample;
                most = std::max(most, moving.GetStats().sample);
            }
            frame.viewProj[12] = frame.invViewProj[12] = 0.0f;
            for (int i = 0; i < 12; ++i) {
                Render(moving, scene, frame);
            }
            Expect(seen == 0x1Eu && most == 4 && moving.GetStats().sample > 4,
                   "moving: the sample dimensions loop over interactiveSamples; still: the film's run on");

            // Adaptive sampling: away from the edge the pixels have no
            // noise and stop at the minimum; the edge's keep sampling to
            // filmSamples. A threshold above their noise stops them all.
            frame.interactiveSamples = 8;
            frame.adaptiveThreshold = 0.002f;
            frame.adaptiveMinSamples = 8;
            vkr::Renderer adaptive(device);
            const Image early = converge(adaptive, &frames);
            const uint32_t active = adaptive.GetStats().filmActivePixels;
            Expect(frames == 32 && active > 0 && active <= 4 * 64 && MaxDifference(early, gauss) < 0.02f,
                   "adaptive sampling: only the noisy pixels take more samples");
            frame.adaptiveThreshold = 0.5f;
            vkr::Renderer coarse(device);
            converge(coarse, &frames);
            Expect(frames == 9 && coarse.GetStats().filmActivePixels == 0,
                   "adaptive sampling: done once every pixel's noise is under the threshold");

            // Final quality samples adaptively by default: with only
            // adaptiveFinalThreshold set, a fresh film (final) stops at the
            // minimum; the film that took over from TAA after a change
            // (interactive) still runs to filmSamples. The converged pixel
            // fraction ends the noisy-edge film early (6% of the pixels
            // are its edge).
            frame.adaptiveThreshold = 0.0f;
            frame.adaptiveFinalThreshold = 0.5f;
            vkr::Renderer finalRun(device);
            converge(finalRun, &frames);
            Expect(frames == 9 && finalRun.GetStats().filmActivePixels == 0,
                   "adaptive sampling: on in a final-quality film by eldr:adaptiveFinalThreshold");
            frame.sceneVersion = 100;
            Render(finalRun, scene, frame);
            converge(finalRun, &frames);
            Expect(frames == 32 + int(vkr::Renderer::kFilmStillFrames) - 1,
                   "adaptive sampling: not in a film that took over from TAA");
            frame.adaptiveFinalThreshold = 0.002f;
            frame.adaptivePixelFraction = 0.9f;
            vkr::Renderer fraction(device);
            converge(fraction, &frames);
            Expect(frames >= 8 && frames < 32, "adaptive sampling: converged once 90% of the pixels stopped");
            frame.adaptivePixelFraction = 0.99f;
            vkr::Renderer edge(device);
            converge(edge, &frames);
            Expect(frames == 32, "adaptive sampling: 6% noisy pixels keep the film running at 99%");
            frame.adaptiveFinalThreshold = 0.0f;
            frame.adaptiveThreshold = 0.002f;
        }

        {
            // Material programs (MaterialX graphs): emission-only quads and no
            // lights or dome, so each pixel is exactly what its program outputs.
            vkr::Scene scene(&device);
            using P = vkr::MaterialProgram;
            auto programmed = [&](float x0, float y0, float x1, float y1) {
                vkr::Mesh *q = AddQuad(scene, x0, y0, x1, y1, 0);
                auto m = std::make_shared<vkr::Material>();
                std::fill(m->baseColor, m->baseColor + 3, 0.0f);
                m->roughness = 1;
                q->material = m;
                return std::make_pair(q, m);
            };
            // Per instance: the primvar "tint", two instances 0.4 apart.
            auto [inst, instM] = programmed(-0.9f, 0.5f, -0.6f, 0.9f);
            instM->program.primvars = {"tint"};
            instM->program.Emit(P::kPrimvar, 0, {instM->program.Constant(1, 1, 1, 1)}, 0);
            instM->program.Emit(P::kOutput, 0, {0}, P::kEmissive);
            const std::vector<float> shifted = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0.4f, 0, 0, 1};
            inst->transforms.insert(inst->transforms.end(), shifted.begin(), shifted.end());
            inst->primvars = {{"tint", vkr::Primvar::Rate::Instance, 3, {1, 0, 0, 0, 1, 0}}};
            // Per triangle: AddQuad's first triangle is the lower right one.
            auto [uni, uniM] = programmed(0.1f, 0.5f, 0.9f, 0.9f);
            uniM->program.primvars = {"face"};
            uniM->program.Emit(P::kPrimvar, 0, {uniM->program.Constant(0, 0, 0, 0)}, 0);
            uniM->program.Emit(P::kOutput, 0, {0}, P::kEmissive);
            uni->primvars = {{"face", vkr::Primvar::Rate::Uniform, 3, {1, 0, 0, 0, 0, 1}}};
            // Per vertex: 0 on the left corners, 1 on the right, interpolated.
            auto [vtx, vtxM] = programmed(-0.9f, -0.3f, -0.1f, 0.3f);
            vtxM->program.primvars = {"w"};
            vtxM->program.Emit(P::kPrimvar, 0, {vtxM->program.Constant(0, 0, 0, 0)}, 0);
            vtxM->program.Emit(P::kOutput, 0, {0}, P::kEmissive);
            std::vector<float> w;
            for (size_t c = 0; c < vtx->points.size() / 3; ++c) {
                w.push_back(vtx->points[c * 3] > -0.5f ? 1.0f : 0.0f);
            }
            vtx->primvars = {{"w", vkr::Primvar::Rate::Vertex, 1, w}};
            // Houdini ramps (hinvlinear + huniformramp, ten keys and values).
            // Three keys 0, 0.25, 1 (the rest 1): 0.5 is 1/3 through the
            // second segment, position 2/3 between red, green and blue.
            auto houdiniRamp = [&](vkr::Mesh *mesh, float at, float num, std::vector<float> keys,
                                   std::vector<std::array<float, 3>> values) {
                P &p = mesh->material->program;
                const uint8_t position = p.Constant(at, at, at, at), count = p.Constant(num, num, num, num);
                const uint32_t keyBase = uint32_t(p.constants.size() / 4);
                keys.resize(12, 0.0f);
                for (size_t i = 0; i < 12; i += 4) {
                    p.Constant(keys[i], keys[i + 1], keys[i + 2], keys[i + 3]);
                }
                const uint32_t valueBase = uint32_t(p.constants.size() / 4);
                values.resize(10, {0, 0, 0});
                for (const auto &v : values) {
                    p.Constant(v[0], v[1], v[2], 1);
                }
                p.Emit(P::kInvLinear, 0, {position, count}, keyBase);
                p.Emit(P::kUniformRamp, 1, {0, count}, valueBase);
                p.Emit(P::kOutput, 0, {1}, P::kEmissive);
            };
            auto [ramp, rampM] = programmed(0.1f, -0.3f, 0.4f, 0.3f);
            houdiniRamp(ramp, 0.5f, 3, {0, 0.25f, 1, 1, 1, 1, 1, 1, 1, 1}, {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}});
            // num past ten (a 17-point Houdini ramp): hinvlinear spreads the
            // keys over i / 16, huniformramp reads ten values over i / 9. At
            // 0.45, halfway from key4 to key5: (4.5 / 16) of the way along.
            auto [ramp17, ramp17M] = programmed(0.5f, -0.9f, 0.9f, -0.5f);
            std::vector<std::array<float, 3>> linear;
            for (int i = 0; i < 10; ++i) {
                linear.push_back({float(i) / 9.0f, 0, 0});
            }
            houdiniRamp(ramp17, 0.45f, 17, {0, 0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f, 1}, linear);
            // Overlay of 0.25 over 0.75: 1 - 2 (1 - 0.25)(1 - 0.75).
            auto [over, overM] = programmed(0.5f, -0.3f, 0.9f, 0.3f);
            {
                P &p = overM->program;
                p.Emit(P::kOverlay, 0,
                       {p.Constant(0.25f, 0.25f, 0.25f, 1), p.Constant(0.75f, 0.75f, 0.75f, 1), p.Constant(1, 1, 1, 1)});
                p.Emit(P::kOutput, 0, {0}, P::kEmissive);
            }
            // Hue of red turned a third: green.
            auto [hsv, hsvM] = programmed(-0.9f, -0.9f, -0.5f, -0.5f);
            {
                P &p = hsvM->program;
                p.Emit(P::kHsvAdjust, 0, {p.Constant(1, 0, 0, 1), p.Constant(1.0f / 3.0f, 1, 1, 0)});
                p.Emit(P::kOutput, 0, {0}, P::kEmissive);
            }
            // A texture (left texel red, right blue) times 2.
            auto [image, imageM] = programmed(-0.4f, -0.9f, 0.0f, -0.5f);
            {
                auto tex = std::make_shared<vkr::Texture>();
                tex->width = 2;
                tex->height = 1;
                tex->pixels = {255, 0, 0, 255, 0, 0, 255, 255};
                P &p = imageM->program;
                p.images.emplace_back();
                p.images.back().texture = tex;
                p.Emit(P::kTexcoord, 0);
                p.Emit(P::kImage, 1, {0, p.Constant(0, 0, 0, 0)}, 0);
                p.Emit(P::kMultiply, 2, {1, p.Constant(2, 2, 2, 1)});
                p.Emit(P::kOutput, 0, {2}, P::kEmissive);
            }
            scene.Commit();
            const Image img = Render(renderer, scene);
            auto near = [&](int x, int y, float r, float g, float b) {
                return std::abs(img.At(x, y, 0) - r) < 0.03f && std::abs(img.At(x, y, 1) - g) < 0.03f &&
                       std::abs(img.At(x, y, 2) - b) < 0.03f;
            };
            Expect(near(8, 54, 1, 0, 0) && near(20, 54, 0, 1, 0), "program reads instance primvars");
            Expect(near(57, 49, 1, 0, 0) && near(38, 59, 0, 0, 1), "program reads uniform primvars");
            const float wl = (PixelNdc(6) + 0.9f) / 0.8f, wr = (PixelNdc(25) + 0.9f) / 0.8f;
            Expect(near(6, 32, wl, wl, wl) && near(25, 32, wr, wr, wr), "program reads vertex primvars");
            Expect(near(40, 32, 0, 2.0f / 3.0f, 1.0f / 3.0f), "Houdini ramp (invlinear, uniformramp)");
            Expect(near(54, 9, 4.5f / 16.0f, 0, 0), "Houdini ramp with num past ten");
            Expect(near(54, 32, 0.625f, 0.625f, 0.625f), "MaterialX overlay");
            Expect(near(9, 9, 0, 1, 0), "MaterialX hsvadjust");
            Expect(img.At(22, 9, 0) > 1.8f && img.At(22, 9, 2) < 0.2f, "program samples a texture");
            vkr::Renderer::Frame frame = IdentityFrame();
            frame.anisotropy = 1;
            Expect(MaxDifference(Render(renderer, scene, frame), img) < 1e-3f,
                   "Frame::anisotropy 1: the same head-on texture");
            frame.materialGraphs = false;
            Expect(Render(renderer, scene, frame).At(8, 54, 0) == 0.0f, "Frame::materialGraphs off: plain (black) inputs");
        }

        {
            // Depth of field: an emissive quad over the left half, 4 units
            // away, through PerspectiveFrame's 90 degree lens (f 0.5 on a
            // 1-unit sensor). At f/1 focused at 1 its edge spreads right (a
            // circle of 2.6 pixels, the cap at 64 wide); focused on it, or
            // with fStop 0, the edge stays hard.
            vkr::Scene scene(&device);
            auto white = std::make_shared<vkr::Material>();
            std::fill(white->baseColor, white->baseColor + 3, 0.0f);
            std::fill(white->emissive, white->emissive + 3, 1.0f);
            AddQuad(scene, -10, -10, 0, 10, -4)->material = white;
            scene.Commit();
            auto beside = [&](float fStop, float focus) {
                vkr::Renderer renderer(device);
                vkr::Renderer::Frame frame = PerspectiveFrame();
                frame.postProcess = true;
                frame.post.autoExposure = false;
                frame.post.bloomIntensity = 0.0f;
                frame.focalLength = 0.5f;
                frame.sensorWidth = 1.0f;
                frame.fStop = fStop;
                frame.focusDistance = focus;
                Image img;
                for (int i = 0; i < 64 && (i == 0 || !renderer.IsConverged()); ++i) {
                    img = Render(renderer, scene, frame);
                }
                return img.At(33, 32, 0);
            };
            const float sharp = beside(0.0f, 1.0f), blurred = beside(1.0f, 1.0f), focused = beside(1.0f, 4.0f);
            // Focused, the post blur's gather still leaks ~2% of the edge
            // into the out-of-focus background beside it (TAA's clipping hid
            // that; the film's plain average keeps it).
            Expect(sharp < 0.01f && focused < 0.03f && blurred > 0.05f,
                   "depth of field spreads an out-of-focus edge, not a focused one");
        }

        {
            // Motion blur: an emissive quad over the left half moves right by
            // 0.25 (8 pixels; the blur is capped at 3.2 at 64 wide). With the
            // shutter open a whole frame it smears over the background beyond
            // its new edge, and that frame is final at once; shutter closed,
            // the edge is hard.
            vkr::Scene scene(&device);
            auto white = std::make_shared<vkr::Material>();
            std::fill(white->baseColor, white->baseColor + 3, 0.0f);
            std::fill(white->emissive, white->emissive + 3, 1.0f);
            vkr::Mesh *quad = AddQuad(scene, -1, -1, 0, 1, 0);
            quad->material = white;
            auto edge = [&](float shutter, float *inside, bool *converged) {
                vkr::Renderer renderer(device);
                vkr::Renderer::Frame frame = IdentityFrame();
                frame.postProcess = true;
                frame.post.autoExposure = false;
                frame.post.bloomIntensity = 0.0f;
                frame.shutter = shutter;
                quad->transforms[12] = 0.0f;
                quad->transformsDirty = true;
                scene.Commit();
                Render(renderer, scene, frame);
                quad->prevTransforms = quad->transforms;
                quad->transforms[12] = 0.25f;
                quad->transformsDirty = true;
                scene.Commit();
                const Image img = Render(renderer, scene, frame);
                *inside = img.At(16, 32, 0);
                *converged = renderer.IsConverged();
                return img.At(40, 32, 0);  // the first pixel past the edge
            };
            float inside = 0.0f, sharpInside = 0.0f;
            bool final = false, sharpFinal = true;
            const float blurred = edge(1.0f, &inside, &final), sharp = edge(0.0f, &sharpInside, &sharpFinal);
            Expect(blurred > 0.05f * inside && sharp < 0.01f * sharpInside && inside > 0.1f,
                   "motion blur smears a moving edge over what it passes");
            Expect(final && !sharpFinal, "a motion-blurred frame is final, a sharp one keeps accumulating");
        }

        {
            // Motion blur time samples: the same quad's edge moves from x = 0
            // to 0.25 (pixel 32 to 40) over a shutter of [-1, 0]; in final
            // quality the film renders each sample with the quad at a time in
            // it, so the edge's pixels are covered for the part of the shutter
            // the edge is past them: a linear ramp (in linear post, no
            // tonemapper). The camera moving instead (by -0.25) gives the
            // same ramp. Time samples off: the film stays off for the motion
            // and the post blur's frame is final at once, as before.
            vkr::Scene scene(&device);
            auto white = std::make_shared<vkr::Material>();
            std::fill(white->baseColor, white->baseColor + 3, 0.0f);
            std::fill(white->emissive, white->emissive + 3, 1.0f);
            vkr::Mesh *quad = AddQuad(scene, -1, -1, 0, 1, 0);
            quad->material = white;
            // sampled: the motion comes with the first frame (Frame::sampledMotion,
            // Mesh::shutterMotion, Frame::prevViewProj), no frame before it.
            auto ramp = [&](bool timeSamples, bool moveCamera, int *frames, bool sampled = false) {
                vkr::Renderer renderer(device);
                vkr::Renderer::Frame frame = IdentityFrame();
                frame.postProcess = true;
                frame.post.autoExposure = false;
                frame.post.bloomIntensity = 0.0f;
                frame.post.tonemapper = vkr::PostProcess::Settings::Tonemapper::None;
                frame.shutterOpen = -1.0f;
                frame.shutter = 1.0f;
                frame.finalQuality = true;
                frame.filmSamples = 64;
                frame.filmFilterRadius = 0.0f;
                frame.motionBlurTimeSamples = timeSamples;
                quad->transforms[12] = 0.0f;
                quad->transformsDirty = true;
                scene.Commit();
                if (sampled) {
                    frame.sampledMotion = true;
                    std::copy(frame.viewProj, frame.viewProj + 16, frame.prevViewProj);
                    std::copy(frame.cameraPos, frame.cameraPos + 3, frame.prevCameraPos);
                    quad->shutterMotion = !moveCamera;
                    quad->geometryDirty = true;  // as new geometry
                } else {
                    Render(renderer, scene, frame);
                }
                if (moveCamera) {
                    frame.viewProj[12] = -0.25f;
                    frame.invViewProj[12] = 0.25f;
                    frame.cameraPos[0] = 0.25f;
                } else {
                    quad->prevTransforms = quad->transforms;
                    quad->transforms[12] = 0.25f;
                    quad->transformsDirty = true;
                    scene.Commit();
                }
                Image img;
                for (*frames = 0; *frames < 100 && (*frames == 0 || !renderer.IsConverged()); ++*frames) {
                    img = Render(renderer, scene, frame);
                    scene.Commit();  // still: no motion rows any more
                }
                std::vector<float> row;
                for (int x = 0; x < 64; ++x) {
                    row.push_back(img.At(x, 32, 0) / std::max(img.At(16, 32, 0), 1e-6f));
                }
                return row;
            };
            int frames = 0;
            bool linear = true, mirrored = true;
            const std::vector<float> rigid = ramp(true, false, &frames);
            const int rigidFrames = frames;
            const std::vector<float> camera = ramp(true, true, &frames);
            for (int x = 33; x < 40; ++x) {  // pixel x is past the edge for t > (x + 0.5 - 32) / 8
                linear = linear && std::abs(rigid[x] - (1.0f - (x + 0.5f - 32.0f) / 8.0f)) < 0.06f;
            }
            for (int x = 25; x < 32; ++x) {  // the edge moves left: covered for t < (31.5 - x) / 8
                mirrored = mirrored && std::abs(camera[x] - (31.5f - x) / 8.0f) < 0.06f;
            }
            Expect(rigidFrames == 64 && linear && rigid[30] > 0.95f && rigid[41] < 0.05f,
                   "time samples: a moving edge blurs into a linear ramp over the film's samples");
            Expect(mirrored, "time samples: the camera's motion blurs the same");
            const std::vector<float> off = ramp(false, false, &frames);
            Expect(frames == 1 && off[34] > 0.9f, "time samples off: the post blur's frame is final at once");
            bool sampledLinear = true, sampledMirrored = true;
            const std::vector<float> first = ramp(true, false, &frames, true);
            const std::vector<float> firstCamera = ramp(true, true, &frames, true);
            for (int x = 33; x < 40; ++x) {
                sampledLinear = sampledLinear && std::abs(first[x] - rigid[x]) < 0.02f;
            }
            for (int x = 25; x < 32; ++x) {
                sampledMirrored = sampledMirrored && std::abs(firstCamera[x] - camera[x]) < 0.02f;
            }
            Expect(sampledLinear && sampledMirrored,
                   "sampled motion: a first frame blurs as one after a rendered frame (rigid and camera)");
        }

        {
            // A long blur in final quality (defaults: adaptive sampling,
            // Gaussian film filter): the edge moves 48 pixels (8 to 56) over
            // the shutter, so pixel x is covered for 1 - (x + 0.5 - 8) / 48
            // of it. 16 samples are 3 pixels apart: without the residual
            // blur between them (Frame::motionBlurResidual) the ramp is a
            // staircase of 1/16 steps (up to 0.06 off). At 64 the tail's
            // pixels (covered 1-12% of the shutter) must not stop dark after
            // the first samples missed them (adaptive sampling). The same
            // edge moved by a joint (skinning) and by a blend shape must
            // blur as smoothly: their motion in a time sample starts from
            // the shutter's captured joints and weights too.
            auto white = std::make_shared<vkr::Material>();
            std::fill(white->baseColor, white->baseColor + 3, 0.0f);
            std::fill(white->emissive, white->emissive + 3, 1.0f);
            enum Mover { kRigid, kJoint, kBlendShape };
            auto longRamp = [&](uint32_t samples, int *frames, Mover mover = kRigid) {
                vkr::Scene scene(&device);
                // Its left edge stays off the image; a blend shape moves the right one.
                vkr::Mesh *quad = AddQuad(scene, -4, -1, mover == kBlendShape ? -0.75f : 0.0f, 1, 0);
                quad->material = white;
                if (mover != kRigid) {
                    quad->jointCount = 1;
                    quad->skinningTransforms = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
                    for (size_t c = 0; c < quad->points.size() / 3; ++c) {
                        quad->jointIndices.insert(quad->jointIndices.end(), {0, 0, 0, 0});
                        quad->jointWeights.insert(quad->jointWeights.end(), {1, 0, 0, 0});
                    }
                }
                if (mover == kBlendShape) {
                    quad->blendOffsets = {1.5f, 0, 0, 0};
                    for (size_t c = 0; c < quad->points.size() / 3; ++c) {
                        quad->blendRanges.insert(quad->blendRanges.end(), {0u, quad->points[c * 3] > -1 ? 1u : 0u});
                    }
                    quad->blendShapeCount = 1;
                    quad->blendWeights = {0.0f};
                }
                auto place = [&](float x) {
                    if (mover == kRigid) {
                        quad->prevTransforms = quad->transforms;
                        quad->transforms[12] = x;
                        quad->transformsDirty = true;
                    } else if (mover == kJoint) {
                        quad->skinningTransforms[12] = x;
                        quad->jointsDirty = true;
                    } else {
                        quad->blendWeights = {(x + 0.75f) / 1.5f};
                        quad->jointsDirty = true;
                    }
                    scene.Commit();
                };
                vkr::Renderer renderer(device);
                vkr::Renderer::Frame frame = IdentityFrame();
                frame.postProcess = true;
                frame.post.autoExposure = false;
                frame.post.bloomIntensity = 0.0f;
                frame.post.tonemapper = vkr::PostProcess::Settings::Tonemapper::None;
                frame.shutterOpen = -1.0f;
                frame.shutter = 1.0f;
                frame.finalQuality = true;
                frame.filmSamples = samples;
                frame.adaptiveFinalThreshold = vkr::Renderer::Frame().adaptiveFinalThreshold;
                place(-0.75f);
                Render(renderer, scene, frame);
                place(0.75f);
                Image img;
                for (*frames = 0; *frames < 300 && (*frames == 0 || !renderer.IsConverged()); ++*frames) {
                    img = Render(renderer, scene, frame);
                    scene.Commit();
                }
                std::vector<float> row;
                for (int x = 0; x < 64; ++x) {
                    row.push_back(img.At(x, 32, 0) / std::max(img.At(2, 32, 0), 1e-6f));
                }
                return row;
            };
            int frames = 0;
            const std::vector<float> coarse = longRamp(16, &frames);
            float worst = 0.0f, rise = 0.0f;
            for (int x = 12; x < 52; ++x) {
                worst = std::max(worst, std::abs(coarse[x] - (1.0f - (x + 0.5f - 8.0f) / 48.0f)));
                rise = std::max(rise, coarse[x + 1] - coarse[x]);
            }
            const std::string smooth = "long blur, 16 time samples: a smooth, monotonic ramp, not steps (worst " +
                                       std::to_string(worst) + ", rise " + std::to_string(rise) + ")";
            Expect(worst < 0.025f && rise < 0.01f, smooth.c_str());
            const std::vector<float> fine = longRamp(64, &frames);
            bool tail = true;
            for (int x = 50; x < 56; ++x) {  // covered 1.0-11.5% of the shutter
                tail = tail && fine[x] > 0.5f * (1.0f - (x + 0.5f - 8.0f) / 48.0f);
            }
            const std::string kept = "long blur: adaptive sampling keeps the faint tail's samples (" +
                                     std::to_string(frames) + " frames, x 55: " + std::to_string(fine[55]) + ")";
            Expect(frames == 64 && tail, kept.c_str());
            for (const Mover mover : {kJoint, kBlendShape}) {
                const std::vector<float> skinned = longRamp(16, &frames, mover);
                float off = 0.0f, up = 0.0f;
                for (int x = 12; x < 52; ++x) {
                    off = std::max(off, std::abs(skinned[x] - (1.0f - (x + 0.5f - 8.0f) / 48.0f)));
                    up = std::max(up, skinned[x + 1] - skinned[x]);
                }
                const std::string what = std::string("long blur moved by ") +
                                         (mover == kJoint ? "a joint" : "a blend shape") +
                                         ", 16 time samples: smooth and monotonic (worst " + std::to_string(off) +
                                         ", rise " + std::to_string(up) + ")";
                Expect(off < 0.025f && up < 0.01f, what.c_str());
            }
        }

        {
            // Upscaling (TAAU): an emitter whose edges fall between pixels
            // (x = 0.3: pixel 41.6; y = 0.45: row 46.4), rendered at half
            // size while TAA resolves (film off: it stays so), comes out at
            // the full size closer to the full-size TAA image than a half-size
            // render is (nearest upscaled); ids and depth come at full size.
            vkr::Scene scene(&device);
            auto white = std::make_shared<vkr::Material>();
            std::fill(white->baseColor, white->baseColor + 3, 0.0f);
            std::fill(white->emissive, white->emissive + 3, 1.0f);
            vkr::Mesh *quad = AddQuad(scene, -1, -1, 0.3f, 0.45f, 0);
            quad->material = white;
            quad->primId = 5;
            scene.Commit();
            auto resolve = [&](float scale, uint32_t size, uint32_t *renderWidth, bool final = false) {
                vkr::Renderer renderer(device);
                vkr::Renderer::Frame frame = IdentityFrame();
                frame.width = frame.height = size;
                frame.postProcess = true;
                frame.post.autoExposure = false;
                frame.post.bloomIntensity = 0.0f;
                frame.post.tonemapper = vkr::PostProcess::Settings::Tonemapper::None;
                frame.filmAccumulation = false;
                frame.renderScale = scale;
                frame.finalQuality = final;
                Image img;
                for (int i = 0; i < 48; ++i) {
                    img = Render(renderer, scene, frame);
                }
                *renderWidth = renderer.GetStats().renderWidth;
                return img;
            };
            uint32_t fullWidth = 0, halfWidth = 0, smallWidth = 0, finalWidth = 0;
            const Image full = resolve(1.0f, 64, &fullWidth), upscaled = resolve(0.5f, 64, &halfWidth),
                        small = resolve(1.0f, 32, &smallWidth);
            resolve(0.5f, 64, &finalWidth, true);
            double upscaledError = 0, smallError = 0;
            for (int y = 0; y < 64; ++y) {
                for (int x = 0; x < 64; ++x) {
                    const float smallValue = HalfToFloat(small.color[((y / 2) * 32 + x / 2) * 4]);
                    upscaledError += std::abs(upscaled.At(x, y, 0) - full.At(x, y, 0));
                    smallError += std::abs(smallValue - full.At(x, y, 0));
                }
            }
            std::printf("upscaling: mean error vs full size %.4f (half size, nearest: %.4f)\n",
                        upscaledError / 4096.0, smallError / 4096.0);
            Expect(fullWidth == 64 && halfWidth == 32 && smallWidth == 32 && finalWidth == 64,
                   "render scale 0.5: the scene renders at half size, final quality at full size");
            Expect(upscaledError < 0.5 * smallError && std::abs(upscaled.At(10, 10, 0) - 1.0f) < 0.02f &&
                       upscaled.At(60, 60, 0) < 0.02f,
                   "upscaling reconstructs the full size's edges better than a half-size image");
            Expect(upscaled.Prim(10, 10) == 5 && upscaled.Prim(60, 60) == -1 &&
                       std::abs(upscaled.depth[10 * 64 + 10] - full.depth[10 * 64 + 10]) < 1e-6f,
                   "upscaling: ids and depth at the frame's size");
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

        {
            // Emissive surfaces as lights: a small bright emitter (black
            // otherwise) at z = 0, x = -0.6, lights the white wall at z = 0.5
            // as a sphere light of its power, and a small occluder at
            // z = 0.25, x = -0.3 shadows it, as in the local light test.
            vkr::Scene scene(&device);
            auto white = std::make_shared<vkr::Material>();
            std::fill(white->baseColor, white->baseColor + 3, 1.0f);
            white->roughness = 1;
            AddQuad(scene, -1, -1, 1, 1, 0.5f)->material = white;
            AddQuad(scene, -0.35f, -0.05f, -0.25f, 0.05f, 0.25f)->material = white;
            AddEmitter(scene, -0.62f, -0.02f, -0.58f, 0.02f, 0, 2000.0f);
            scene.Commit();
            const Image lit = Render(renderer, scene);
            scene.SetEmissiveLights(false, 4.0f);
            scene.Commit();
            const Image off = Render(renderer, scene);
            scene.SetEmissiveLights(true, 5000.0f);
            scene.Commit();
            const Image dim = Render(renderer, scene);
            scene.SetEmissiveLights(true, 4.0f);
            scene.Commit();
            Expect(lit.At(41, 32, 0) > 0.05f && off.At(41, 32, 0) == 0.0f && dim.At(41, 32, 0) == 0.0f,
                   "a bright emitter lights the wall; off or under the threshold it doesn't");
            Expect(lit.At(32, 32, 0) < 0.2f * lit.At(41, 32, 0), "the emitter's light casts shadows");
            if (device.rayTracing) {
                vkr::Renderer::Frame frame = IdentityFrame();
                frame.rtShadows = true;
                const Image traced = Render(renderer, scene, frame);
                Expect(traced.At(32, 32, 0) < 0.2f * traced.At(41, 32, 0) &&
                           std::abs(traced.At(41, 32, 0) - lit.At(41, 32, 0)) < 0.05f * lit.At(41, 32, 0),
                       "the emitter's light has traced shadows, and doesn't shadow itself");
            }
        }

        if (device.rayTracing) {
            // Ray-traced sun shadow: the cascade test's scene with a hard sun
            // (angle 0), traced instead of the cascades.
            vkr::Scene scene(&device);
            vkr::Light *sun = AddLight(scene, 1, 0, -1);
            sun->castShadows = true;
            sun->angle = 0;
            AddQuad(scene, -1, -1, 1, 1, 0.5f)->color[0] = 0.8f;
            vkr::Mesh *occluder = AddQuad(scene, -0.25f, -0.25f, 0.25f, 0.25f, -0.5f);
            scene.Commit();
            vkr::Renderer::Frame frame = IdentityFrame();
            frame.rtShadows = true;
            const Image traced = Render(renderer, scene, frame);
            const vkr::Renderer::Stats stats = renderer.GetStats();
            const Image mapped = Render(renderer, scene);
            const float lit = mapped.At(51, 32, 0);
            int differ = 0;
            for (int x = 0; x < 64; ++x) {
                differ += std::abs(traced.At(x, 32, 0) - mapped.At(x, 32, 0)) > 0.1f * lit;
            }
            Expect(traced.At(3, 32, 0) < 0.01f * lit && std::abs(traced.At(51, 32, 0) - lit) < 0.01f * lit &&
                       differ <= 2,
                   "ray-traced sun shadow matches the cascades'");
            Expect(stats.accelerationStructureBytes > 0 &&
                       std::any_of(stats.passMs.begin(), stats.passMs.end(),
                                   [](const auto &p) { return std::strcmp(p.first, "rayTracing") == 0; }),
                   "ray tracing pass timed, its memory counted");

            // A cutout with nothing left casts no traced shadow (any-hit).
            auto cutout = std::make_shared<vkr::Material>();
            cutout->opacity = 0;
            cutout->opacityThreshold = 0.5f;
            occluder->material = cutout;
            scene.Commit();
            Expect(Render(renderer, scene, frame).At(3, 32, 0) > 0.9f * lit, "traced shadows skip cut-out surfaces");
            occluder->material = nullptr;
            scene.Commit();

            // The sun's angle softens the traced shadow: TAA-free frames of
            // one ray each differ, their average has a penumbra.
            sun->angle = 20;
            scene.Commit();
            frame.rtShadowSamples = 16;
            const Image soft = Render(renderer, scene, frame);
            int partial = 0;
            for (int x = 0; x < 32; ++x) {
                const float v = soft.At(x, 32, 0) / lit;
                partial += v > 0.05f && v < 0.95f;
            }
            Expect(partial >= 3, "the sun's angle gives traced shadows a penumbra");

            // Off again: no tracing, no acceleration structures.
            Render(renderer, scene);
            Expect(renderer.GetStats().accelerationStructureBytes == 0, "ray tracing off frees its structures");
        }

        if (device.rayTracing) {
            // Ray-traced local light shadow (the atlas test's scene).
            vkr::Scene scene(&device);
            auto white = std::make_shared<vkr::Material>();
            std::fill(white->baseColor, white->baseColor + 3, 1.0f);
            white->roughness = 1;
            AddQuad(scene, -1, -1, 1, 1, 0.5f)->material = white;
            AddQuad(scene, -0.35f, -0.05f, -0.25f, 0.05f, 0.25f)->material = white;
            AddSphereLight(scene, -0.6f, 0, 0, 0.02f, 400.0f)->castShadows = true;
            scene.Commit();
            vkr::Renderer::Frame frame = IdentityFrame();
            frame.rtShadows = true;
            const Image img = Render(renderer, scene, frame);
            Expect(img.At(32, 32, 0) < 0.2f * img.At(41, 32, 0), "ray-traced local light shadow");

            // Interactive frames trace three local lights, final quality up
            // to 15: with six the atlas is drawn, then not.
            for (int i = 0; i < 5; ++i) {
                AddSphereLight(scene, 0.6f, -0.6f + 0.3f * float(i), 0, 0.02f, 50.0f)->castShadows = true;
            }
            scene.Commit();
            auto atlasDrawn = [](const vkr::Renderer::Stats &stats) {
                return std::any_of(stats.passMs.begin(), stats.passMs.end(),
                                   [](const auto &p) { return std::strcmp(p.first, "localShadows") == 0; });
            };
            Render(renderer, scene, frame);
            Expect(atlasDrawn(renderer.GetStats()), "rtShadows: interactive frames trace three local lights");
            vkr::Renderer::Frame finalFrame = frame;
            finalFrame.rtShadows = false;
            finalFrame.finalShadows = vkr::Renderer::Frame::FinalShadows::Raytraced;
            finalFrame.finalQuality = true;
            finalFrame.postProcess = true;
            finalFrame.post.autoExposure = false;
            vkr::Renderer fresh(device);
            Render(fresh, scene, finalFrame);
            Expect(!atlasDrawn(fresh.GetStats()), "final shadows raytraced: six local lights all traced");
            finalFrame.finalShadows = vkr::Renderer::Frame::FinalShadows::Auto;
            Render(fresh, scene, finalFrame);
            Expect(atlasDrawn(fresh.GetStats()), "final shadows auto: maps with shadowed local lights");
        }

        if (device.rayTracing) {
            // Ray-traced reflections: the SSR test's mirror over an emitter,
            // without post (so no SSR): only traced rays see the emitter.
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
            vkr::Renderer::Frame frame = IdentityFrame();
            const float off = Render(renderer, scene, frame).At(32, 41, 1);
            frame.rtReflections = true;
            const float on = Render(renderer, scene, frame).At(32, 41, 1);
            Expect(off < 0.01f && on > 0.8f, "ray-traced reflections show the emitter in the mirror");
        }

        if (device.rayTracing) {
            // Global illumination: a white wall at z = 0.5 and, edge-on to the
            // camera in the x = 0 plane, a red panel whose -x side the sun
            // lights head-on; the sun only grazes the wall. Bounced light
            // tints the wall left of the panel red.
            vkr::Scene scene(&device);
            auto white = std::make_shared<vkr::Material>();
            std::fill(white->baseColor, white->baseColor + 3, 0.8f);
            white->roughness = 1;
            auto red = std::make_shared<vkr::Material>();
            red->baseColor[0] = 0.9f;
            red->baseColor[1] = red->baseColor[2] = 0.0f;
            red->roughness = 1;
            // Wound counter-clockwise towards the camera and the light: the
            // GI probes there must see front faces.
            vkr::Mesh *wall = AddQuad(scene, -1, -1, 1, 1, 0.5f);
            wall->points = {-1, -1, 0.5f, 1, 1, 0.5f, 1, -1, 0.5f, -1, -1, 0.5f, -1, 1, 0.5f, 1, 1, 0.5f};
            wall->material = white;
            vkr::Mesh *panel = scene.AddMesh();
            panel->points = {0, -0.6f, -0.5f, 0, 0.6f, 0.45f, 0, 0.6f, -0.5f,
                             0, -0.6f, -0.5f, 0, -0.6f, 0.45f, 0, 0.6f, 0.45f};
            panel->material = red;
            AddLight(scene, -1, 0, -0.2f)->castShadows = true;
            scene.Commit();
            vkr::Renderer::Frame frame = IdentityFrame();
            const Image direct = Render(renderer, scene, frame);
            frame.gi = true;
            frame.giProbes = 8;
            Image img, last;
            int frames = 0;
            do {
                last = img;
                img = Render(renderer, scene, frame);
                ++frames;
            } while (!renderer.IsConverged() && frames < 200);
            for (int k = 0; k < 40; ++k) {  // the probes keep blending: the image stays put
                last = img;
                img = Render(renderer, scene, frame);
            }
            const float r = img.At(28, 32, 0), g = img.At(28, 32, 1);
            Expect(std::abs(direct.At(28, 32, 0) - direct.At(28, 32, 1)) < 1e-3f && r > 1.2f * g && g > 0.0f,
                   "GI bounces the red panel's light onto the white wall");
            Expect(frames < 200, "GI converges");
            Expect(std::abs(img.At(28, 32, 0) - last.At(28, 32, 0)) < 0.02f * r, "settled GI is stable frame to frame");

            // Light path intensity: indirect 0 leaves the direct light (the
            // GI-less image), direct 0 the bounced light alone; they add up.
            auto intensity = [&](float d, float i) {
                vkr::Renderer::Frame f = frame;
                f.directIntensity = d;
                f.indirectIntensity = i;
                Image out;
                for (int k = 0; k < 4; ++k) {
                    out = Render(renderer, scene, f);
                }
                return out;
            };
            const Image noIndirect = intensity(1.0f, 0.0f), noDirect = intensity(0.0f, 1.0f), twice = intensity(2.0f, 2.0f);
            for (int c = 0; c < 2; ++c) {
                Expect(std::abs(noIndirect.At(28, 32, c) - direct.At(28, 32, c)) < 0.03f * direct.At(28, 32, c),
                       "indirectIntensity 0: the GI-lit wall keeps its direct light");
                Expect(std::abs(noDirect.At(28, 32, c) + noIndirect.At(28, 32, c) - img.At(28, 32, c)) <
                           0.03f * img.At(28, 32, c),
                       "directIntensity 0 + indirectIntensity 0 images add up to the beauty");
                Expect(std::abs(twice.At(28, 32, c) - 2.0f * img.At(28, 32, c)) < 0.03f * img.At(28, 32, c),
                       "both intensities 2: twice the light");
            }
            Expect(noDirect.At(28, 32, 0) > 1.2f * noDirect.At(28, 32, 1) && noDirect.At(28, 32, 0) < 0.8f * img.At(28, 32, 0),
                   "directIntensity 0: only the red bounce is left");

            // GI preconverge: the film's 8 samples start once the probes
            // have settled (their first 1 / (1 - 0.97) = 33 updates, all in
            // the first frame), so no sample sees half-filled probes; off,
            // the film starts at once and averages them in.
            auto filmFrames = [&](bool preconverge, Image *out) {
                vkr::Renderer r(device);
                vkr::Renderer::Frame f = frame;
                f.postProcess = true;
                f.post.autoExposure = false;
                f.post.bloomIntensity = 0.0f;
                f.post.tonemapper = vkr::PostProcess::Settings::Tonemapper::None;
                f.filmSamples = 8;
                f.giPreconverge = preconverge;
                int n = 0;
                do {
                    *out = Render(r, scene, f);
                    ++n;
                } while (!r.IsConverged() && n < 200);
                return n;
            };
            Image settledFilm, earlyFilm;
            const int settledFrames = filmFrames(true, &settledFilm), earlyFrames = filmFrames(false, &earlyFilm);
            Expect(settledFrames >= 8 && settledFrames <= 8 + 3 && earlyFrames >= 33,
                   "GI preconverge: the 33 updates cost one frame, not 33");
            Image slowGi;
            {
                vkr::Renderer r(device);
                vkr::Renderer::Frame f = frame;
                f.giPreconverge = false;
                for (int n = 0; n < 150; ++n) {
                    slowGi = Render(r, scene, f);
                }
            }
            Expect(std::abs(img.At(28, 32, 0) - slowGi.At(28, 32, 0)) < 0.05f * slowGi.At(28, 32, 0) &&
                       std::abs(img.At(28, 32, 1) - slowGi.At(28, 32, 1)) < 0.05f * slowGi.At(28, 32, 1),
                   "GI preconverge: the burst's probes match the frame by frame ones");
            Expect(settledFilm.At(28, 32, 0) > earlyFilm.At(28, 32, 0),
                   "GI preconverge: no dark half-filled probes in the film");
            Render(renderer, scene);
            Expect(renderer.IsConverged() && renderer.GetStats().accelerationStructureBytes == 0, "GI off: nothing traced");
        }

        if (device.rayTracing) {
            // No light through walls: a closed room (thin walls without
            // normals, its front one behind the camera's near plane) with
            // the sun outside; its back wall only sees the room.
            vkr::Scene scene(&device);
            auto white = std::make_shared<vkr::Material>();
            std::fill(white->baseColor, white->baseColor + 3, 0.8f);
            white->roughness = 1;
            auto wall = [&](std::array<float, 12> c) {
                vkr::Mesh *m = scene.AddMesh();
                m->points = {c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7], c[8],
                             c[0], c[1], c[2], c[6], c[7], c[8], c[9], c[10], c[11]};
                m->material = white;
            };
            wall({-1, -1, 0.5f, 1, -1, 0.5f, 1, 1, 0.5f, -1, 1, 0.5f});            // back
            wall({-1, -1, -1.5f, 1, -1, -1.5f, 1, 1, -1.5f, -1, 1, -1.5f});        // front
            wall({-1, -1, -1.5f, -1, -1, 0.5f, -1, 1, 0.5f, -1, 1, -1.5f});        // left
            wall({1, -1, -1.5f, 1, -1, 0.5f, 1, 1, 0.5f, 1, 1, -1.5f});            // right
            wall({-1, -1, -1.5f, 1, -1, -1.5f, 1, -1, 0.5f, -1, -1, 0.5f});        // floor
            wall({-1, 1, -1.5f, 1, 1, -1.5f, 1, 1, 0.5f, -1, 1, 0.5f});            // ceiling
            AddLight(scene, -1, 0.3f, -0.2f)->castShadows = true;
            scene.Commit();
            vkr::Renderer::Frame frame = IdentityFrame();
            frame.gi = true;
            Image img;
            for (int i = 0; i < 40; ++i) {
                img = Render(renderer, scene, frame);
            }
            float brightest = 0;
            for (int y = 4; y < 60; y += 4) {
                for (int x = 4; x < 60; x += 4) {
                    brightest = std::max(brightest, img.At(x, y, 0));
                }
            }
            Expect(brightest < 0.01f, "GI leaks no sunlight into a closed room");

        }

        if (device.rayTracing) {
            // Fog in a closed room (walls facing in, the camera inside)
            // under a white dome: with GI, the dome's light in the fog comes
            // from the probes, which see only the dark room.
            vkr::Scene scene(&device);
            auto white = std::make_shared<vkr::Material>();
            std::fill(white->baseColor, white->baseColor + 3, 0.8f);
            white->roughness = 1;
            const vkr::Vec3 center{0, 0, -0.5f};
            auto wall = [&](std::array<float, 12> c) {
                vkr::Mesh *m = scene.AddMesh();
                const vkr::Vec3 a{c[0], c[1], c[2]}, b{c[3], c[4], c[5]}, d{c[6], c[7], c[8]};
                if (vkr::Dot(vkr::Cross(b - a, d - a), center - a) < 0.0f) {  // front faces towards the centre
                    std::swap_ranges(c.begin() + 3, c.begin() + 6, c.begin() + 9);
                }
                m->points = {c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7], c[8],
                             c[0], c[1], c[2], c[6], c[7], c[8], c[9], c[10], c[11]};
                m->material = white;
            };
            wall({-1, -1, 0.5f, 1, -1, 0.5f, 1, 1, 0.5f, -1, 1, 0.5f});
            wall({-1, -1, -1.5f, 1, -1, -1.5f, 1, 1, -1.5f, -1, 1, -1.5f});
            wall({-1, -1, -1.5f, -1, -1, 0.5f, -1, 1, 0.5f, -1, 1, -1.5f});
            wall({1, -1, -1.5f, 1, -1, 0.5f, 1, 1, 0.5f, 1, 1, -1.5f});
            wall({-1, -1, -1.5f, 1, -1, -1.5f, 1, -1, 0.5f, -1, -1, 0.5f});
            wall({-1, 1, -1.5f, 1, 1, -1.5f, 1, 1, 0.5f, -1, 1, 0.5f});
            scene.AddDome();
            scene.Commit();
            vkr::Renderer::Frame frame = IdentityFrame();
            frame.cameraPos[2] = -1.4f;  // inside, the front wall behind it
            frame.depthToDistance[1] = -1.4f;
            frame.gi = true;
            frame.fog = true;
            frame.fogDensity = 0.5f;
            frame.fogAnisotropy = 0.0f;
            auto haze = [&](bool giAmbient) {
                frame.fogGiAmbient = giAmbient;
                Image img;
                for (int i = 0; i < 40; ++i) {
                    img = Render(renderer, scene, frame);
                }
                return img.At(32, 32, 0);
            };
            const float open = haze(false), closed = haze(true);
            Expect(open > 0.2f && closed < 0.25f * open, "fog: the GI probes keep the dome's light out of a room");
        } else {
            std::puts("no ray tracing on this GPU: its tests skipped");
        }

        {
            // Screen-space GI (no ray tracing needed): a red emitter leaning
            // from the white wall (x = 0, z = 0.5) towards the camera (x =
            // 0.4, z = 0) and nothing else lighting it: the wall left of it
            // stays black, unless SSGI gathers the emitter's light from last
            // frame's image.
            vkr::Scene scene(&device);
            auto white = std::make_shared<vkr::Material>();
            std::fill(white->baseColor, white->baseColor + 3, 0.8f);
            white->roughness = 1;
            AddQuad(scene, -1, -1, 1, 1, 0.5f)->material = white;
            vkr::Mesh *panel = AddEmitter(scene, 0, 0, 0, 0, 0, 2.0f);
            panel->points = {0, -0.6f, 0.5f, 0.4f, -0.6f, 0, 0.4f, 0.6f, 0, 0, -0.6f, 0.5f, 0.4f, 0.6f, 0, 0, 0.6f, 0.5f};
            auto red = std::make_shared<vkr::Material>(*panel->material);
            red->emissive[1] = red->emissive[2] = 0;
            panel->material = red;
            scene.Commit();
            auto wall = [&](bool ssgi) {
                vkr::Renderer renderer(device);
                vkr::Renderer::Frame frame = IdentityFrame();
                frame.postProcess = true;
                frame.post.autoExposure = false;
                frame.post.bloomIntensity = 0.0f;
                frame.ssgi = ssgi;
                frame.ssgiRadius = 2.0f;
                Image img;
                for (int i = 0; i < 16; ++i) {
                    img = Render(renderer, scene, frame);
                }
                return std::array<float, 2>{img.At(29, 32, 0), img.At(29, 32, 1)};
            };
            const auto off = wall(false), on = wall(true);
            Expect(off[0] < 0.005f && on[0] > 0.03f && on[0] > 4.0f * on[1],
                   "SSGI bleeds the red emitter's light onto the white wall beside it");
        }

        if (device.rayTracing) {
            // Skinned instances block rays: the sun test's occluder as a
            // skinned quad (one joint), its traced shadow on the ground one
            // unit to the left. Its joint moves it up by 0.5: the shadow
            // follows (posed and refit on the GPU); with no skinned instance
            // allowed it casts none.
            vkr::Scene scene(&device);
            vkr::Light *sun = AddLight(scene, 1, 0, -1);
            sun->castShadows = true;
            sun->angle = 0;
            AddQuad(scene, -1, -1, 1, 1, 0.5f)->color[0] = 0.8f;
            vkr::Mesh *occluder = AddQuad(scene, -0.25f, -0.25f, 0.25f, 0.25f, -0.5f);
            occluder->jointCount = 1;
            for (size_t c = 0; c < occluder->points.size() / 3; ++c) {
                occluder->jointIndices.insert(occluder->jointIndices.end(), {0, 0, 0, 0});
                occluder->jointWeights.insert(occluder->jointWeights.end(), {1, 0, 0, 0});
            }
            auto lift = [&](float y) {
                occluder->skinningTransforms = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, y, 0, 1};
                occluder->jointsDirty = true;
                scene.Commit();
            };
            lift(0);
            vkr::Renderer::Frame frame = IdentityFrame();
            frame.rtShadows = true;
            const Image rest = Render(renderer, scene, frame);
            lift(0.5f);
            const Image lifted = Render(renderer, scene, frame);
            const float lit = rest.At(51, 32, 0);
            // Row 32 (y = 0) is in the rest shadow only, row 48 (y = 0.52) in the lifted one only.
            Expect(rest.At(3, 32, 0) < 0.01f * lit && rest.At(3, 48, 0) > 0.9f * lit,
                   "a skinned occluder casts a traced shadow");
            Expect(lifted.At(3, 32, 0) > 0.9f * lit && lifted.At(3, 48, 0) < 0.01f * lit,
                   "its traced shadow follows a moved joint");
            frame.rtSkinnedInstances = 0;
            Expect(Render(renderer, scene, frame).At(3, 48, 0) > 0.9f * lit,
                   "Frame::rtSkinnedInstances 0: skinned meshes don't block rays");

            // Displaced after skinning: the occluder's material moves it 0.5
            // up along its normals, so its shadow does what the lifted
            // joint's did; with rtDisplacement off, rays see the rest quad.
            auto material = std::make_shared<vkr::Material>();
            material->displacement = 0.5f;
            material->displacementBound = 0.5f;
            occluder->material = material;
            occluder->normals.clear();
            for (size_t c = 0; c < occluder->points.size() / 3; ++c) {
                occluder->normals.insert(occluder->normals.end(), {0, 1, 0});
            }
            occluder->geometryDirty = true;
            lift(0);
            frame = IdentityFrame();
            frame.rtShadows = true;
            const Image displaced = Render(renderer, scene, frame);
            Expect(displaced.At(3, 32, 0) > 0.9f * lit && displaced.At(3, 48, 0) < 0.01f * lit,
                   "a skinned occluder's traced shadow follows its displacement");
            frame.rtDisplacement = false;
            const Image restShadow = Render(renderer, scene, frame);
            Expect(restShadow.At(3, 32, 0) < 0.01f * lit && restShadow.At(3, 48, 0) > 0.9f * lit,
                   "Frame::rtDisplacement off: rays see the skinned rest surface");
        }

        {
            // Wrap modes: emissive rows with st (0..2, 0..1) over a 2x1
            // texture (left texel red, right blue), one row per mode, the
            // last a program's image; u 1.27 (pixel 40) and 1.77 (pixel 56)
            // read past the texture. A last row wraps t (0..2) black above
            // v = 1, s repeating.
            vkr::Scene scene(&device);
            auto tex = std::make_shared<vkr::Texture>();
            tex->width = 2;
            tex->height = 1;
            tex->pixels = {255, 0, 0, 255, 0, 0, 255, 255};
            using W = vkr::Wrap;
            const W modes[4] = {W::Repeat, W::Mirror, W::Clamp, W::Black};
            auto row = [&](int i, W s, W t, bool program, float v1) {
                vkr::Mesh *q = AddEmitter(scene, -1, -1 + 0.3f * float(i), 1, -0.75f + 0.3f * float(i), 0, 0.0f);
                q->uvs = {0, 0, 2, 0, 2, v1, 0, 0, 2, v1, 0, v1};
                vkr::TextureRef ref;
                ref.texture = tex;
                ref.wrap[0] = s;
                ref.wrap[1] = t;
                using P = vkr::MaterialProgram;
                if (program) {
                    P &p = q->material->program;
                    p.images.push_back(ref);
                    p.Emit(P::kTexcoord, 0);
                    p.Emit(P::kImage, 1, {0, p.Constant(0, 0, 0, 0)}, 0);
                    p.Emit(P::kOutput, 0, {1}, P::kEmissive);
                } else {
                    q->material->emissiveTex = ref;
                }
            };
            for (int i = 0; i < 4; ++i) {
                row(i, modes[i], W::Repeat, false, 1);
            }
            row(4, W::Mirror, W::Repeat, true, 1);
            row(5, W::Repeat, W::Black, false, 2);
            scene.Commit();
            Expect(scene.GetGpu().textures.size() == 5, "a texture per wrap pair in the bindless array");
            const Image img = Render(renderer, scene);
            auto red = [&](int x, int y) { return img.At(x, y, 0) > 0.9f && img.At(x, y, 2) < 0.1f; };
            auto blue = [&](int x, int y) { return img.At(x, y, 2) > 0.9f && img.At(x, y, 0) < 0.1f; };
            auto black = [&](int x, int y) { return img.At(x, y, 0) < 0.01f && img.At(x, y, 2) < 0.01f; };
            auto y = [](int i) { return int((0.3f * float(i) + 0.125f) * 32.0f); };  // a row's middle
            Expect(red(8, y(0)) && red(40, y(0)) && blue(56, y(0)), "wrap repeat");
            Expect(red(8, y(1)) && blue(40, y(1)) && red(56, y(1)), "wrap mirror");
            Expect(red(8, y(2)) && blue(40, y(2)) && blue(56, y(2)), "wrap clamp");
            Expect(red(8, y(3)) && black(40, y(3)) && black(56, y(3)), "wrap black");
            Expect(red(8, y(4)) && blue(40, y(4)) && red(56, y(4)), "wrap mirror in a program's image");
            // (The one texel row blends 1/8 with the border at v 0.625.)
            Expect(img.At(8, y(5) - 2, 0) > 0.8f && black(8, y(5) + 2), "wrap t black, s repeat");
        }

        {
            // Procedurals: emission programs. Left half: the object-space
            // position of a quad its instance scales by 0.5 and moves left by
            // 0.5. Upper right: the world position. Lower right: noise2d
            // (amplitude 0.5, pivot 0.25) at uv * 4, which
            // Frame::proceduralTextures off makes its pivot.
            vkr::Scene scene(&device);
            using P = vkr::MaterialProgram;
            vkr::Mesh *object = AddEmitter(scene, -1, -1, 1, 1, 0, 0.0f);
            object->transforms = {0.5f, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -0.5f, 0, 0, 1};
            P &po = object->material->program;
            po.Emit(P::kPosition, 0, {}, 0);
            po.Emit(P::kOutput, 0, {0}, P::kEmissive);
            P &pw = AddEmitter(scene, 0, 0, 1, 1, 0, 0.0f)->material->program;
            pw.Emit(P::kPosition, 0, {}, 1);
            pw.Emit(P::kOutput, 0, {0}, P::kEmissive);
            P &pn = AddEmitter(scene, 0, -1, 1, 0, 0, 0.0f)->material->program;
            pn.Emit(P::kTexcoord, 0);
            pn.Emit(P::kMultiply, 0, {0, pn.Constant(4, 4, 4, 4)});
            pn.Emit(P::kNoise, 0, {0, pn.Constant(0.5f, 0.5f, 0.5f, 0.5f), pn.Constant(0.25f, 0.25f, 0.25f, 0.25f)}, 2, 1);
            pn.Emit(P::kOutput, 0, {0}, P::kEmissive);
            scene.Commit();
            const Image img = Render(renderer, scene);
            auto near = [&](const Image &i, int x, int y, float r, float g, float b) {
                return std::abs(i.At(x, y, 0) - r) < 0.01f && std::abs(i.At(x, y, 1) - g) < 0.01f &&
                       std::abs(i.At(x, y, 2) - b) < 0.01f;
            };
            Expect(near(img, 24, 40, (PixelNdc(24) + 0.5f) / 0.5f, PixelNdc(40), 0) &&
                       near(img, 6, 60, (PixelNdc(6) + 0.5f) / 0.5f, PixelNdc(60), 0),
                   "procedural: object-space position under the instance transform");
            Expect(near(img, 40, 50, PixelNdc(40), PixelNdc(50), 0), "procedural: world position");
            float lo = 1, hi = 0;
            for (int x = 34; x < 62; ++x) {
                lo = std::min(lo, img.At(x, 16, 0));
                hi = std::max(hi, img.At(x, 16, 0));
            }
            Expect(lo > -0.25f && hi < 0.75f && hi - lo > 0.1f &&
                       near(img, 48, 16, img.At(48, 16, 0), img.At(48, 16, 0), img.At(48, 16, 0)),
                   "procedural: noise2d varies around its pivot, float splat");
            vkr::Renderer::Frame frame = IdentityFrame();
            frame.proceduralTextures = false;
            const Image off = Render(renderer, scene, frame);
            Expect(near(off, 40, 16, 0.25f, 0.25f, 0.25f) && near(off, 56, 4, 0.25f, 0.25f, 0.25f),
                   "Frame::proceduralTextures off: noise reads 0 (its pivot)");
        }

        {
            // AgX tonemapping: auto-exposure puts a uniform screen at middle
            // gray, which each curve maps to its own value (none: as is).
            vkr::Scene scene(&device);
            AddEmitter(scene, -1, -1, 1, 1, 0, 1.0f);
            scene.Commit();
            using T = vkr::PostProcess::Settings::Tonemapper;
            for (auto [tonemapper, expected, what] :
                 {std::tuple{T::Aces, 0.106f, "tonemap: ACES(0.18) = 0.106"},
                  std::tuple{T::Agx, 0.214f, "tonemap: AgX(0.18) = 0.214"},
                  std::tuple{T::AgxPunchy, 0.097f, "tonemap: AgX punchy(0.18) = 0.097"},
                  std::tuple{T::None, 0.18f, "tonemap: none(0.18) = 0.18"}}) {
                vkr::Renderer post(device);
                vkr::Renderer::Frame frame = IdentityFrame();
                frame.postProcess = true;
                frame.taa = false;
                frame.post.bloomIntensity = 0.0f;
                frame.post.tonemapper = tonemapper;
                Expect(std::abs(Render(post, scene, frame).At(32, 32, 1) - expected) < 0.01f, what);
            }
        }

        {
            // Lens distortion: barrel (k1 > 0) bows a vertical strip right
            // of the centre into a ")", and ids and depth move with the colour
            // (row 55 still looks inside the unwidened view).
            vkr::Scene scene(&device);
            vkr::Mesh *strip = AddEmitter(scene, 0.4f, -1, 0.6f, 1, 0, 1.0f);
            strip->primId = 7;
            scene.Commit();
            vkr::Renderer::Frame frame = IdentityFrame();
            frame.distortion.k1 = 0.5f;
            const Image warped = Render(renderer, scene, frame);
            frame.lensDistortion = false;
            const Image straight = Render(renderer, scene, frame);
            auto centre = [](const Image &img, int y) {
                float sum = 0, count = 0;
                for (int x = 0; x < 64; ++x) {
                    const bool lit = img.At(x, y, 0) > 0.5f;
                    sum += lit ? float(x) : 0.0f;
                    count += lit ? 1.0f : 0.0f;
                }
                return count > 0 ? sum / count : -1.0f;
            };
            Expect(std::abs(centre(straight, 32) - 47.5f) < 0.6f && std::abs(centre(straight, 63) - 47.5f) < 0.6f,
                   "distortion off: the strip stays straight");
            Expect(std::abs(centre(warped, 32) - 46.0f) < 1.0f && std::abs(centre(warped, 55) - 43.5f) < 1.0f,
                   "barrel distortion bows the strip");
            Expect(warped.Prim(46, 32) == 7 && warped.Prim(51, 32) == -1 && warped.depth[32 * 64 + 51] == 1.0f &&
                       std::abs(warped.depth[32 * 64 + 46] - 0.5f) < 1e-3f,
                   "ids and depth are warped with the colour");
            {
                // An EXR's linear beauty (what husk and render products
                // read) is warped like the colour.
                vkr::Renderer::Frame linear = IdentityFrame();
                linear.distortion.k1 = 0.5f;
                linear.postProcess = true;
                linear.linearColor = true;
                linear.taa = false;
                linear.post.autoExposure = false;
                linear.post.bloomIntensity = 0.0f;
                linear.post.tonemapper = vkr::PostProcess::Settings::Tonemapper::None;
                Render(renderer, scene, linear);
                std::vector<uint16_t> pixels(64 * 64 * 4);
                vkr::Renderer::Targets read;
                read.linearColor = pixels.data();
                renderer.Read(read);
                float sum = 0, count = 0;
                for (int x = 0; x < 64; ++x) {
                    const bool lit = HalfToFloat(pixels[(55 * 64 + x) * 4]) > 0.5f;
                    sum += lit ? float(x) : 0.0f;
                    count += lit ? 1.0f : 0.0f;
                }
                Expect(count > 0 && std::abs(sum / count - 43.5f) < 1.0f, "distortion warps the linear beauty too");
            }

            // The warp's reach: a corner of k1 = 0.1 looks 1.2 times as far
            // out; a fisheye's 45 degree half view reaches tan(1 rad).
            vkr::LensDistortion::Params p;
            p.k1 = 0.1f;
            Expect(std::abs(vkr::LensDistortion::Overscan(p, 1.0f) - 1.2f) < 1e-3f, "overscan of a barrel lens");
            p = {};
            p.fisheye = true;
            const float edge[2] = {1, 0};
            float source[2];
            Expect(vkr::LensDistortion::Warp(p, 1.0f, edge, source) && std::abs(source[0] - std::tan(1.0f)) < 1e-4f,
                   "equidistant fisheye");
            // OpenCV's model (Karma's lens material): k4 divides, p1 and p2
            // shift tangentially.
            p = {};
            p.k4 = 0.5f;
            vkr::LensDistortion::Warp(p, 1.0f, edge, source);
            Expect(std::abs(source[0] - 1.0f / 1.5f) < 1e-5f, "OpenCV k4: 1 / (1 + k4 r^2)");
            p = {};
            p.p1 = 0.1f;
            p.p2 = 0.2f;
            const float corner[2] = {0.5f, 0.5f};
            vkr::LensDistortion::Warp(p, 1.0f, corner, source);
            Expect(std::abs(source[0] - (0.5f + 0.05f + 0.2f)) < 1e-5f && std::abs(source[1] - (0.5f + 0.1f + 0.1f)) < 1e-5f,
                   "OpenCV p1, p2: tangential");
        }

        {
            // Blend shape motion: a skinned quad (one joint at rest) whose
            // blend shape lifts its top corners by 0.4. At row 40 (y = 0.27)
            // the surface rose 0.22 between weight 0 and 1 (motion vector
            // -0.11 uv); back at weight 0 it sank 0.31 (+0.15).
            vkr::Scene scene(&device);
            AddLight(scene, 0, 0, -1);
            vkr::Mesh *quad = AddQuad(scene, -0.5f, -0.5f, 0.5f, 0.5f, 0);
            quad->jointCount = 1;
            quad->skinningTransforms = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            quad->blendOffsets = {0, 0.4f, 0, 0};
            for (size_t c = 0; c < quad->points.size() / 3; ++c) {
                quad->jointIndices.insert(quad->jointIndices.end(), {0, 0, 0, 0});
                quad->jointWeights.insert(quad->jointWeights.end(), {1, 0, 0, 0});
                quad->blendRanges.insert(quad->blendRanges.end(), {0u, quad->points[c * 3 + 1] > 0 ? 1u : 0u});
            }
            quad->blendShapeCount = 1;
            quad->blendWeights = {0.0f};
            scene.Commit();
            Render(renderer, scene);
            auto weight = [&](float w, bool motion) {
                quad->blendWeights = {w};
                quad->jointsDirty = true;
                scene.Commit();
                vkr::Renderer::Frame frame = IdentityFrame();
                frame.blendShapeMotion = motion;
                return Render(renderer, scene, frame).Vel(32, 40, 1);
            };
            Expect(std::abs(weight(1.0f, true) + 0.11f) < 0.005f, "blend shape motion: rising");
            Expect(std::abs(weight(0.0f, true) - 0.153f) < 0.005f, "blend shape motion: sinking");
            Expect(std::abs(weight(1.0f, false)) < 1e-4f, "Frame::blendShapeMotion off: no blend shape motion");
        }

        {
            // Extra AOVs and Cryptomatte: a quad at z = 0.5 facing the camera
            // (identity view), uv across it; nothing around it.
            Expect(vkr::CryptomatteHash("The quick brown fox jumps over the lazy dog") == 0x2e4ff723u &&
                       vkr::CryptomatteHash("") == 0x00800000u,
                   "Cryptomatte ids: MurmurHash3, no denormal floats");
            vkr::Scene scene(&device);
            AddLight(scene, 0, 0, -1);
            vkr::Mesh *quad = AddQuad(scene, -0.5f, -0.5f, 0.5f, 0.5f, 0.5f);
            quad->primId = 3;
            auto material = std::make_shared<vkr::Material>();
            const float base[3] = {0.2f, 0.4f, 0.6f}, emissive[3] = {1.0f, 0.5f, 0.25f};
            std::copy(base, base + 3, material->baseColor);
            std::copy(emissive, emissive + 3, material->emissive);
            material->metallic = 0.25f;
            quad->material = material;
            scene.Commit();
            std::vector<std::vector<float>> aovs(vkr::Aovs::kCount);
            vkr::Renderer::Targets out;
            for (uint32_t k = 0; k < vkr::Aovs::kCount; ++k) {
                aovs[k].assign(64 * 64 * vkr::Aovs::Components(vkr::Aovs::Kind(k)), -7.0f);
                out.aovs[k] = aovs[k].data();
            }
            const uint32_t ids[8] = {0, 0, 0, 0, 0, 0, vkr::CryptomatteHash("/quad"), vkr::CryptomatteHash("/mat")};
            vkr::Renderer::Frame frame = IdentityFrame();
            frame.cryptomatte = ids;
            frame.cryptomatteIds = 4;
            renderer.Render(scene, frame, out);
            auto at = [&](vkr::Aovs::Kind k, int x, int y, int c) {
                return aovs[k][(y * 64 + x) * vkr::Aovs::Components(k) + c];
            };
            auto near = [](float a, float b) { return std::abs(a - b) < 2e-3f; };
            using A = vkr::Aovs;
            Expect(near(at(A::kNormal, 32, 32, 2), -1.0f) && near(at(A::kNormalEye, 32, 32, 2), -1.0f),
                   "AOV normal and Neye face the camera");
            Expect(near(at(A::kPositionEye, 32, 32, 2), 0.5f) && near(at(A::kCameraDepth, 32, 32, 0), 0.5f) &&
                       near(at(A::kPositionEye, 32, 32, 0), 1.0f / 64.0f),
                   "AOV Peye and cameraDepth");
            Expect(near(at(A::kSt, 32, 32, 0), 0.5f + 1.0f / 64.0f) && near(at(A::kSt, 32, 32, 1), 0.5f + 1.0f / 64.0f),
                   "AOV primvars:st");
            Expect(near(at(A::kAlbedo, 32, 32, 1), 0.4f) && near(at(A::kEmission, 32, 32, 2), 0.25f),
                   "AOV albedo and emission");
            Expect(near(at(A::kPositionWorld, 32, 32, 0), 1.0f / 64.0f) && near(at(A::kPositionWorld, 32, 32, 2), 0.5f) &&
                       near(at(A::kDepth, 32, 32, 0), -0.5f) && near(at(A::kDiffuseAlbedo, 32, 32, 1), 0.3f),
                   "AOV P, depth (-camera z) and diffuse albedo (Karma's)");
            float object, matte;
            std::memcpy(&object, &ids[6], 4);
            std::memcpy(&matte, &ids[7], 4);
            Expect(at(A::kCryptoObject, 32, 32, 0) == object && at(A::kCryptoObject, 32, 32, 1) == 1.0f &&
                       at(A::kCryptoMaterial, 32, 32, 0) == matte && at(A::kCryptoMaterial, 32, 32, 2) == 0.0f,
                   "Cryptomatte object and material ids at coverage 1");
            bool empty = true;
            for (uint32_t k = 0; k < A::kCount; ++k) {
                for (uint32_t c = 0; c < A::Components(A::Kind(k)); ++c) {
                    empty = empty && at(A::Kind(k), 2, 2, int(c)) == 0.0f;
                }
            }
            Expect(empty, "AOVs are 0 where nothing was drawn");

            // A target may take another number of components: st as three.
            std::vector<float> st3(64 * 64 * 3, -7.0f);
            vkr::Renderer::Targets three;
            three.aovs[A::kSt] = st3.data();
            three.aovComponents[A::kSt] = 3;
            renderer.Render(scene, frame, three);
            const float *texel = &st3[(32 * 64 + 32) * 3];
            Expect(near(texel[0], 0.5f + 1.0f / 64.0f) && texel[2] == 0.0f && st3[(2 * 64 + 2) * 3 + 2] == 0.0f,
                   "AOV components as the target asks");
        }

        {
            // Specular workflow: F0 is specularColor. A black quad with F0
            // (0.5, 0, 0) under the head-on light reflects 12.5 times the
            // default material's 0.16 (plus multiple scattering) in red and
            // nothing in green; the setting off falls back to F0 0.04.
            vkr::Scene scene(&device);
            AddLight(scene, 0, 0, -1);
            auto material = std::make_shared<vkr::Material>();
            std::fill(material->baseColor, material->baseColor + 3, 0.0f);
            material->specularWorkflow = true;
            material->specularColor[0] = 0.5f;
            AddQuad(scene, -1, -1, 1, 1, 0)->material = material;
            scene.Commit();
            vkr::Renderer::Frame frame = IdentityFrame();
            const Image on = Render(renderer, scene, frame);
            frame.specularWorkflow = false;
            const Image off = Render(renderer, scene, frame);
            Expect(on.At(32, 32, 0) > 2.0f && on.At(32, 32, 0) < 2.2f && on.At(32, 32, 1) < 0.001f,
                   "specular workflow: specularColor is F0");
            Expect(std::abs(off.At(32, 32, 0) - 0.16f) < 0.02f && std::abs(off.At(32, 32, 1) - 0.16f) < 0.02f,
                   "Frame::specularWorkflow off: F0 0.04");
        }

        {
            // Clearcoat: a coat as rough as the base (0.5, F0 0.04) over a
            // black dielectric adds the base's own 0.16 under the head-on
            // light, minus the few percent it takes from the base; a red
            // coat colour tints the white diffuse beneath. Under a uniform
            // dome (white furnace) the coated white surface still returns 1.
            vkr::Scene scene(&device);
            AddLight(scene, 0, 0, -1);
            auto material = std::make_shared<vkr::Material>();
            std::fill(material->baseColor, material->baseColor + 3, 0.0f);
            material->coat = 1;
            material->coatRoughness = 0.5f;
            AddQuad(scene, -1, -1, 1, 1, 0)->material = material;
            scene.Commit();
            vkr::Renderer::Frame frame = IdentityFrame();
            const float coated = Render(renderer, scene, frame).At(32, 32, 1);
            frame.clearcoat = false;
            const float bare = Render(renderer, scene, frame).At(32, 32, 1);
            Expect(std::abs(bare - 0.16f) < 0.02f && coated > 1.8f * bare && coated < 2.0f * bare,
                   "clearcoat adds a second specular lobe");
            std::fill(material->baseColor, material->baseColor + 3, 1.0f);
            material->coatColor[1] = material->coatColor[2] = 0.0f;
            scene.Commit();
            frame.clearcoat = true;
            const Image red = Render(renderer, scene, frame);
            Expect(red.At(32, 32, 0) > 1.0f && red.At(32, 32, 1) < 0.4f, "coat colour tints the base beneath");

            vkr::Scene furnace(&device);
            furnace.AddDome();
            auto white = std::make_shared<vkr::Material>();
            std::fill(white->baseColor, white->baseColor + 3, 1.0f);
            white->roughness = 0.7f;
            white->coat = 1;
            white->coatRoughness = 0.1f;
            AddQuad(furnace, -1, -1, 1, 1, 0)->material = white;
            furnace.Commit();
            const float v = Render(renderer, furnace).At(32, 32, 0);
            Expect(std::abs(v - 1.0f) < 0.03f, "furnace: coated white dielectric returns 1");
        }

        {
            // Sheen: microfibers shine at grazing angles. A strip tilted 80
            // degrees away from the view, lit from the camera: the Imageworks
            // lobe (roughness 0.3) adds D * V * cos * pi = 0.81 * 0.8 * 0.17
            // * pi = 0.35. Under a uniform dome (white furnace) white sheen
            // over a white diffuse still returns 1.
            vkr::Scene scene(&device);
            AddLight(scene, 0, 0, -1);
            const float c = std::cos(80.0f * 3.14159265f / 180.0f), s = std::sin(80.0f * 3.14159265f / 180.0f);
            vkr::Mesh *strip = AddQuad(scene, 0, 0, 0, 0, 0);
            strip->points = {-1, -c, -s, 1, -c, -s, 1, c, s, -1, -c, -s, 1, c, s, -1, c, s};
            auto material = std::make_shared<vkr::Material>();
            std::fill(material->baseColor, material->baseColor + 3, 0.0f);
            material->sheen = 1;
            strip->material = material;
            scene.Commit();
            vkr::Renderer::Frame frame = IdentityFrame();
            const float on = Render(renderer, scene, frame).At(32, 32, 1);
            frame.sheen = false;
            const float off = Render(renderer, scene, frame).At(32, 32, 1);
            Expect(on - off > 0.3f && on - off < 0.4f, "sheen brightens grazing angles");

            vkr::Scene furnace(&device);
            furnace.AddDome();
            auto white = std::make_shared<vkr::Material>();
            std::fill(white->baseColor, white->baseColor + 3, 1.0f);
            white->roughness = 0.7f;
            white->sheen = 1;
            AddQuad(furnace, -1, -1, 1, 1, 0)->material = white;
            furnace.Commit();
            const float v = Render(renderer, furnace).At(32, 32, 0);
            Expect(std::abs(v - 1.0f) < 0.03f, "furnace: white sheen over white returns 1");
        }

        {
            // Thin film: 300 nm of index 1.8 on a black dielectric reflects
            // (0.095, 0.043, 0.122) head-on (Belcour & Barla's model, worked
            // out on the CPU), purple instead of the bare 0.04 grey: times
            // 4 under the head-on light (the default material's 0.16 / 0.04).
            vkr::Scene scene(&device);
            AddLight(scene, 0, 0, -1);
            auto material = std::make_shared<vkr::Material>();
            std::fill(material->baseColor, material->baseColor + 3, 0.0f);
            material->thinFilm = 1;
            material->thinFilmThickness = 300;
            material->thinFilmIor = 1.8f;
            AddQuad(scene, -1, -1, 1, 1, 0)->material = material;
            scene.Commit();
            vkr::Renderer::Frame frame = IdentityFrame();
            const Image on = Render(renderer, scene, frame);
            frame.thinFilm = false;
            const Image off = Render(renderer, scene, frame);
            const float expected[3] = {0.095f * 4, 0.043f * 4, 0.122f * 4};
            bool close = true;
            for (int c = 0; c < 3; ++c) {
                close = close && std::abs(on.At(32, 32, c) / expected[c] - 1.0f) < 0.15f;
            }
            Expect(close, "thin film reflects its interference colour");
            Expect(std::abs(off.At(32, 32, 0) - 0.16f) < 0.02f && std::abs(off.At(32, 32, 2) - 0.16f) < 0.02f,
                   "Frame::thinFilm off: the bare base");
        }

        {
            // Anisotropy: on a black quad whose u runs along x, a light
            // tilted towards x shines on the centre far more than one tilted
            // as much towards y (the highlight stretches along u); a quarter
            // turn of rotation swaps them. Nearly isotropic, the anisotropic
            // GGX is the isotropic one.
            vkr::Scene scene(&device);
            vkr::Light *light = AddLight(scene, 0, 0, -1);
            auto material = std::make_shared<vkr::Material>();
            std::fill(material->baseColor, material->baseColor + 3, 0.0f);
            material->roughness = 0.3f;
            material->anisotropy = 0.8f;
            AddQuad(scene, -1, -1, 1, 1, 0)->material = material;
            auto specular = [&](float x, float y) {
                const float len = std::sqrt(x * x + y * y + 1);
                light->direction[0] = x / len;
                light->direction[1] = y / len;
                light->direction[2] = -1 / len;
                scene.Commit();
                return Render(renderer, scene).At(32, 32, 1);
            };
            Expect(specular(0.4f, 0) > 3 * specular(0, 0.4f), "anisotropic highlight stretches along u");
            material->anisotropyRotation = 0.25f;
            Expect(specular(0, 0.4f) > 3 * specular(0.4f, 0), "rotated a quarter turn it stretches along v");
            vkr::Renderer::Frame frame = IdentityFrame();
            frame.anisotropicSpecular = false;
            const float across = specular(0.4f, 0);
            Expect(Render(renderer, scene, frame).At(32, 32, 1) > 2 * across, "Frame::anisotropicSpecular off: isotropic");
            material->anisotropy = 1e-3f;
            const float nearly = specular(0.4f, 0);
            Expect(std::abs(nearly - Render(renderer, scene, frame).At(32, 32, 1)) < 0.01f * nearly,
                   "nearly isotropic anisotropic GGX is GGX");
        }

        {
            // Toon: with two bands per unit a white quad lit at cos 0.6 and
            // 0.7 shows the same flat band (both in [0.5, 1), shown at 0.75),
            // at cos 0.4 the band below (0.25), a third as bright.
            // Frame::toonBands bands a plain material the same way; toon off
            // is smooth again.
            vkr::Scene scene(&device);
            vkr::Light *light = AddLight(scene, 0, 0, -1);
            auto material = std::make_shared<vkr::Material>();
            std::fill(material->baseColor, material->baseColor + 3, 1.0f);
            material->roughness = 1;
            material->toonBands = 2;
            AddQuad(scene, -1, -1, 1, 1, 0)->material = material;
            vkr::Renderer::Frame frame = IdentityFrame();
            auto lit = [&](float cosine) {
                light->direction[0] = std::sqrt(1 - cosine * cosine);
                light->direction[2] = -cosine;
                scene.Commit();
                return Render(renderer, scene, frame).At(32, 32, 0);
            };
            auto banded = [&](const char *what) {
                const float a = lit(0.6f), b = lit(0.7f), c = lit(0.4f);
                Expect(std::abs(a - b) < 0.01f * a && std::abs(c / a - 1.0f / 3.0f) < 0.05f, what);
            };
            banded("toon: flat bands");
            frame.toon = false;
            Expect(lit(0.7f) > 1.1f * lit(0.6f), "Frame::toon off: smooth");
            material->toonBands = 0;
            frame.toon = true;
            frame.toonBands = 2;
            banded("Frame::toonBands: every material in bands");
        }

        {
            // Hair: strands along u (x here) light up where the half vector
            // is across them. A red strip without a material: in green (the
            // white primary lobe; the diffuse is red) a light tilted 45
            // degrees towards y shines on the centre far more than one tilted
            // as much towards x. Without hair shading it is isotropic GGX.
            vkr::Scene scene(&device);
            vkr::Light *light = AddLight(scene, 0, 0, -1);
            vkr::Mesh *strip = AddQuad(scene, -1, -0.2f, 1, 0.2f, 0);
            strip->color[0] = 1;
            strip->color[1] = strip->color[2] = 0;
            strip->hair = true;
            vkr::Renderer::Frame frame = IdentityFrame();
            auto specular = [&](float x, float y) {
                const float len = std::sqrt(x * x + y * y + 1);
                light->direction[0] = x / len;
                light->direction[1] = y / len;
                light->direction[2] = -1 / len;
                scene.Commit();
                return Render(renderer, scene, frame).At(32, 32, 1);
            };
            Expect(specular(0, 1) > 2 * specular(1, 0), "hair: highlights across the strands");
            frame.hairShading = false;
            Expect(std::abs(specular(0, 1) / specular(1, 0) - 1) < 0.05f, "Frame::hairShading off: isotropic");
        }

        {
            // Order-independent transparency: one translucent mesh (opacity
            // 0.5, displayColor) of two quads over the left half of an opaque
            // red one, the near green quad's triangles listed before the far
            // blue one's. Weighted blended OIT lets the nearer dominate
            // whatever the triangle order; the sorted mode blends in list
            // order (blue over green).
            vkr::Scene scene(&device);
            AddLight(scene, 0, 0, -1);
            auto red = std::make_shared<vkr::Material>(), glass = std::make_shared<vkr::Material>();
            red->baseColor[0] = 1;
            red->baseColor[1] = red->baseColor[2] = 0;
            red->roughness = 1;
            glass->baseColorFromDisplayColor = true;
            glass->roughness = 1;
            glass->opacity = 0.5f;
            AddQuad(scene, -1, -1, 1, 1, 0.5f)->material = red;
            vkr::Mesh *layers = AddQuad(scene, -1, -1, 0, 1, 0);
            const std::vector<float> points = layers->points, uvs = layers->uvs;
            layers->points.insert(layers->points.end(), points.begin(), points.end());
            layers->uvs.insert(layers->uvs.end(), uvs.begin(), uvs.end());
            for (size_t i = 18; i < 36; i += 3) {
                layers->points[i + 2] = 0.25f;  // the second quad behind the first
            }
            for (int corner = 0; corner < 12; ++corner) {
                layers->colors.insert(layers->colors.end(), {0, corner < 6 ? 1.0f : 0.0f, corner < 6 ? 0.0f : 1.0f});
            }
            layers->material = glass;
            scene.Commit();
            const Image oit = Render(renderer, scene);
            vkr::Renderer::Frame frame = IdentityFrame();
            frame.oit = false;
            const Image sorted = Render(renderer, scene, frame);
            Expect(oit.At(16, 32, 1) > oit.At(16, 32, 2) + 0.1f && std::abs(oit.At(16, 32, 0) - 0.25f) < 0.06f,
                   "OIT: the nearer layer dominates whatever the triangle order, the opaque shows by revealage");
            Expect(sorted.At(16, 32, 2) > sorted.At(16, 32, 1) + 0.1f, "sorted mode: blended in list order");
            Expect(std::abs(oit.At(48, 32, 0) - sorted.At(48, 32, 0)) < 1e-3f && oit.At(48, 32, 1) < 0.05f,
                   "OIT leaves pixels without translucency alone");
        }

        {
            // Refraction: a glass quad (transmission 1) turned 50 degrees
            // about y, in front of a red (x < 0) and green (x > 0) wall seen
            // through a perspective camera. An ior moves the colour edge,
            // roughness blurs it, the transmission colour tints, and with
            // Frame::refraction off the wall shows straight and untinted.
            vkr::Scene scene(&device);
            vkr::Mesh *left = AddEmitter(scene, -6, -6, 0, 6, -5, 1.0f);
            left->material->emissive[1] = left->material->emissive[2] = 0;
            vkr::Mesh *right = AddEmitter(scene, 0, -6, 6, 6, -5, 1.0f);
            right->material->emissive[0] = right->material->emissive[2] = 0;
            vkr::Mesh *glassQuad = AddQuad(scene, -1, -1, 1, 1, 0);
            const float t = std::tan(50.0f * 3.14159265f / 180.0f);
            for (size_t i = 0; i < glassQuad->points.size(); i += 3) {
                glassQuad->points[i + 2] = -2.5f + glassQuad->points[i] * t;
            }
            auto glass = std::make_shared<vkr::Material>();
            std::fill(glass->baseColor, glass->baseColor + 3, 1.0f);
            glass->roughness = 0;
            glass->transmission = 1;
            glassQuad->material = glass;
            auto edge = [&](const Image &img) {  // first pixel of row 32 more green than red
                int x = 0;
                while (x < 63 && img.At(x, 32, 1) <= img.At(x, 32, 0)) {
                    ++x;
                }
                return x;
            };
            auto blurred = [&](const Image &img) {  // pixels of row 32 mixing both
                int n = 0;
                for (int x = 0; x < 64; ++x) {
                    n += img.At(x, 32, 0) > 0.1f && img.At(x, 32, 1) > 0.1f;
                }
                return n;
            };
            vkr::Renderer::Frame frame = PerspectiveFrame();
            glass->ior = 1.0f;
            scene.Commit();
            const Image straight = Render(renderer, scene, frame);
            glass->ior = 1.5f;
            scene.Commit();
            const Image bent = Render(renderer, scene, frame);
            Expect(std::abs(edge(bent) - edge(straight)) >= 2, "refraction: an ior moves the edge seen through glass");
            Expect(straight.At(44, 32, 1) > 0.8f && straight.At(44, 32, 0) < 0.05f,
                   "refraction: smooth clear glass transmits almost all");
            glass->roughness = 0.5f;
            scene.Commit();
            Expect(blurred(Render(renderer, scene, frame)) > blurred(bent) + 2, "refraction: roughness blurs");
            glass->roughness = 0;
            std::copy_n(std::array<float, 3>{1, 0.2f, 0.2f}.data(), 3, glass->transmissionColor);
            scene.Commit();
            const Image tinted = Render(renderer, scene, frame);
            Expect(tinted.At(44, 32, 1) < 0.25f * bent.At(44, 32, 1) && tinted.At(44, 32, 1) > 0.05f,
                   "refraction: the transmission colour tints");
            frame.refraction = false;
            const Image off = Render(renderer, scene, frame);
            Expect(off.At(44, 32, 1) > 0.8f && edge(off) == edge(straight),
                   "Frame::refraction off: straight through, untinted");
        }

        {
            // Translucent shadows: the sun shadow test's occluder made of red
            // glass (transmission 1, ior 1.5: 8% lost to its two interfaces),
            // then of opacity 0.5, then the same for a spot and a point
            // light's atlas tiles.
            vkr::Scene scene(&device);
            vkr::Light *sun = AddLight(scene, 1, 0, -1);
            sun->castShadows = true;
            AddQuad(scene, -1, -1, 1, 1, 0.5f)->color[0] = 0.8f;
            vkr::Mesh *occluder = AddQuad(scene, -0.25f, -0.25f, 0.25f, 0.25f, -0.5f);
            AddQuad(scene, 0.95f, 0.95f, 0.99f, 0.99f, 0.9f);  // keeps the ground off the far edge
            auto glass = std::make_shared<vkr::Material>();
            glass->transmission = 1;
            glass->transmissionColor[1] = glass->transmissionColor[2] = 0.2f;
            occluder->material = glass;
            scene.Commit();
            const Image img = Render(renderer, scene);
            // Lit for comparison: pixel 10, just beside the shadow.
            const float r = img.At(3, 32, 0) / img.At(10, 32, 0), g = img.At(3, 32, 1) / img.At(10, 32, 1);
            Expect(std::abs(r - 0.92f) < 0.04f && std::abs(g - 0.18f) < 0.04f,
                   "red glass casts a red shadow, weakened by its interfaces' reflections");
            vkr::Renderer::Frame frame = IdentityFrame();
            frame.translucentShadows = false;
            const Image none = Render(renderer, scene, frame);
            Expect(none.At(3, 32, 1) > 0.97f * none.At(10, 32, 1), "Frame::translucentShadows off: none cast");
            frame = IdentityFrame();
            frame.translucentShadowDivisor = 4;
            const Image quarter = Render(renderer, scene, frame);
            Expect(std::abs(quarter.At(3, 32, 1) / quarter.At(10, 32, 1) - 0.18f) < 0.04f,
                   "Frame::translucentShadowDivisor 4: still tinted");
            if (device.rayTracing) {
                // Traced: the shadow rays go on through the glass, its colour.
                frame = IdentityFrame();
                frame.rtShadows = true;
                const Image traced = Render(renderer, scene, frame);
                const float tr = traced.At(3, 32, 0) / traced.At(10, 32, 0), tg = traced.At(3, 32, 1) / traced.At(10, 32, 1);
                std::printf("translucent shadows: red glass traced %.3f %.3f, maps %.3f %.3f\n", tr, tg, r, g);
                Expect(std::abs(tr - r) < 0.03f && std::abs(tg - g) < 0.03f,
                       "translucent shadows traced: red glass casts the maps' red shadow");
                frame.rtTranslucentShadows = false;
                const Image clear = Render(renderer, scene, frame);
                Expect(clear.At(3, 32, 1) > 0.97f * clear.At(10, 32, 1),
                       "Frame::rtTranslucentShadows off: traced shadows ignore the glass");
                frame = IdentityFrame();
            }
            glass->transmission = 0;
            glass->opacity = 0.5f;
            glass->ior = 1.0f;
            scene.Commit();
            const Image half = Render(renderer, scene);
            Expect(std::abs(half.At(3, 32, 1) / half.At(10, 32, 1) - 0.5f) < 0.04f,
                   "an opacity 0.5 occluder lets half the light through");
            if (device.rayTracing) {
                frame.rtShadows = true;
                const Image traced = Render(renderer, scene, frame);
                Expect(std::abs(traced.At(3, 32, 1) / traced.At(10, 32, 1) - 0.5f) < 0.03f,
                       "translucent shadows traced: an opacity 0.5 occluder lets half through");
                frame = IdentityFrame();
            }

            scene.RemoveLight(sun);
            glass->opacity = 0.25f;
            for (const float cone : {45.0f, 180.0f}) {
                // Through the occluder's centre the shadow lands around x =
                // -0.7 (pixel 12), which the camera sees past the occluder.
                vkr::Light *light = AddSphereLight(scene, 0.8f, 0, -1.6f, 0.02f, 400.0f);
                light->point = true;
                const float len = std::sqrt(0.8f * 0.8f + 1.1f * 1.1f);
                light->direction[0] = -0.8f / len;
                light->direction[2] = 1.1f / len;
                light->coneAngle = cone;
                light->castShadows = true;
                scene.Commit();
                const Image local = Render(renderer, scene);
                light->castShadows = false;
                scene.Commit();
                const Image open = Render(renderer, scene);
                Expect(std::abs(local.At(12, 32, 1) / open.At(12, 32, 1) - 0.75f) < 0.06f,
                       cone < 90 ? "translucent spot light shadow (atlas tile)"
                                 : "translucent point light shadow (cube faces)");
                scene.RemoveLight(light);
            }
        }

        {
            // Subsurface scattering: a white subsurface ground (radii 0.1,
            // 0.05, 0.02: red travels furthest) with the sun shadow test's
            // geometry, an occluder over x in [0.5, 1] shadowing [-0.5, 0].
            // Light bleeds into the shadow across its edge at pixel 32, red
            // furthest; the lit side gives it up.
            vkr::Scene scene(&device);
            AddLight(scene, 1, 0, -1)->castShadows = true;
            vkr::Mesh *ground = AddQuad(scene, -1, -1, 1, 1, 0.5f);
            AddQuad(scene, 0.5f, -1, 1, 1, -0.5f);
            AddQuad(scene, 0.95f, 0.95f, 0.99f, 0.99f, 0.9f);  // keeps the ground off the far edge
            auto skin = std::make_shared<vkr::Material>();
            std::fill(skin->baseColor, skin->baseColor + 3, 1.0f);
            skin->roughness = 1;
            skin->subsurface = 1;
            const float radii[3] = {0.1f, 0.05f, 0.02f};
            std::copy(radii, radii + 3, skin->subsurfaceRadius);
            ground->material = skin;
            scene.Commit();
            const Image on = Render(renderer, scene);
            vkr::Renderer::Frame frame = IdentityFrame();
            frame.subsurface = false;
            const Image off = Render(renderer, scene, frame);
            const float bleedR = on.At(30, 32, 0) - off.At(30, 32, 0), bleedB = on.At(30, 32, 2) - off.At(30, 32, 2);
            Expect(bleedR > 0.05f && bleedR > 2 * bleedB && bleedB >= 0,
                   "subsurface: light bleeds into the shadow, red the furthest");
            Expect(on.At(34, 32, 0) < off.At(34, 32, 0), "subsurface: the lit side gives light up");
            Expect(std::abs(on.At(8, 32, 2) - off.At(8, 32, 2)) < 0.01f, "subsurface: far from edges nothing changes");
            frame = IdentityFrame();
            frame.subsurfaceSamples = 1;
            Expect(MaxDifference(Render(renderer, scene, frame), off) < 0.01f, "Frame::subsurfaceSamples 1: no blur");
        }

        {
            // Thin-object transmission: a thin-walled subsurface quad lit
            // only from behind glows by the light through it (subsurface 0.5
            // of a white colour: half the light), and is black without.
            vkr::Scene scene(&device);
            AddLight(scene, 0, 0, 1)->castShadows = true;
            vkr::Mesh *leaf = AddQuad(scene, -1, -1, 1, 1, 0);
            auto material = std::make_shared<vkr::Material>();
            std::fill(material->baseColor, material->baseColor + 3, 1.0f);
            material->roughness = 1;
            material->subsurface = 0.5f;
            material->thinWalled = true;
            leaf->material = material;
            scene.Commit();
            const Image lit = Render(renderer, scene);
            vkr::Renderer::Frame frame = IdentityFrame();
            frame.subsurface = false;
            const Image dark = Render(renderer, scene, frame);
            Expect(std::abs(lit.At(32, 32, 1) - 0.5f) < 0.05f && dark.At(32, 32, 1) < 0.01f,
                   "thin-walled subsurface: light from behind shows through");
        }

        {
            // Holdouts: a holdout quad over the left half of a red wall is
            // black with zero alpha and still hides the wall (its depth);
            // post composites the clear colour there exactly. A translucent
            // holdout leaves the wall alone.
            vkr::Scene scene(&device);
            AddLight(scene, 0, 0, -1);
            auto red = std::make_shared<vkr::Material>();
            red->baseColor[0] = 1;
            red->baseColor[1] = red->baseColor[2] = 0;
            red->roughness = 1;
            AddQuad(scene, -1, -1, 1, 1, 0.5f)->material = red;
            vkr::Mesh *matte = AddQuad(scene, -1, -1, 0, 1, 0);
            scene.Commit();
            Expect(Render(renderer, scene).At(16, 32, 3) == 1.0f, "not yet a holdout");
            matte->holdout = true;  // changes after a frame: only the DrawInfo table
            scene.Commit();
            const Image img = Render(renderer, scene);
            Expect(img.At(16, 32, 0) == 0 && img.At(16, 32, 1) == 0 && img.At(16, 32, 3) == 0 &&
                       img.depth[32 * 64 + 16] < img.depth[32 * 64 + 48],
                   "holdout: black, zero alpha, still in front");
            Expect(img.At(48, 32, 0) > 0.9f && img.At(48, 32, 3) == 1.0f, "beside the holdout the wall as usual");
            const Image post = RenderPost(device, scene, 0.0f, false);
            Expect(post.At(16, 32, 2) == 1.0f && post.At(16, 32, 0) == 0.0f && post.At(16, 32, 3) == 1.0f,
                   "holdout after post: exactly the clear colour (opaque blue)");
            vkr::Renderer::Frame frame = IdentityFrame();
            frame.holdouts = false;
            Expect(Render(renderer, scene, frame).At(16, 32, 1) > 0.1f, "Frame::holdouts off: drawn as usual");
            auto glass = std::make_shared<vkr::Material>();
            glass->opacity = 0.5f;
            matte->material = glass;
            scene.Commit();
            const Image translucent = Render(renderer, scene);
            Expect(std::abs(translucent.At(16, 32, 0) - 0.5f * translucent.At(48, 32, 0)) < 0.01f &&
                       std::abs(translucent.At(16, 32, 3) - 0.5f) < 0.01f,
                   "translucent holdout (opacity 0.5): cuts colour and alpha by half");
            frame = IdentityFrame();
            frame.holdoutAlpha = false;
            const Image uncut = Render(renderer, scene, frame);
            Expect(std::abs(uncut.At(16, 32, 0) - uncut.At(48, 32, 0)) < 1e-3f && uncut.At(16, 32, 3) == 1.0f,
                   "Frame::holdoutAlpha off: a translucent holdout leaves what's behind it alone");
            // Opacity from a material program (the u coordinate): the matte
            // pass runs it, so the cut grows from left to right per pixel.
            auto graph = std::make_shared<vkr::Material>();
            graph->program.Emit(vkr::MaterialProgram::kTexcoord, 0);
            graph->program.Emit(vkr::MaterialProgram::kOutput, 0, {0}, vkr::MaterialProgram::kOpacity);
            matte->material = graph;
            scene.Commit();
            const Image programmed = Render(renderer, scene);
            const float leftCut = 1.0f - (8.5f / 32.0f), rightCut = 1.0f - (24.5f / 32.0f);
            Expect(std::abs(programmed.At(8, 32, 3) - leftCut) < 0.03f && std::abs(programmed.At(24, 32, 3) - rightCut) < 0.03f &&
                       std::abs(programmed.At(8, 32, 0) - leftCut * programmed.At(48, 32, 0)) < 0.03f,
                   "holdout with an opacity program: the matte cuts alpha by the program's opacity per pixel");
        }

        {
            // Cylinder light along x (half length 0.3, radius 0.05) 0.5 in
            // front of a white, fully rough wall: its diffuse light against
            // the integral over its side (the caps don't emit), in front of
            // it, beside it and past its end.
            vkr::Scene scene(&device);
            auto white = std::make_shared<vkr::Material>();
            std::fill(white->baseColor, white->baseColor + 3, 1.0f);
            white->roughness = 1;
            AddQuad(scene, -1, -1, 1, 1, 0.5f)->material = white;
            vkr::Light *tube = AddSphereLight(scene, 0, 0, 0, 0.05f, 4.0f);
            tube->type = vkr::Light::Type::Cylinder;
            tube->halfWidth[0] = 0.3f;
            scene.Commit();
            const Image img = Render(renderer, scene);
            auto expected = [](int px, int py) {
                const float px3[3] = {PixelNdc(px), PixelNdc(py), 0.5f};
                const int nx = 300, nphi = 128;
                const float r = 0.05f, h = 0.3f, dx = 2 * h / nx, dphi = 2 * 3.14159265f / nphi;
                double sum = 0;
                for (int i = 0; i < nx; ++i) {
                    for (int j = 0; j < nphi; ++j) {
                        const float phi = (j + 0.5f) * dphi, c = std::cos(phi), s = std::sin(phi);
                        const float w[3] = {-h + (i + 0.5f) * dx - px3[0], r * c - px3[1], r * s - px3[2]};
                        const float dist = std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
                        const float cosWall = -w[2] / dist, cosLight = -(c * w[1] + s * w[2]) / dist;
                        if (cosWall > 0 && cosLight > 0) {
                            sum += 4.0 * cosWall * cosLight / (dist * dist) * r * dphi * dx;
                        }
                    }
                }
                return float(sum / 3.14159265);
            };
            // (Specular at roughness 1 adds a few percent at grazing angles.)
            for (auto [px, py, tolerance] : {std::tuple{32, 32, 0.03f}, {32, 50, 0.03f}, {60, 32, 0.07f}}) {
                Expect(std::abs(img.At(px, py, 0) / expected(px, py) - 1.0f) < tolerance,
                       "cylinder light: diffuse matches the integral over its side");
            }
            vkr::Renderer::Frame frame = IdentityFrame();
            frame.clusteredLights = false;
            Expect(MaxDifference(Render(renderer, scene, frame), img) < 1e-3f,
                   "cylinder light: clusters reach as far as the light does");
            tube->point = true;  // treatAsLine: a thinner highlight, the same diffuse
            scene.Commit();
            Expect(std::abs(Render(renderer, scene).At(32, 32, 0) / img.At(32, 32, 0) - 1.0f) < 0.02f,
                   "cylinder light: treatAsLine keeps its power");
            scene.RemoveLight(tube);

            // Shadow from the atlas (cube faces): the setup of the local light
            // shadow test, with a short tube along y.
            AddQuad(scene, -0.35f, -0.05f, -0.25f, 0.05f, 0.25f)->material = white;
            tube = AddSphereLight(scene, -0.6f, 0, 0, 0.01f, 80.0f);
            tube->type = vkr::Light::Type::Cylinder;
            tube->halfWidth[0] = 0;
            tube->halfWidth[1] = 0.05f;
            tube->castShadows = true;
            scene.Commit();
            const Image shadowed = Render(renderer, scene);
            Expect(shadowed.At(32, 32, 0) < 0.3f * shadowed.At(41, 32, 0) && shadowed.At(41, 32, 0) > 0.01f,
                   "cylinder light shadow (atlas cube faces)");
        }

        {
            // Glossy reflection of a sphere light against the brute-force integral of GGX (D V F cos, f0 0.04)
            // over the light's cone, per pixel: a black quad tilted 50 degrees (NoV ~0.64), the light 1.5 away
            // along the mirror direction, once larger than the lobe (roughness 0.15) and once about as wide
            // (0.4). The peak and the highlight's total match within 5% (LightRegion; 0.475 vs 0.457 and 50.6 vs 52.9,
            // 0.183 vs 0.177 and 40.5 vs 41.3).
            const float tilt = 50.0f * 3.14159265f / 180.0f, tanTilt = std::tan(tilt);
            const float nrm[3] = {-std::sin(tilt), 0, -std::cos(tilt)};  // facing the camera
            vkr::Scene scene(&device);
            vkr::Mesh *quad = scene.AddMesh();
            const float x0 = -0.4f, x1 = 0.4f, z0 = 0.5f - x0 * tanTilt, z1 = 0.5f - x1 * tanTilt;
            quad->points = {x0, -1, z0, x1, -1, z1, x1, 1, z1, x0, -1, z0, x1, 1, z1, x0, 1, z0};
            auto glossy = std::make_shared<vkr::Material>();
            std::fill(glossy->baseColor, glossy->baseColor + 3, 0.0f);
            quad->material = glossy;
            // Mirror direction at (0, 0, 0.5) seen from the camera at z = -2.
            const float v0[3] = {0, 0, -1}, nv = -nrm[2];
            const float r0[3] = {2 * nv * nrm[0] - v0[0], 0, 2 * nv * nrm[2] - v0[2]};
            const float radius = 0.3f, L = 10.0f;
            AddSphereLight(scene, 1.5f * r0[0], 0, 0.5f + 1.5f * r0[2], radius, L);
            scene.Commit();
            const float c[3] = {1.5f * r0[0], 0, 0.5f + 1.5f * r0[2]};
            for (float roughness : {0.15f, 0.4f}) {
                glossy->roughness = roughness;
                scene.Commit();
                const Image img = Render(renderer, scene);
                const double a = double(roughness) * roughness, a2 = a * a;
                double sumGpu = 0, sumCpu = 0, peakGpu = 0, peakCpu = 0;
                for (int py = 0; py < 64; ++py) {
                    for (int px = 0; px < 64; ++px) {
                        const double p[3] = {PixelNdc(px), PixelNdc(py), 0.5 - PixelNdc(px) * tanTilt};
                        if (p[0] < x0 + 0.02 || p[0] > x1 - 0.02) {
                            continue;  // the quad's edge pixels
                        }
                        double v[3] = {-p[0], -p[1], -2 - p[2]}, w[3] = {c[0] - p[0], c[1] - p[1], c[2] - p[2]};
                        const double vl = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
                        const double dist = std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
                        for (int k = 0; k < 3; ++k) {
                            v[k] /= vl;
                            w[k] /= dist;
                        }
                        // A frame around w and the cone of the sphere, uniform in solid angle.
                        double t[3] = {w[1], -w[0], 0};  // w is never along z here
                        const double tl = std::sqrt(t[0] * t[0] + t[1] * t[1]);
                        t[0] /= tl;
                        t[1] /= tl;
                        const double b[3] = {w[1] * t[2] - w[2] * t[1], w[2] * t[0] - w[0] * t[2], w[0] * t[1] - w[1] * t[0]};
                        const double cosMax = std::sqrt(1 - double(radius) * radius / (dist * dist));
                        const double NoV = std::max(nrm[0] * v[0] + nrm[1] * v[1] + nrm[2] * v[2], 1e-4);
                        const int nu = 48, nphi = 96;
                        double sum = 0;
                        for (int i = 0; i < nu; ++i) {
                            const double u = 1 - (i + 0.5) / nu * (1 - cosMax), s = std::sqrt(1 - u * u);
                            for (int j = 0; j < nphi; ++j) {
                                const double phi = (j + 0.5) / nphi * 2 * 3.14159265358979;
                                double l[3], h[3];
                                for (int k = 0; k < 3; ++k) {
                                    l[k] = s * (std::cos(phi) * t[k] + std::sin(phi) * b[k]) + u * w[k];
                                    h[k] = l[k] + v[k];
                                }
                                const double hl = std::sqrt(h[0] * h[0] + h[1] * h[1] + h[2] * h[2]);
                                const double NoL = nrm[0] * l[0] + nrm[1] * l[1] + nrm[2] * l[2];
                                if (NoL <= 0) {
                                    continue;
                                }
                                const double NoH = (nrm[0] * h[0] + nrm[1] * h[1] + nrm[2] * h[2]) / hl;
                                const double LoH = (l[0] * h[0] + l[1] * h[1] + l[2] * h[2]) / hl;
                                const double f = NoH * NoH * (a2 - 1) + 1;
                                const double D = a2 / (3.14159265358979 * f * f);
                                const double V = 0.5 / (NoL * std::sqrt(NoV * NoV * (1 - a2) + a2) +
                                                        NoV * std::sqrt(NoL * NoL * (1 - a2) + a2));
                                const double F = 0.04 + 0.96 * std::pow(1 - LoH, 5.0);
                                sum += D * V * F * NoL;
                            }
                        }
                        const double expected = L * sum * 2 * 3.14159265358979 * (1 - cosMax) / (nu * nphi);
                        sumCpu += expected;
                        sumGpu += img.At(px, py, 0);
                        peakCpu = std::max(peakCpu, expected);
                        peakGpu = std::max(peakGpu, double(img.At(px, py, 0)));
                    }
                }
                std::printf("sphere light glossy reflection (roughness %.2f): peak %.3f vs %.3f, total %.2f vs %.2f\n",
                            roughness, peakGpu, peakCpu, sumGpu, sumCpu);
                Expect(std::abs(peakGpu / peakCpu - 1) < 0.07, "sphere light: glossy reflection's peak as the GGX integral");
                Expect(std::abs(sumGpu / sumCpu - 1) < 0.07, "sphere light: glossy reflection's energy as the GGX integral");
            }
        }

        {
            // A rect light's texture in a glossy reflection (roughness 0.1) takes the mip of the lobe's
            // footprint at the light, not of the light's size: a 1 x 1 light 0.5 in front of a black quad,
            // its texture black over one half and white over the other, reflects both halves sharply on
            // either side of the edge (0.09 from it at the light; a mip as wide as the light's size mixed
            // them to ~30%).
            vkr::Scene scene(&device);
            auto glossy = std::make_shared<vkr::Material>();
            std::fill(glossy->baseColor, glossy->baseColor + 3, 0.0f);
            glossy->roughness = 0.1f;
            AddQuad(scene, -1, -1, 1, 1, 0.5f)->material = glossy;
            vkr::Light *rect = scene.AddLight();
            rect->type = vkr::Light::Type::Rect;
            rect->halfWidth[0] = 0.5f;
            rect->halfHeight[1] = 0.5f;
            rect->direction[2] = 1;  // emits towards the quad
            std::fill(rect->radiance, rect->radiance + 3, 10.0f);
            auto halves = std::make_shared<vkr::Texture>();
            halves->width = 64;
            halves->height = 64;
            halves->pixels.assign(64 * 64 * 4, 255);
            for (int y = 0; y < 64; ++y) {
                std::fill_n(&halves->pixels[y * 64 * 4], 32 * 4, uint8_t(0));
                for (int x = 0; x < 32; ++x) {
                    halves->pixels[(y * 64 + x) * 4 + 3] = 255;
                }
            }
            rect->texture = halves;
            scene.Commit();
            const Image img = Render(renderer, scene);
            const float left = img.At(28, 32, 0), right = img.At(35, 32, 0);
            std::printf("textured rect light reflection either side of the edge: %.4f, %.4f\n", left, right);
            Expect(std::max(left, right) > 0.05f && std::min(left, right) < 0.1f * std::max(left, right),
                   "rect light texture: a glossy reflection takes the lobe's mip, sharp across the edge");
        }

        {
            // Light shapes (Frame::lightShapesVisible): spheres at (+-0.5, 0.5) and a cylinder at (0.5, -0.5), all
            // radiance 50, over a quad in front of the left half (z -0.8; the lights are at z -0.5). The shapes show
            // as their radiance where nothing is nearer; one hidden from the camera doesn't.
            vkr::Scene scene(&device);
            AddQuad(scene, -1, -1, 0, 1, -0.8f);
            vkr::Light *shown = AddSphereLight(scene, 0.5f, 0.5f, -0.5f, 0.2f, 50.0f);
            AddSphereLight(scene, -0.5f, 0.5f, -0.5f, 0.2f, 50.0f);
            vkr::Light *tube = AddSphereLight(scene, 0.5f, -0.5f, -0.5f, 0.1f, 50.0f);
            tube->type = vkr::Light::Type::Cylinder;
            tube->halfWidth[0] = 0.3f;
            scene.Commit();
            vkr::Renderer::Frame frame = IdentityFrame();
            const Image hidden = Render(renderer, scene, frame);
            frame.lightShapesVisible = true;
            const Image seen = Render(renderer, scene, frame);
            shown->cameraVisible = false;
            scene.Commit();
            const Image camera = Render(renderer, scene, frame);
            Expect(hidden.At(48, 48, 0) < 10.0f && hidden.At(48, 16, 0) < 10.0f,
                   "light shapes are off by default");
            Expect(std::abs(seen.At(48, 48, 0) - 50.0f) < 0.5f && std::abs(seen.At(48, 16, 0) - 50.0f) < 0.5f,
                   "a sphere and a cylinder light show their radiance to the camera");
            Expect(seen.At(16, 48, 0) < 10.0f, "a light shape behind a surface stays hidden");
            Expect(camera.At(48, 48, 0) < 10.0f && std::abs(camera.At(48, 16, 0) - 50.0f) < 0.5f,
                   "a light hidden from the camera still shines but isn't drawn");
        }

        {
            // Soft sun shadows: the sun shadow test's occluder (one unit above
            // the ground along z, light at 45 degrees). A 10 degree sun spreads
            // the shadow's right edge (around x = -0.75, pixel 8) over a
            // penumbra of about 1.4 * tan(5 degrees) each side, ~4 pixels; a
            // point sun or the settings off keep it hard. The tilted ground
            // stays lit beside it (the taps follow the receiver's plane).
            vkr::Scene scene(&device);
            vkr::Light *sun = AddLight(scene, 1, 0, -1);
            sun->castShadows = true;
            AddQuad(scene, -1, -1, 1, 1, 0.5f)->color[0] = 0.8f;
            AddQuad(scene, -0.25f, -0.25f, 0.25f, 0.25f, -0.5f);
            AddQuad(scene, 0.95f, 0.95f, 0.99f, 0.99f, 0.9f);
            auto penumbra = [&](float angle, bool soft, bool softSun, float *lit) {
                sun->angle = angle;
                scene.Commit();
                vkr::Renderer::Frame frame = IdentityFrame();
                frame.softShadows = soft;
                frame.softSunShadows = softSun;
                const Image img = Render(renderer, scene, frame);
                *lit = img.At(51, 32, 0);
                int partial = 0;
                for (int x = 0; x < 24; ++x) {
                    const float v = img.At(x, 32, 0) / *lit;
                    partial += v > 0.1f && v < 0.9f;
                }
                return partial;
            };
            float lit = 0, hardLit = 0, unused = 0;
            const int hard = penumbra(0.0f, true, true, &hardLit), soft = penumbra(10.0f, true, true, &lit);
            Expect(soft >= hard + 4, "soft sun shadows: a 10 degree sun has a penumbra");
            Expect(std::abs(lit / hardLit - 1.0f) < 0.02f, "soft sun shadows: no self-shadowing on a tilted receiver");
            Expect(penumbra(10.0f, true, false, &unused) <= hard + 1, "Frame::softSunShadows off: hard sun shadows");
            Expect(penumbra(10.0f, false, true, &unused) <= hard + 1, "Frame::softShadows off: hard sun shadows");

            // Jittered light shapes (Frame::shadowJitter): the converged film
            // of the same scene is the true penumbra of a 10 degree disc. At
            // ground x the occluder's right edge (x = 0.25, one unit above)
            // hides the sun's directions up to atan(0.25 - x) from the
            // vertical, so the lit part of the disc (centre 45 degrees) is
            // the circular segment beyond t = (atan(0.25 - x) - 45 deg) / 5 deg.
            // Visibility is the shadowed film over the unshadowed one.
            sun->angle = 10.0f;
            scene.Commit();
            vkr::Renderer::Frame frame = IdentityFrame();
            frame.postProcess = true;
            frame.post.autoExposure = false;
            frame.post.bloomIntensity = 0.0f;
            frame.post.tonemapper = vkr::PostProcess::Settings::Tonemapper::None;
            frame.filmFilterRadius = 0.0f;
            frame.filmSamples = 128;
            frame.finalQuality = true;
            auto film = [&](vkr::Scene &s, const vkr::Renderer::Frame &f) {
                vkr::Renderer r(device);
                Image img;
                for (int i = 0; i < 400; ++i) {
                    img = Render(r, s, f);
                    if (r.IsConverged()) {
                        break;
                    }
                }
                return img;
            };
            auto segment = [](float t) {  // part of a unit disc beyond t
                t = std::clamp(t, -1.0f, 1.0f);
                return (std::acos(t) - t * std::sqrt(1.0f - t * t)) / 3.14159265f;
            };
            sun->castShadows = false;
            scene.Commit();
            const Image open = film(scene, frame);
            sun->castShadows = true;
            scene.Commit();
            auto error = [&](const Image &img, float *partial) {
                float worst = 0.0f;
                *partial = 0.0f;
                for (int x = 0; x < 20; ++x) {
                    const float t = (std::atan(0.25f - PixelNdc(x)) - 0.7853982f) / 0.0872665f;
                    const float v = img.At(x, 32, 0) / open.At(x, 32, 0);
                    worst = std::max(worst, std::abs(v - segment(t)));
                    *partial += v > 0.05f && v < 0.95f ? 1.0f : 0.0f;
                }
                return worst;
            };
            float jitteredPartial = 0, pcssPartial = 0;
            const float jittered = error(film(scene, frame), &jitteredPartial);
            frame.shadowJitter = false;
            const float pcss = error(film(scene, frame), &pcssPartial);
            std::printf("shadow jitter: sun penumbra error %.3f over %.0f pixels (PCSS %.3f over %.0f)\n", jittered,
                        jitteredPartial, pcss, pcssPartial);
            Expect(jittered < 0.03f && jitteredPartial >= 8, "shadow jitter: the film converges to the sun's penumbra");
            if (device.rayTracing) {
                // Traced instead (Frame::finalShadows, without rtShadows):
                // one ray per film sample, a new point of the disc each
                // (the sample dimensions), converges to the same penumbra.
                frame.finalShadows = vkr::Renderer::Frame::FinalShadows::Raytraced;
                float tracedPartial = 0;
                const float traced = error(film(scene, frame), &tracedPartial);
                std::printf("final shadows: traced sun penumbra error %.3f over %.0f pixels\n", traced, tracedPartial);
                Expect(traced < 0.03f && tracedPartial >= 8,
                       "final shadows raytraced: the film converges to the sun's penumbra");
                frame.finalShadows = vkr::Renderer::Frame::FinalShadows::Maps;
            }
            // Off the film (TAA frames, film off) and for a point sun nothing changes.
            frame.filmAccumulation = false;
            const Image taaOff = film(scene, frame);
            frame.shadowJitter = true;
            Expect(MaxDifference(film(scene, frame), taaOff) == 0.0f, "shadow jitter: TAA frames keep PCSS");
            // The film a change handed back to TAA (usdview's still camera) is
            // interactive too: PCSS, jitter on or off.
            frame.filmAccumulation = true;
            frame.finalQuality = false;
            auto interactive = [&](const vkr::Renderer::Frame &f) {
                vkr::Renderer r(device);
                Render(r, scene, f);
                vkr::Renderer::Frame changed = f;
                changed.sceneVersion = 7;
                Image img;
                for (int i = 0; i < 400; ++i) {
                    img = Render(r, scene, changed);
                    if (r.IsConverged()) {
                        break;
                    }
                }
                return img;
            };
            const Image interactiveJitter = interactive(frame);
            frame.shadowJitter = false;
            const Image interactivePcss = interactive(frame);
            // (Measured 0.004 apart, not bit for bit; a jittered film's
            // penumbra is up to about 0.1 away from PCSS.)
            Expect(MaxDifference(interactiveJitter, interactivePcss) < 0.01f,
                   "shadow jitter: only in final quality (an interactive film keeps PCSS)");
            if (device.rayTracing) {
                // (Its first frame is a fresh, final one, which traces.)
                frame.finalShadows = vkr::Renderer::Frame::FinalShadows::Raytraced;
                vkr::Renderer r(device);
                Render(r, scene, frame);
                vkr::Renderer::Frame changed = frame;
                changed.sceneVersion = 7;
                for (int i = 0; i < 400 && !r.IsConverged(); ++i) {
                    Render(r, scene, changed);
                }
                Expect(r.GetStats().accelerationStructureBytes == 0,
                       "final shadows raytraced: an interactive film keeps the maps");
                frame.finalShadows = vkr::Renderer::Frame::FinalShadows::Maps;
            }
            frame.shadowJitter = true;
            frame.finalQuality = true;
            sun->angle = 0.0f;
            scene.Commit();
            const Image pointOn = film(scene, frame);
            frame.shadowJitter = false;
            Expect(MaxDifference(film(scene, frame), pointOn) == 0.0f, "shadow jitter: a point sun is unchanged");

            // A sphere light of radius 0.1 at (-0.3, 0, -0.5) behind a
            // half-plane occluder (x < 0 at z = 0): from ground point g its
            // disc is cut by the plane through g and the occluder's edge
            // (the y axis); the lit part is the segment beyond the centre's
            // angular distance from that plane, over its angular radius.
            vkr::Scene local(&device);
            AddQuad(local, -1, -1, 1, 1, 0.5f);
            AddQuad(local, -4, -4, 0, 4, 0.0f);
            vkr::Light *sphere = AddSphereLight(local, -0.3f, 0, -0.5f, 0.1f, 50.0f);
            sphere->castShadows = false;
            local.Commit();
            frame = IdentityFrame();
            frame.postProcess = true;
            frame.post.autoExposure = false;
            frame.post.bloomIntensity = 0.0f;
            frame.post.tonemapper = vkr::PostProcess::Settings::Tonemapper::None;
            frame.filmFilterRadius = 0.0f;
            frame.filmSamples = 256;
            frame.finalQuality = true;
            const Image localOpen = film(local, frame);
            sphere->castShadows = true;
            local.Commit();
            auto localError = [&](const Image &img, float *partial) {
                float worst = 0.0f;
                *partial = 0.0f;
                for (int x = 34; x < 52; ++x) {
                    const float gx = PixelNdc(x), len = std::sqrt(0.25f + gx * gx);
                    const float s = (-0.15f + 0.5f * gx) / len;  // centre to the plane, lit side positive
                    const float d = std::sqrt((gx + 0.3f) * (gx + 0.3f) + 1.0f);
                    const float lit = segment(-std::asin(s / d) / std::asin(0.1f / d));
                    const float v = img.At(x, 32, 0) / localOpen.At(x, 32, 0);
                    worst = std::max(worst, std::abs(v - lit));
                    *partial += v > 0.05f && v < 0.95f ? 1.0f : 0.0f;
                }
                return worst;
            };
            const float localJittered = localError(film(local, frame), &jitteredPartial);
            frame.shadowJitter = false;
            const float localPcss = localError(film(local, frame), &pcssPartial);
            std::printf("shadow jitter: sphere light penumbra error %.3f over %.0f pixels (PCSS %.3f over %.0f)\n",
                        localJittered, jitteredPartial, localPcss, pcssPartial);
            Expect(localJittered < 0.03f && jitteredPartial >= 4,
                   "shadow jitter: the film converges to a sphere light's penumbra");
            if (device.rayTracing) {
                frame.finalShadows = vkr::Renderer::Frame::FinalShadows::Raytraced;
                float tracedPartial = 0;
                const float traced = localError(film(local, frame), &tracedPartial);
                std::printf("final shadows: traced sphere light penumbra error %.3f over %.0f pixels\n", traced,
                            tracedPartial);
                Expect(traced < 0.03f && tracedPartial >= 4,
                       "final shadows raytraced: the film converges to a sphere light's penumbra");
                frame.finalShadows = vkr::Renderer::Frame::FinalShadows::Maps;
            }
            frame.filmAccumulation = false;
            const Image localTaa = film(local, frame);
            frame.shadowJitter = true;
            Expect(MaxDifference(film(local, frame), localTaa) == 0.0f, "shadow jitter: TAA frames keep local PCSS");

            // A cylinder light along y (radius 0.1) in the same place: the
            // plane through the occluder's edge contains its axis, so it
            // cuts the strip it shows (2r wide, emission uniform across it)
            // at one place, and the lit part is linear in that place.
            vkr::Scene tubeScene(&device);
            AddQuad(tubeScene, -1, -1, 1, 1, 0.5f);
            AddQuad(tubeScene, -4, -4, 0, 4, 0.0f);
            vkr::Light *tube = AddSphereLight(tubeScene, -0.3f, 0, -0.5f, 0.1f, 50.0f);
            tube->type = vkr::Light::Type::Cylinder;
            tube->halfWidth[0] = 0.0f;
            tube->halfWidth[1] = 0.5f;
            tube->castShadows = false;
            tubeScene.Commit();
            frame.filmAccumulation = true;
            frame.shadowJitter = true;
            const Image tubeOpen = film(tubeScene, frame);
            tube->castShadows = true;
            tubeScene.Commit();
            const Image tubeImg = film(tubeScene, frame);
            float tubeWorst = 0.0f, tubePartial = 0.0f;
            for (int x = 34; x < 52; ++x) {
                const float gx = PixelNdc(x), len = std::sqrt(0.25f + gx * gx);
                const float s = (-0.15f + 0.5f * gx) / len;
                const float d = std::sqrt((gx + 0.3f) * (gx + 0.3f) + 1.0f);
                const float lit = std::clamp(0.5f + 0.5f * std::asin(s / d) / std::asin(0.1f / d), 0.0f, 1.0f);
                const float v = tubeImg.At(x, 32, 0) / tubeOpen.At(x, 32, 0);
                tubeWorst = std::max(tubeWorst, std::abs(v - lit));
                tubePartial += v > 0.05f && v < 0.95f ? 1.0f : 0.0f;
            }
            std::printf("shadow jitter: cylinder light penumbra error %.3f over %.0f pixels\n", tubeWorst,
                        tubePartial);
            Expect(tubeWorst < 0.05f && tubePartial >= 4,
                   "shadow jitter: the film converges to a cylinder light's penumbra across its axis");
            // A sphere light of radius 0.05 at z = -0.5 with a ceiling just
            // behind it (z = -0.42, 1.6 radii from the centre) that has a
            // hole at x 0.7 .. 0.9. The floor seen through the hole is dark:
            // the ceiling blocks every ray from the ball. (Clearing twice the
            // radius for the jittered sample point dropped the ceiling.)
            vkr::Scene leak(&device);
            AddQuad(leak, -4, -4, 4, 4, 0.5f);
            AddQuad(leak, -4, -4, 0.7f, 4, -0.42f);
            AddQuad(leak, 0.9f, -4, 4, 4, -0.42f);
            vkr::Light *bulb = AddSphereLight(leak, 0, 0, -0.5f, 0.05f, 50.0f);
            bulb->castShadows = false;
            leak.Commit();
            frame.filmAccumulation = true;
            frame.shadowJitter = true;
            const Image leakOpen = film(leak, frame);
            bulb->castShadows = true;
            leak.Commit();
            const Image leakOn = film(leak, frame);
            float worstLeak = 0.0f;
            for (int x = 56; x < 59; ++x) {
                worstLeak = std::max(worstLeak, leakOn.At(x, 32, 0) / leakOpen.At(x, 32, 0));
            }
            std::printf("shadow jitter: floor behind a ceiling near a bulb lit to %.3f\n", worstLeak);
            Expect(leakOpen.At(57, 32, 0) > 0.01f && worstLeak < 0.02f,
                   "shadow jitter: a caster just outside a sphere light still shadows");
            // The same with a tube of radius 0.05 along y in the bulb's place
            // (a fluorescent tube just under a shelf). Clearing its radius
            // past the sample point dropped the ceiling.
            bulb->type = vkr::Light::Type::Cylinder;
            bulb->halfWidth[0] = 0.0f;
            bulb->halfWidth[1] = 0.5f;
            bulb->castShadows = false;
            leak.Commit();
            const Image tubeLeakOpen = film(leak, frame);
            bulb->castShadows = true;
            leak.Commit();
            const Image tubeLeakOn = film(leak, frame);
            float worstTubeLeak = 0.0f;
            for (int x = 56; x < 59; ++x) {
                worstTubeLeak = std::max(worstTubeLeak, tubeLeakOn.At(x, 32, 0) / tubeLeakOpen.At(x, 32, 0));
            }
            std::printf("shadow jitter: floor behind a ceiling near a tube lit to %.3f\n", worstTubeLeak);
            Expect(tubeLeakOpen.At(57, 32, 0) > 0.01f && worstTubeLeak < 0.02f,
                   "shadow jitter: a caster just outside a cylinder light still shadows");
        }

        {
            // Lens jitter (Frame::dofJitter), through PerspectiveFrame (90
            // degrees, 64 pixels): f = 1, sensor 2, f/1 focused at 2. A 2 x 2
            // pixel emitter 4 away spreads over the thin lens's circle of
            // confusion, 0.5 f^2 / (N (S - f)) * 32 * |D - S| / D = 8 pixels
            // in radius: its light keeps its sum and its RMS radius is
            // sqrt(8^2 / 2 + 2^2 / 6). An emitter's edge in the focus plane
            // stays as sharp as without depth of field. Off the film the
            // image is the post DOF's, bit for bit.
            vkr::Scene scene(&device);
            AddEmitter(scene, -0.125f, -0.125f, 0.125f, 0.125f, -4.0f, 1.0f);
            AddEmitter(scene, -1.0f, 0.75f, 0.25f, 1.5f, -2.0f, 1.0f);  // lower edge at row 44
            scene.Commit();
            vkr::Renderer::Frame frame = PerspectiveFrame();
            std::fill(frame.clearColor, frame.clearColor + 4, 0.0f);
            frame.postProcess = true;
            frame.post.autoExposure = false;
            frame.post.bloomIntensity = 0.0f;
            frame.post.tonemapper = vkr::PostProcess::Settings::Tonemapper::None;
            frame.filmFilterRadius = 0.0f;
            frame.filmSamples = 256;
            frame.finalQuality = true;
            frame.focalLength = 1.0f;
            frame.sensorWidth = 2.0f;
            frame.fStop = 1.0f;
            frame.focusDistance = 2.0f;
            auto film = [&](const vkr::Renderer::Frame &f) {
                vkr::Renderer r(device);
                Image img;
                for (int i = 0; i < 400; ++i) {
                    img = Render(r, scene, f);
                    if (r.IsConverged()) {
                        break;
                    }
                }
                return img;
            };
            auto spot = [](const Image &img, float *rms) {  // the far emitter: rows below 42
                double sum = 0, moment = 0;
                for (int y = 0; y < 42; ++y) {
                    for (int x = 0; x < 64; ++x) {
                        const double v = img.At(x, y, 1), dx = x + 0.5 - 32.0, dy = y + 0.5 - 32.0;
                        sum += v;
                        moment += v * (dx * dx + dy * dy);
                    }
                }
                *rms = float(std::sqrt(moment / std::max(sum, 1e-9)));
                return float(sum);
            };
            const Image jittered = film(frame);
            frame.depthOfField = false;
            const Image sharp = film(frame);
            float rms = 0, sharpRms = 0;
            const float energy = spot(jittered, &rms), sharpEnergy = spot(sharp, &sharpRms);
            const float expected = std::sqrt(32.0f + 4.0f / 6.0f);
            float edge = 0;
            for (int y = 40; y < 48; ++y) {
                edge = std::max(edge, std::abs(jittered.At(24, y, 1) - sharp.At(24, y, 1)));
            }
            std::printf("dof jitter: spot sum %.2f (sharp %.2f), RMS radius %.2f px (thin lens %.2f), focus edge %.3f\n",
                        energy, sharpEnergy, rms, expected, edge);
            Expect(std::abs(energy / sharpEnergy - 1.0f) < 0.05f && std::abs(rms / expected - 1.0f) < 0.08f,
                   "dof jitter: the film converges to the thin lens's circle of confusion");
            Expect(edge < 0.1f, "dof jitter: the focus plane stays sharp");
            frame.depthOfField = true;
            frame.filmAccumulation = false;
            const Image post = film(frame);
            frame.dofJitter = false;
            Expect(MaxDifference(film(frame), post) == 0.0f, "dof jitter: TAA frames keep the post DOF");
            // Residual DOF (Frame::dofResidualBlur, on above): a post DOF of
            // 1/sqrt(samples) the circle between the lens samples. At 4
            // samples it is nearer the converged spot (256 samples) than
            // lens jitter alone; at 64 it converges to what jitter alone does.
            frame.dofJitter = true;
            frame.filmAccumulation = true;
            auto error = [](const Image &a, const Image &b) {
                double e = 0;
                for (int y = 0; y < 42; ++y) {
                    for (int x = 0; x < 64; ++x) {
                        const double d = a.At(x, y, 1) - b.At(x, y, 1);
                        e += d * d;
                    }
                }
                return float(std::sqrt(e / (42 * 64)));
            };
            frame.filmSamples = 4;
            const Image residual4 = film(frame);
            frame.filmSamples = 64;
            const Image residual64 = film(frame);
            frame.dofResidualBlur = false;
            const Image jitter64 = film(frame);
            frame.filmSamples = 4;
            const Image jitter4 = film(frame);
            float rms64 = 0, jitterRms64 = 0;
            const float sum64 = spot(residual64, &rms64), jitterSum64 = spot(jitter64, &jitterRms64);
            std::printf("dof residual: 64 samples spot sum %.2f (jitter alone %.2f), RMS radius %.2f px (%.2f)\n",
                        sum64, jitterSum64, rms64, jitterRms64);
            std::printf("dof residual: 4 samples RMSE %.4f (jitter alone %.4f), 64 samples %.4f (%.4f), "
                        "residual vs jitter at 64 %.4f, RMS radius %.2f px\n",
                        error(residual4, jittered), error(jitter4, jittered), error(residual64, jittered),
                        error(jitter64, jittered), error(residual64, jitter64), rms64);
            Expect(error(residual4, jittered) < 0.8f * error(jitter4, jittered),
                   "dof residual: fills the gaps between 4 lens samples");
            Expect(error(residual64, jitter64) < 0.5f * error(jitter4, jittered) &&
                       std::abs(rms64 / expected - 1.0f) < 0.08f && std::abs(sum64 / jitterSum64 - 1.0f) < 0.1f,
                   "dof residual: 64 samples converge to lens jitter's image");
        }

        {
            // Film ids: primId and depth come from the film sample nearest
            // the pixel centre, not the last one. A quad in front of a
            // backdrop ends a quarter pixel outside the centres of columns
            // 20 and 40, so every jittered sample farther out than that
            // (the last one, 0.49 pixels off, on one side) misses it there.
            vkr::Scene scene(&device);
            const float px = 2.0f / 64.0f;
            AddQuad(scene, -1, -1, 1, 1, 0.5f)->primId = 2;
            AddQuad(scene, -1.0f + 20.5f * px - 0.25f * px, -0.8f, -1.0f + 40.5f * px + 0.25f * px, 0.8f, 0.0f)->primId =
                1;
            scene.Commit();
            vkr::Renderer::Frame frame = IdentityFrame();
            frame.postProcess = true;
            frame.post.autoExposure = false;
            frame.finalQuality = true;
            frame.filmSamples = 64;
            vkr::Renderer r(device);
            Image img;
            for (int i = 0; i < 200; ++i) {
                img = Render(r, scene, frame);
                if (r.IsConverged()) {
                    break;
                }
            }
            bool ids = true;
            for (int y = 16; y < 48; ++y) {
                ids = ids && img.Prim(20, y) == 1 && img.Prim(40, y) == 1 && img.Prim(19, y) == 2 &&
                      img.Prim(41, y) == 2 && img.depth[y * 64 + 20] == img.depth[y * 64 + 30] &&
                      img.depth[y * 64 + 40] == img.depth[y * 64 + 30];
            }
            std::printf("film ids: columns 19..21 %d %d %d, 39..41 %d %d %d after %u samples\n", img.Prim(19, 32),
                        img.Prim(20, 32), img.Prim(21, 32), img.Prim(39, 32), img.Prim(40, 32), img.Prim(41, 32),
                        r.GetStats().taaSamples);
            Expect(ids, "film ids: primId and depth of the sample nearest the pixel centre");
        }

        {
            // Volumetric fog, through PerspectiveFrame (camera at the origin
            // looking down -z): an emitter 4 away behind uniform fog of
            // density 0.2 keeps exp(-0.8) of its light; a black backdrop
            // shows the light the fog scatters from a sun behind the camera
            // (isotropic phase, so E / 4 pi (1 - exp(-0.8)) along the axis);
            // half of the view lies in the shadow of a horizontal plane
            // above it, where the fog stays dark.
            vkr::Scene scene(&device);
            vkr::Mesh *backdrop = AddEmitter(scene, -10, -10, 10, 10, -4, 1.0f);
            std::fill(backdrop->material->specular, backdrop->material->specular + 3, 0.0f);  // no highlight
            vkr::Renderer::Frame frame = PerspectiveFrame();
            frame.fog = true;
            frame.fogDensity = 0.2f;
            frame.fogAnisotropy = 0.0f;
            scene.Commit();
            const float transmitted = Render(renderer, scene, frame).At(32, 32, 0);
            Expect(std::abs(transmitted - std::exp(-0.8f)) < 0.01f, "fog: transmittance exp(-density * distance)");
            Expect(renderer.GetStats().passMs.size() > 0 &&
                       std::any_of(renderer.GetStats().passMs.begin(), renderer.GetStats().passMs.end(),
                                   [](const auto &p) { return std::strcmp(p.first, "fog") == 0; }),
                   "fog: its own pass");
            frame.fog = false;
            Expect(Render(renderer, scene, frame).At(32, 32, 0) == 1.0f &&
                       std::none_of(renderer.GetStats().passMs.begin(), renderer.GetStats().passMs.end(),
                                    [](const auto &p) { return std::strcmp(p.first, "fog") == 0; }),
                   "Frame::fog off: no fog, no pass");
            frame.fog = true;

            backdrop->material->emissive[0] = backdrop->material->emissive[1] = backdrop->material->emissive[2] = 0;
            vkr::Light *sun = AddLight(scene, 0, 0, 1);
            std::fill(sun->irradiance, sun->irradiance + 3, 4.0f * 3.14159265f);  // E / 4 pi = 1
            scene.Commit();
            const float scattered = Render(renderer, scene, frame).At(32, 32, 0);
            Expect(std::abs(scattered - (1.0f - std::exp(-0.8f))) < 0.02f, "fog: in-scattered sunlight");
            {
                // Under the film each sample jitters the froxels anew, with
                // no history; the average is the same medium.
                vkr::Renderer filmRenderer(device);
                vkr::Renderer::Frame film = frame;
                film.postProcess = true;
                film.post.autoExposure = false;
                film.post.bloomIntensity = 0.0f;
                film.post.tonemapper = vkr::PostProcess::Settings::Tonemapper::None;
                film.filmSamples = 16;
                Image first, img;
                int n = 0;
                do {
                    img = Render(filmRenderer, scene, film);
                    first = n++ == 1 ? img : first;
                } while (!filmRenderer.IsConverged() && n < 100);
                Expect(std::abs(img.At(32, 32, 0) - (1.0f - std::exp(-0.8f))) < 0.02f && MaxDifference(first, img) > 0.0f,
                       "fog under the film: froxels jittered per sample average to the medium");
            }

            // Light from above (+y), a plane at y = 0.5 over x < 0.
            sun->direction[1] = 1;
            sun->direction[2] = 0;
            sun->castShadows = true;
            vkr::Mesh *roof = scene.AddMesh();
            roof->points = {-10, 0.5f, -0.05f, 0, 0.5f, -0.05f, 0, 0.5f, -10, -10, 0.5f, -0.05f, 0, 0.5f, -10, -10, 0.5f, -10};
            roof->material = backdrop->material;
            scene.Commit();
            Image img = Render(renderer, scene, frame);
            Expect(img.At(12, 32, 0) < 0.3f * img.At(52, 32, 0) && img.At(52, 32, 0) > 0.2f,
                   "fog: the sun's shadow volumes stay dark");

            // Volumes shadow the fog from the sun (through the sun's tint
            // map, VolumePass::Settings::shadows, as surfaces): a flat dense cloud
            // at y 1.5 over x < 0 instead of the roof. Fresh renderers: no
            // fog history.
            roof->visible = false;
            vkr::Volume *cloud = scene.AddVolume();
            cloud->fields["density"] = BallGrid(12.0f, 20.0f);
            cloud->transforms = {16, 0, 0, 0, 0, 0.5f, 0, 0, 0, 0, 16, 0, -6, 1.5f, -2, 1};
            scene.Commit();
            const Image clouded = Render(*std::make_unique<vkr::Renderer>(device), scene, frame);
            frame.volume.shadows = false;
            const Image unclouded = Render(*std::make_unique<vkr::Renderer>(device), scene, frame);
            frame.volume.shadows = true;
            cloud->visible = false;
            scene.Commit();
            const Image clear = Render(*std::make_unique<vkr::Renderer>(device), scene, frame);
            std::printf("fog under a volume %.3f / %.3f (off) / %.3f (no volume), beside it %.3f / %.3f\n",
                        clouded.At(12, 32, 0), unclouded.At(12, 32, 0), clear.At(12, 32, 0), clouded.At(52, 32, 0),
                        unclouded.At(52, 32, 0));
            Expect(clouded.At(12, 32, 0) < 0.6f * unclouded.At(12, 32, 0) &&
                       std::abs(clouded.At(52, 32, 0) - unclouded.At(52, 32, 0)) < 0.02f * unclouded.At(52, 32, 0),
                   "fog: volumes shadow it from the sun");
            Expect(std::abs(unclouded.At(12, 32, 0) - clear.At(12, 32, 0)) < 0.02f * clear.At(12, 32, 0),
                   "fog: volume shadows off, as without the volume");

            // Red glass (transmission colour 1, 0.2, 0.2) as the roof, no
            // volumes: the fog under it is lit red through the sun's tint
            // map whenever translucent shadows are on.
            auto redGlass = std::make_shared<vkr::Material>();
            redGlass->transmission = 1;
            redGlass->transmissionColor[1] = redGlass->transmissionColor[2] = 0.2f;
            roof->material = redGlass;
            roof->visible = true;
            scene.Commit();
            const Image redFog = Render(*std::make_unique<vkr::Renderer>(device), scene, frame);
            frame.translucentShadows = false;
            const Image plainFog = Render(*std::make_unique<vkr::Renderer>(device), scene, frame);
            frame.translucentShadows = true;
            roof->material = backdrop->material;
            std::printf("fog under red glass %.3f %.3f, translucent shadows off %.3f %.3f\n", redFog.At(12, 32, 0),
                        redFog.At(12, 32, 1), plainFog.At(12, 32, 0), plainFog.At(12, 32, 1));
            Expect(redFog.At(12, 32, 0) > 0.1f && redFog.At(12, 32, 1) < 0.4f * redFog.At(12, 32, 0) &&
                       plainFog.At(12, 32, 1) >= 0.9f * plainFog.At(12, 32, 0),
                   "fog: glass between it and the sun tints it, without volumes too");

            // Height fog: thinner upwards (y), so less light scatters at the
            // top of the view than at the bottom.
            roof->visible = false;
            frame.fogHeightFalloff = 2.0f;
            scene.Commit();
            img = Render(renderer, scene, frame);
            Expect(img.At(32, 60, 0) < 0.7f * img.At(32, 4, 0), "fog: height falloff");
        }

        {
            // Reflection probe: a mirror facing the camera (IdentityFrame,
            // shaded as seen from z = -2) reflects an emitter behind the
            // camera, out of view, at x -1..-0.2. The probe at the bounds'
            // centre sees it. The mirror's ray at x reaches it at 2.4 x, so
            // it shows at x = -0.27 (pixel 23) but not at -0.61 (pixel 12),
            // where the probe's own ray in that direction would meet it:
            // parallax correction.
            vkr::Scene scene(&device);
            vkr::Mesh *mirror = AddQuad(scene, -1, -1, 1, 1, 0.5f);
            auto chrome = std::make_shared<vkr::Material>();
            std::fill(chrome->baseColor, chrome->baseColor + 3, 1.0f);
            chrome->metallic = 1;
            chrome->roughness = 0;
            mirror->material = chrome;
            AddEmitter(scene, -1, -1, -0.2f, 1, -3, 2.0f);
            vkr::Dome *dome = scene.AddDome();  // black, but image-based lighting is on
            std::fill(dome->radiance, dome->radiance + 3, 0.0f);
            scene.Commit();
            vkr::Renderer::Frame frame = IdentityFrame();
            const Image off = Render(renderer, scene, frame);
            frame.reflectionProbe = true;
            const Image on = Render(renderer, scene, frame);
            Expect(off.At(23, 32, 0) < 0.01f, "no probe: the mirror shows the black dome");
            Expect(on.At(23, 32, 0) > 1.6f && on.At(48, 32, 0) < 0.1f && renderer.IsConverged(),
                   "reflection probe: the emitter behind the camera");
            Expect(on.At(12, 32, 0) < 0.1f, "reflection probe: parallax corrected");
            Expect(renderer.GetStats().passMs.size() > 0 &&
                       std::strcmp(renderer.GetStats().passMs.front().first, "probeCapture") == 0,
                   "reflection probe: its capture is a pass");
            const Image again = Render(renderer, scene, frame);
            Expect(MaxDifference(again, on) < 1e-3f && (renderer.GetStats().passMs.empty() ||
                                                        std::strcmp(renderer.GetStats().passMs.front().first,
                                                                    "probeCapture") != 0),
                   "reflection probe: captured once for a still scene");

            // Authored probes (primvars:eldr:reflectionProbe): a box over
            // the mirror's left half reaching back past the emitter. It is
            // never drawn; inside it the probe at its centre reflects the
            // emitter, outside it the (black) dome shows.
            vkr::Mesh *box = scene.AddMesh();
            box->points = {-1.3f, -1.2f, -3.2f, 0, 1.2f, 1.2f, 0, -1.2f, 1.2f};
            box->primId = 9;
            box->reflectionProbe = true;
            mirror->primId = 3;
            scene.Commit();
            Image authored = Render(renderer, scene, frame);
            Expect(authored.At(23, 32, 0) > 1.6f && authored.At(40, 32, 0) < 0.01f,
                   "authored probe: only inside its box");
            Expect(authored.Prim(16, 20) != 9 && authored.Prim(16, 20) == mirror->primId,
                   "authored probe: its mesh isn't drawn");
            frame.authoredProbes = false;  // the automatic probe again: the whole mirror
            authored = Render(renderer, scene, frame);
            Expect(authored.At(23, 32, 0) > 1.6f && authored.At(40, 32, 0) < 0.1f && authored.Prim(16, 20) != 9,
                   "Frame::authoredProbes off: the automatic probe");
            frame.authoredProbes = true;

            // Probe normalization: with SSGI the mirror's local ambient light
            // is known, and none reaches it (black dome, nothing lit on
            // screen), while its probe saw the emitter: its reflection fades.
            frame.ssgi = true;
            frame.probeNormalize = false;
            const float unnormalized = Render(renderer, scene, frame).At(23, 32, 0);
            frame.probeNormalize = true;
            const float normalized = Render(renderer, scene, frame).At(23, 32, 0);
            Expect(unnormalized > 1.6f && normalized < 0.1f * unnormalized,
                   "probe normalization: no local light, no probe light");
            frame.ssgi = false;
            Expect(Render(renderer, scene, frame).At(23, 32, 0) > 1.6f, "probe normalization needs GI or SSGI");
            frame.imageBasedLighting = false;
            Expect(Render(renderer, scene, frame).At(23, 32, 0) < 0.01f,
                   "Frame::imageBasedLighting off: no probe either");
        }

        {
            // Planar reflections through PerspectiveFrame: a chrome floor at
            // y = -0.5 reflects an emitter standing behind it (y 0.6..2,
            // z = -4), not a black panel hidden under it (clipped at the
            // mirror). The view ray at pixel row 16 meets the floor at depth
            // 1 and, reflected, the emitter at y = 1.
            vkr::Scene scene(&device);
            vkr::Mesh *floor = scene.AddMesh();
            floor->points = {-3, -0.5f, -0.1f, 3, -0.5f, -0.1f, 3, -0.5f, -10, -3, -0.5f, -0.1f, 3, -0.5f, -10,
                             -3, -0.5f, -10};
            auto chrome = std::make_shared<vkr::Material>();
            std::fill(chrome->baseColor, chrome->baseColor + 3, 1.0f);
            chrome->metallic = 1;
            chrome->roughness = 0.1f;
            floor->material = chrome;
            AddEmitter(scene, -2, 0.6f, 2, 2, -4, 1.0f);
            // Black, just under the floor: mirrored, it would stand in front
            // of the floor where row 16 looks.
            AddEmitter(scene, -2, -1.2f, 2, -0.55f, -0.8f, 0.0f);
            scene.Commit();
            vkr::Renderer::Frame frame = PerspectiveFrame();
            const float off = Render(renderer, scene, frame).At(32, 16, 0);
            frame.planarReflections = true;
            const Image on = Render(renderer, scene, frame);
            Expect(off < 0.01f && on.At(32, 16, 0) > 0.8f, "planar reflection of what stands on the mirror");
            Expect(on.At(32, 26, 0) < 0.05f, "planar reflection: the floor's far part reflects the empty sky");
            Expect(renderer.GetStats().passMs.size() > 0 &&
                       std::strcmp(renderer.GetStats().passMs.front().first, "planarReflection") == 0,
                   "planar reflection: its render is a pass");
            chrome->roughness = 0.5f;  // no longer glossy: no mirror
            scene.Commit();
            Expect(Render(renderer, scene, frame).At(32, 16, 0) < 0.01f, "planar reflection: only glossy floors");
            floor->planarReflection = true;  // primvars:eldr:planarReflection, at roughness 0.5
            scene.Commit();
            const float flagged = Render(renderer, scene, frame).At(32, 16, 0);
            Expect(flagged > 0.1f, "planar reflection: a flagged mirror");
            // A second, larger flagged mirror far behind (z = -9): it comes
            // first, the floor is the second render and image.
            vkr::Mesh *wall = scene.AddMesh();
            wall->points = {-15, -15, -9, 15, -15, -9, 15, 15, -9, -15, -15, -9, 15, 15, -9, -15, 15, -9};
            wall->material = chrome;
            wall->planarReflection = true;
            scene.Commit();
            Expect(std::abs(Render(renderer, scene, frame).At(32, 16, 0) - flagged) < 0.02f * flagged,
                   "planar reflections: two mirrors, each its own");
            frame.planarCount = 1;
            Expect(Render(renderer, scene, frame).At(32, 16, 0) < 0.01f, "planar reflections: only the largest");
        }

        {
            // Volumes: a ball of density 2 (radius 12 voxels, 0.375) at the
            // origin, seen along +z. Unlit, the blue background shows
            // through by the transmittance, exp(-2 * 0.75) in the middle;
            // the rest of the image is untouched.
            vkr::Scene scene(&device);
            vkr::Volume *ball = scene.AddVolume();
            ball->fields["density"] = BallGrid(12.0f, 2.0f);
            scene.Commit();
            vkr::Renderer renderer(device);
            vkr::Renderer::Frame frame = IdentityFrame();
            Image img = Render(renderer, scene, frame);
            Expect(std::abs(img.At(32, 32, 2) - std::exp(-1.5f)) < 0.03f && img.At(32, 32, 0) == 0.0f,
                   "a volume's transmittance follows Beer-Lambert");
            Expect(img.At(60, 32, 2) == 1.0f && img.At(32, 60, 2) == 1.0f, "a volume leaves the rest alone");
            frame.volumes = false;
            Expect(Render(renderer, scene, frame).At(32, 32, 2) == 1.0f, "volumes can be switched off");
            frame.volumes = true;

            // Opaque surfaces: an emitter in front (left half) hides the
            // ball, one behind (right half) is seen through it.
            AddEmitter(scene, -1, -1, 0, 1, -0.5f, 0.5f);
            AddEmitter(scene, 0, -1, 1, 1, 0.5f, 1.0f);
            scene.Commit();
            img = Render(renderer, scene, frame);
            Expect(img.At(28, 32, 0) == 0.5f, "a volume behind a surface is hidden");
            Expect(img.At(36, 32, 0) > 0.1f && img.At(36, 32, 0) < 0.35f, "a surface behind a volume shows through");
            frame.depthPrepass = false;
            const Image noPrepass = Render(renderer, scene, frame);
            Expect(noPrepass.At(28, 32, 0) == 0.5f && noPrepass.At(36, 32, 0) == img.At(36, 32, 0),
                   "volumes stop at the main pass's depth without the prepass too");
            frame.depthPrepass = true;

            // A light from +x: the ball's lit side scatters more than the side
            // its own density shadows; without the shadow march they match.
            vkr::Scene lit(&device);
            lit.AddVolume()->fields = ball->fields;
            AddLight(lit, 1, 0, 0)->castShadows = true;
            lit.Commit();
            img = Render(renderer, lit, frame);
            const float near = img.At(40, 32, 0), far = img.At(23, 32, 0);  // x = +-0.27
            Expect(near > 0.02f && near > 2.0f * far, "a volume shadows itself");
            frame.volume.shadowSteps = 0;
            img = Render(renderer, lit, frame);
            Expect(std::abs(img.At(40, 32, 0) - img.At(23, 32, 0)) < 0.05f * img.At(40, 32, 0),
                   "without shadow steps the ball is lit evenly");
            frame.volume.shadowSteps = vkr::VolumePass::Settings().shadowSteps;

            // Temperature 0.3 (normalized: 1500 K) glows red-orange.
            ball->fields["temperature"] = BallGrid(12.0f, 0.3f);
            scene.Commit();
            img = Render(renderer, scene, frame);
            const float r = img.At(36, 40, 0), g = img.At(36, 40, 1), bl = img.At(36, 40, 2);
            Expect(r > 0.2f && r > 1.5f * g && g > bl - 0.5f, "a hot volume glows like a blackbody");
            std::printf("volume: T %.3f, lit %.3f / %.3f, hot %.3f %.3f %.3f\n", std::exp(-1.5f), near, far, r, g, bl);

            // A uniform dome lights the ball from above: its top (y = 0.27)
            // scatters more than its bottom. A sphere light at x = 0.6 lights
            // its near side.
            vkr::Scene sky(&device);
            sky.AddVolume()->fields["density"] = ball->fields["density"];
            sky.AddDome();
            sky.Commit();
            img = Render(renderer, sky, frame);
            Expect(img.At(32, 40, 0) > 0.05f && img.At(32, 40, 0) > 1.2f * img.At(32, 23, 0),
                   "the dome lights a volume from above");
            vkr::Scene bulb(&device);
            bulb.AddVolume()->fields["density"] = ball->fields["density"];
            AddSphereLight(bulb, 0.6f, 0, 0, 0.05f, 200.0f);
            bulb.Commit();
            img = Render(renderer, bulb, frame);
            Expect(img.At(40, 32, 0) > 0.05f && img.At(40, 32, 0) > 3.0f * img.At(23, 32, 0),
                   "a sphere light lights a volume's near side");

            // Volumes shadow each other (VolumePass::Settings::mutualShadows):
            // a big thin ball (radius 1.125, density 0.5) at x 2, between the
            // ball and the light from +x. The ball's lit side (x 0.27, z within
            // +-0.26 of its ray) gets the big one's transmittance along the
            // light. Reference: brute force over the ball's voxels on the
            // pixel's ray, each lit through the big one's voxels (nearest, as
            // the shaders read them) along +x, averaged. The big one,
            // upwind, is as without the ball; mutual shadows off, the ball is
            // as alone. The same from a sphere light at x 4.
            const float alone = Render(renderer, lit, frame).At(40, 32, 0);
            auto inBall = [](int i, int j, int k) {
                const float x = float(i) - 15.5f, y = float(j) - 15.5f, z = float(k) - 15.5f;
                return x * x + y * y + z * z <= 144.0f;
            };
            const float px = (40.5f / 32.0f - 1.0f), py = (32.5f / 32.0f - 1.0f);  // the pixel's world x, y
            double sumT = 0.0, sumHalf = 0.0;  // the dome's: half the optical depth (volume.frag)
            int samples = 0;
            for (float z = -0.5f; z < 0.5f; z += 1.0f / 256.0f) {
                if (!inBall(int(std::floor(px * 32 + 16)), int(std::floor(py * 32 + 16)), int(std::floor(z * 32 + 16)))) {
                    continue;
                }
                int voxels = 0;  // the big one's along +x from (px, py, z): index (world - 2) * 32 / 3 + 15.5
                const int j = int(std::floor(py * 32 / 3 + 16)), k = int(std::floor(z * 32 / 3 + 16));
                for (int i = 0; i < 32; ++i) {
                    voxels += inBall(i, j, k) && (float(i) - 15.5f) * 3 / 32 + 2 > px ? 1 : 0;
                }
                sumT += std::exp(-0.5 * voxels * 3.0 / 32.0);
                sumHalf += std::exp(-0.25 * voxels * 3.0 / 32.0);
                ++samples;
            }
            const float expected = float(sumT / std::max(samples, 1));
            for (int sun = 1; sun >= 0; --sun) {
                vkr::Scene pair(&device);
                pair.AddVolume()->fields["density"] = ball->fields["density"];
                vkr::Volume *upwind = pair.AddVolume();
                upwind->fields["density"] = BallGrid(12.0f, 0.5f);
                upwind->transforms = {3, 0, 0, 0, 0, 3, 0, 0, 0, 0, 3, 0, 2, 0, 0, 1};
                if (sun) {
                    AddLight(pair, 1, 0, 0)->castShadows = true;
                } else {
                    AddSphereLight(pair, 4, 0, 0, 0.05f, 6000.0f);
                }
                pair.Commit();
                const Image both = Render(renderer, pair, frame);
                frame.volume.mutualShadows = false;
                const Image apart = Render(renderer, pair, frame);
                frame.volume.mutualShadows = true;
                const float ratio = both.At(40, 32, 0) / apart.At(40, 32, 0);
                std::printf("mutual volume shadows (%s): lit side %.4f / %.4f = %.3f (expected %.3f), alone %.4f, "
                            "upwind %.4f / %.4f\n", sun ? "sun" : "sphere", both.At(40, 32, 0), apart.At(40, 32, 0),
                            ratio, expected, alone, both.At(62, 32, 0), apart.At(62, 32, 0));
                Expect(apart.At(40, 32, 0) > 0.02f && std::abs(ratio - expected) < 0.02f,
                       "a volume shadows another by its transmittance along the light");
                Expect(std::abs(both.At(62, 32, 0) - apart.At(62, 32, 0)) <= 0.01f * apart.At(62, 32, 0),
                       "a volume isn't shadowed by one behind it");
                if (sun) {
                    Expect(std::abs(apart.At(40, 32, 0) - alone) <= 0.01f * alone,
                           "without mutual shadows a volume is lit as alone");
                }
            }
            frame.volume.mutualShadows = false;
            const float aloneOff = Render(renderer, lit, frame).At(40, 32, 0);
            frame.volume.mutualShadows = true;
            Expect(aloneOff == alone, "mutual shadows leave a lone volume alone");
            {
                // The dome's light comes down from its pole (+y) through the
                // big ball above at y 2: the small ball's top (y 0.27), as the
                // sun's case turned by 90 degrees, at half the optical depth.
                vkr::Scene above(&device);
                above.AddVolume()->fields["density"] = ball->fields["density"];
                vkr::Volume *upper = above.AddVolume();
                upper->fields["density"] = BallGrid(12.0f, 0.5f);
                upper->transforms = {3, 0, 0, 0, 0, 3, 0, 0, 0, 0, 3, 0, 0, 2, 0, 1};
                above.AddDome();
                above.Commit();
                const Image both = Render(renderer, above, frame);
                frame.volume.mutualShadows = false;
                const Image apart = Render(renderer, above, frame);
                frame.volume.mutualShadows = true;
                const float domeExpected = float(sumHalf / std::max(samples, 1));
                const float ratio = both.At(32, 40, 0) / apart.At(32, 40, 0);
                std::printf("mutual volume shadows (dome): top %.4f / %.4f = %.3f (expected %.3f)\n", both.At(32, 40, 0),
                            apart.At(32, 40, 0), ratio, domeExpected);
                Expect(apart.At(32, 40, 0) > 0.02f && std::abs(ratio - domeExpected) < 0.03f,
                       "a volume dims the dome's light on another below it");
            }

            // The material (VolumeShading): absorption 1 at half the density
            // halves the optical depth and scatters nothing; red-only
            // scattering (grey extinction 1) scatters only red; emission 1 per
            // unit of density glows. The density setting multiplies.
            auto shading = std::make_shared<vkr::VolumeShading>();
            std::fill(shading->scattering, shading->scattering + 3, 0.0f);
            std::fill(shading->absorption, shading->absorption + 3, 1.0f);
            shading->densityScale = 0.5f;
            vkr::Scene absorbing(&device);
            vkr::Volume *a = absorbing.AddVolume();
            a->fields["density"] = ball->fields["density"];
            a->shading = shading;
            AddLight(absorbing, 1, 0, 0);
            absorbing.Commit();
            img = Render(renderer, absorbing, frame);
            Expect(std::abs(img.At(32, 32, 2) - std::exp(-0.75f)) < 0.03f && img.At(40, 32, 0) == 0.0f,
                   "a volume material's absorption and density scale");
            frame.volume.density = 2.0f;
            Expect(std::abs(Render(renderer, absorbing, frame).At(32, 32, 2) - std::exp(-1.5f)) < 0.03f,
                   "the density setting multiplies the material's");
            frame.volume.density = 1.0f;
            const float red[3] = {1, 0, 0}, rest[3] = {0, 1, 1};
            std::copy(red, red + 3, shading->scattering);
            std::copy(rest, rest + 3, shading->absorption);
            shading->densityScale = 1.0f;
            absorbing.Commit();  // tables are rebuilt per commit
            img = Render(renderer, absorbing, frame);
            Expect(img.At(40, 32, 0) > 0.05f && img.At(40, 32, 1) == 0.0f, "a volume material's scattering colour");
            std::fill(shading->scattering, shading->scattering + 3, 0.0f);
            const float green[3] = {0, 1, 0};
            std::copy(green, green + 3, shading->emission);
            absorbing.Commit();
            img = Render(renderer, absorbing, frame);
            Expect(img.At(32, 32, 1) > 0.2f && img.At(32, 32, 0) == 0.0f, "a volume material's emission");

            // Half resolution: the same transmittance, and surfaces in front
            // still hide the ball pixel exactly.
            frame.volume.resolution = 2;
            img = Render(renderer, scene, frame);
            const Image full = [&] {
                vkr::Renderer::Frame f = frame;
                f.volume.resolution = 1;
                return Render(renderer, scene, f);
            }();
            Expect(img.At(28, 32, 0) == 0.5f && std::abs(img.At(36, 32, 0) - full.At(36, 32, 0)) < 0.05f,
                   "half resolution volumes stop at nearer surfaces");
            frame.volume.resolution = 1;

            // Under transparency: black glass (opacity 0.5) in front of the
            // glowing ball's left half dims it to half; glass behind its
            // right half leaves it alone. Off, the ball goes over the glass.
            vkr::Scene glassy(&device);
            vkr::Volume *glow = glassy.AddVolume();
            glow->fields["density"] = ball->fields["density"];
            auto emitting = std::make_shared<vkr::VolumeShading>();
            std::fill(emitting->scattering, emitting->scattering + 3, 0.0f);
            std::fill(emitting->absorption, emitting->absorption + 3, 0.2f);
            std::copy(green, green + 3, emitting->emission);
            glow->shading = emitting;
            auto glass = std::make_shared<vkr::Material>();
            std::fill(glass->baseColor, glass->baseColor + 3, 0.0f);
            glass->opacity = 0.5f;
            AddQuad(glassy, -1, -1, 0, 1, -0.5f)->material = glass;
            AddQuad(glassy, 0, -1, 1, 1, 0.5f)->material = glass;
            glassy.Commit();
            const Image under = Render(renderer, glassy, frame);
            frame.volume.underTransparency = false;
            const Image over = Render(renderer, glassy, frame);
            frame.volume.underTransparency = true;
            const float front = under.At(28, 32, 1), back = under.At(36, 32, 1);
            Expect(back > 0.2f && std::abs(front - 0.5f * back) < 0.05f * back,
                   "glass in front of a volume dims it, glass behind doesn't");
            Expect(std::abs(over.At(28, 32, 1) - back) < 0.05f * back && std::abs(over.At(36, 32, 1) - back) < 1e-3f,
                   "without volumes under transparency they go over the glass");
            std::printf("volume behind glass %.3f, in front of glass %.3f, over glass %.3f\n", front, back, over.At(28, 32, 1));

            // Volume shadows: the ball at z -0.5 under a sun from (1, 0, -1)
            // shadows the ground (z 0.5) around x -1, outside the ball's
            // own pixels, by its transmittance along the light: a chord of
            // 0.74 at pixel 3, exp(-2 * 0.74). Black glass of opacity 0.5
            // above (y 0.5..0.9) shadows it by half at the same time.
            vkr::Scene shadowed(&device);
            vkr::Volume *smoke = shadowed.AddVolume();
            smoke->fields["density"] = ball->fields["density"];
            smoke->transforms = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, -0.5f, 1};
            AddLight(shadowed, 1, 0, -1)->castShadows = true;
            AddQuad(shadowed, -1, -1, 1, 1, 0.5f);
            AddQuad(shadowed, 0.95f, 0.95f, 0.99f, 0.99f, 0.9f);  // keeps the ground off the far edge
            AddQuad(shadowed, 0.0f, 0.5f, 0.25f, 0.9f, -0.5f)->material = glass;
            shadowed.Commit();
            const Image cast = Render(renderer, shadowed, frame);
            frame.volume.shadows = false;
            const Image uncast = Render(renderer, shadowed, frame);
            frame.volume.shadows = true;
            frame.volumes = false;
            const Image noVolumes = Render(renderer, shadowed, frame);
            frame.volumes = true;
            const float ratio = cast.At(3, 32, 0) / uncast.At(3, 32, 0);
            const float glassRatio = cast.At(3, 52, 0) / uncast.At(3, 52, 0);
            Expect(uncast.At(3, 32, 0) > 0.1f && std::abs(ratio - std::exp(-1.48f)) < 0.05f,
                   "a volume shadows surfaces from the sun by its transmittance");
            Expect(std::abs(glassRatio - 1.0f) < 0.02f && uncast.At(3, 52, 0) < 0.6f * noVolumes.At(3, 20, 0),
                   "volume and glass shadows together");
            Expect(uncast.At(3, 32, 0) == noVolumes.At(3, 32, 0),
                   "without volume shadows the ground is as without volumes");
            std::printf("volume shadow %.3f (expected %.3f) on ground %.3f, glass beside it %.3f\n", ratio,
                        std::exp(-1.48f), uncast.At(3, 32, 0), uncast.At(3, 52, 0) / noVolumes.At(3, 20, 0));
            if (device.rayTracing) {
                // Traced: the sun's shadow rays march the volume and pass the glass.
                frame.rtShadows = true;
                const Image traced = Render(renderer, shadowed, frame);
                const float tracedRatio = traced.At(3, 32, 0) / uncast.At(3, 32, 0);
                const float tracedGlass = traced.At(3, 52, 0) / noVolumes.At(3, 20, 0);
                std::printf("translucent shadows: volume traced %.3f, maps %.3f; glass traced %.3f, maps %.3f\n",
                            tracedRatio, ratio, tracedGlass, uncast.At(3, 52, 0) / noVolumes.At(3, 20, 0));
                Expect(std::abs(tracedRatio - ratio) < 0.05f && std::abs(tracedGlass - uncast.At(3, 52, 0) / noVolumes.At(3, 20, 0)) < 0.05f,
                       "translucent shadows traced: a volume's transmittance and glass beside it, as the maps'");
                frame.rtTranslucentShadows = false;
                Expect(Render(renderer, shadowed, frame).At(3, 32, 0) > 0.98f * uncast.At(3, 32, 0),
                       "Frame::rtTranslucentShadows off: traced shadows ignore the volume");
                frame.rtTranslucentShadows = true;
                frame.rtShadows = false;
            }

            // The same from a small sphere light on the sun's line, 8 away
            // from the ground point (the ray to pixel 3 crosses the ball
            // as the sun's did): its atlas tiles take the volume's
            // transmittance (Frame::localVolumeShadows).
            vkr::Scene lamp(&device);
            vkr::Volume *puff = lamp.AddVolume();
            puff->fields["density"] = ball->fields["density"];
            puff->transforms = smoke->transforms;
            vkr::Light *lampLight = AddSphereLight(lamp, 4.75f, 0.0156f, -5.157f, 0.05f, 4000.0f);
            lampLight->castShadows = true;
            AddQuad(lamp, -1, -1, 1, 1, 0.5f);
            lamp.Commit();
            const Image lampLit = Render(renderer, lamp, frame);
            frame.localVolumeShadows = false;
            const Image lampUnlit = Render(renderer, lamp, frame);
            frame.localVolumeShadows = true;
            const float lampRatio = lampLit.At(3, 32, 0) / lampUnlit.At(3, 32, 0);
            std::printf("local volume shadow %.3f (expected %.3f) on ground %.3f\n", lampRatio, std::exp(-1.48f),
                        lampUnlit.At(3, 32, 0));
            Expect(lampUnlit.At(3, 32, 0) > 0.05f && std::abs(lampRatio - std::exp(-1.48f)) < 0.06f,
                   "a volume shadows surfaces from a local light by its transmittance");
            Expect(std::abs(lampLit.At(40, 32, 0) / lampUnlit.At(40, 32, 0) - 1.0f) < 0.02f,
                   "local volume shadows leave the ground out of the smoke's shadow alone");
            if (device.rayTracing) {
                frame.rtShadows = true;
                const float tracedLamp = Render(renderer, lamp, frame).At(3, 32, 0) / lampUnlit.At(3, 32, 0);
                frame.rtShadows = false;
                std::printf("translucent shadows: local volume traced %.3f, maps %.3f\n", tracedLamp, lampRatio);
                Expect(std::abs(tracedLamp - lampRatio) < 0.06f,
                       "translucent shadows traced: a volume shadows a local light's light as the maps do");
            }
        }

        {
            // Displacement: a quad behind the far plane (z 1.1, culled and
            // clipped as it is) displaced 0.3 along its normal (-z, towards
            // the camera) comes into view at z 0.8, depth 0.9, in every pass.
            vkr::Scene scene(&device);
            AddLight(scene, 0, 0, -1);
            vkr::Mesh *quad = AddQuad(scene, -1, -1, 0, 1, 1.1f);
            quad->normals.clear();
            for (int i = 0; i < 6; ++i) {
                quad->normals.insert(quad->normals.end(), {0, 0, -1});
            }
            auto material = std::make_shared<vkr::Material>();
            material->displacement = 0.3f;
            material->displacementBound = 0.3f;
            quad->material = material;
            scene.Commit();
            const Image img = Render(renderer, scene);
            Expect(std::abs(img.depth[32 * 64 + 16] - 0.9f) < 1e-3f && img.At(16, 32, 0) > 0.1f,
                   "displacement: the quad moved along its normal into view");
            vkr::Renderer::Frame frame = IdentityFrame();
            frame.depthPrepass = false;
            Expect(MaxDifference(Render(renderer, scene, frame), img) == 0.0f,
                   "displacement: the depth prepass sees the same positions");
            frame = IdentityFrame();
            frame.displacementScale = 0.5f;
            Expect(std::abs(Render(renderer, scene, frame).depth[32 * 64 + 16] - 0.975f) < 1e-3f,
                   "Frame::displacementScale scales the offset");
            frame.displacement = false;
            Expect(Render(renderer, scene, frame).depth[32 * 64 + 16] == 1.0f, "Frame::displacement off: not displaced");
        }

        {
            // Levels of detail: a bumpy 32 x 32 grid (2048 triangles), one
            // instance filling the view and one 4 pixels wide. The small one
            // draws a coarser level; Frame::lod off, or a bias asking for a
            // thousandth of a pixel, draws both whole.
            vkr::Scene scene(&device);
            AddLight(scene, 0, 0, -1);
            vkr::Mesh *grid = scene.AddMesh();
            const int n = 32;
            auto point = [&](int i, int j) {
                const float x = float(i) / n * 2 - 1, y = float(j) / n * 2 - 1;
                return std::array<float, 3>{x, y, 0.05f * std::sin(x * 9) * std::cos(y * 7)};
            };
            for (int j = 0; j < n; ++j) {
                for (int i = 0; i < n; ++i) {
                    for (const auto &c : {point(i, j), point(i + 1, j), point(i + 1, j + 1), point(i, j),
                                          point(i + 1, j + 1), point(i, j + 1)}) {
                        grid->points.insert(grid->points.end(), c.begin(), c.end());
                    }
                }
            }
            grid->transforms = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0.5f, 1,
                                0.06f, 0, 0, 0, 0, 0.06f, 0, 0, 0, 0, 0.06f, 0, 0.5f, 0.5f, 0.2f, 1};
            do {  // the levels build in the background
                scene.Commit();
            } while (scene.LodsPending());
            Render(renderer, scene);
            const uint64_t lod = renderer.GetStats().triangles;
            vkr::Renderer::Frame whole = IdentityFrame();
            whole.lod = false;
            Render(renderer, scene, whole);
            Expect(renderer.GetStats().triangles == 2u * 2048u, "Frame::lod off: every triangle");
            Expect(lod < 2u * 2048u - 512u, "levels of detail: the small grid draws a coarser level");
            whole.lod = true;
            whole.lodBias = -10.0f;
            Render(renderer, scene, whole);
            Expect(renderer.GetStats().triangles == 2u * 2048u, "Frame::lodBias -10: every triangle");
        }

        {
            // Levels of detail keep the shading: a flat 32 x 32 grid filling
            // the view collapses to a few triangles with one normal, but
            // keeps them all with a bumpy surface's normals (a smooth-shaded
            // panel, whose big triangles would interpolate other normals).
            uint64_t triangles[2] = {};
            for (int bumpy = 0; bumpy < 2; ++bumpy) {
                vkr::Scene scene(&device);
                AddLight(scene, 0, 0, -1);
                vkr::Mesh *grid = scene.AddMesh();
                const int n = 32;
                for (int j = 0; j < n; ++j) {
                    for (int i = 0; i < n; ++i) {
                        for (const auto &[di, dj] : {std::pair{0, 0}, {1, 0}, {1, 1}, {0, 0}, {1, 1}, {0, 1}}) {
                            const float x = float(i + di) / n * 2 - 1, y = float(j + dj) / n * 2 - 1;
                            const float nx = bumpy ? 0.3f * std::cos(x * 9) : 0.0f, ny = bumpy ? 0.3f * std::sin(y * 7) : 0.0f;
                            const float length = std::sqrt(nx * nx + ny * ny + 1);
                            grid->points.insert(grid->points.end(), {x, y, 0.0f});
                            grid->normals.insert(grid->normals.end(), {nx / length, ny / length, -1 / length});
                        }
                    }
                }
                grid->transforms = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0.5f, 1};
                do {
                    scene.Commit();
                } while (scene.LodsPending());
                Render(renderer, scene);
                triangles[bumpy] = renderer.GetStats().triangles;
            }
            Expect(triangles[0] < 512u, "levels of detail: a flat grid with one normal draws a few triangles");
            Expect(triangles[1] == 2048u, "levels of detail: a flat grid with bumpy normals keeps every triangle");
        }

        {
            // Level of detail crossfades: a bumpy grid 4 pixels wide (a
            // coarse level) grows to fill the view (all 2048 triangles). With
            // Frame::lodFade it draws both levels for a few frames, then the
            // fine one alone; without, it switches at once.
            for (const bool fade : {true, false}) {
                vkr::Scene scene(&device);
                AddLight(scene, 0, 0, -1);
                vkr::Mesh *grid = scene.AddMesh();
                const int n = 32;
                for (int j = 0; j < n; ++j) {
                    for (int i = 0; i < n; ++i) {
                        for (const auto &[di, dj] : {std::pair{0, 0}, {1, 0}, {1, 1}, {0, 0}, {1, 1}, {0, 1}}) {
                            const float x = float(i + di) / n * 2 - 1, y = float(j + dj) / n * 2 - 1;
                            grid->points.insert(grid->points.end(), {x, y, 0.05f * std::sin(x * 9) * std::cos(y * 7)});
                        }
                    }
                }
                grid->transforms = {0.06f, 0, 0, 0, 0, 0.06f, 0, 0, 0, 0, 0.06f, 0, 0.5f, 0.5f, 0.2f, 1};
                do {
                    scene.Commit();
                } while (scene.LodsPending());
                vkr::Renderer::Frame frame = IdentityFrame();
                frame.lodFade = fade;
                frame.lodBias = -4.0f;  // a sixteenth of a pixel: the full-size grid needs every triangle
                Render(renderer, scene, frame);
                Render(renderer, scene, frame);
                const uint64_t small = renderer.GetStats().triangles;
                grid->transforms = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0.5f, 1};
                grid->transformsDirty = true;
                scene.Commit();
                Render(renderer, scene, frame);
                const uint64_t first = renderer.GetStats().triangles;
                for (int k = 0; k < 10; ++k) {
                    Render(renderer, scene, frame);
                }
                const uint64_t settled = renderer.GetStats().triangles;
                Expect(small < 2048u && settled == 2048u, "lod fade: the grown grid ends at its finest level");
                Expect(fade ? first == 2048u + small : first == 2048u,
                       fade ? "Frame::lodFade: both levels while fading" : "lodFade off: the level switches at once");
            }
        }

        {
            // Light clamps: a quad in a sun ten times as bright as usual
            // (radiance 10 x albedo) comes out at the direct clamp; the SSGI
            // scene's red emitter (radiance 2) bleeds a quarter of its light
            // onto the wall when it comes in clamped to 0.5 (less than half
            // after the tonemap).
            vkr::Scene lit(&device);
            vkr::Light *sun = AddLight(lit, 0, 0, -1);
            std::fill(sun->irradiance, sun->irradiance + 3, 31.4159265f);
            AddQuad(lit, -1, -1, 1, 1, 0);
            lit.Commit();
            auto direct = [&](float clamp) {
                vkr::Renderer::Frame frame = IdentityFrame();
                frame.directClamp = clamp;
                const Image img = Render(renderer, lit, frame);
                return std::max({img.At(32, 32, 0), img.At(32, 32, 1), img.At(32, 32, 2)});
            };
            const float unclamped = direct(0.0f), clamped = direct(2.0f);
            Expect(unclamped > 5.0f && std::abs(clamped - 2.0f) < 0.01f, "the direct clamp caps a pixel's direct light");

            vkr::Scene scene(&device);
            auto white = std::make_shared<vkr::Material>();
            std::fill(white->baseColor, white->baseColor + 3, 0.8f);
            white->roughness = 1;
            AddQuad(scene, -1, -1, 1, 1, 0.5f)->material = white;
            vkr::Mesh *panel = AddEmitter(scene, 0, 0, 0, 0, 0, 2.0f);
            panel->points = {0, -0.6f, 0.5f, 0.4f, -0.6f, 0, 0.4f, 0.6f, 0, 0, -0.6f, 0.5f, 0.4f, 0.6f, 0, 0, 0.6f, 0.5f};
            auto red = std::make_shared<vkr::Material>(*panel->material);
            red->emissive[1] = red->emissive[2] = 0;
            panel->material = red;
            scene.Commit();
            auto wall = [&](float clamp) {
                vkr::Renderer ssgiRenderer(device);
                vkr::Renderer::Frame frame = IdentityFrame();
                frame.postProcess = true;
                frame.post.autoExposure = false;
                frame.post.bloomIntensity = 0.0f;
                frame.ssgi = true;
                frame.ssgiRadius = 2.0f;
                frame.indirectClamp = clamp;
                Image img;
                for (int i = 0; i < 16; ++i) {
                    img = Render(ssgiRenderer, scene, frame);
                }
                return img.At(29, 32, 0);
            };
            const float full = wall(0.0f), quarter = wall(0.5f);
            Expect(quarter > 0.0f && quarter < 0.5f * full, "the indirect clamp caps SSGI's incoming light");
            Expect(vkr::Renderer::Frame().indirectClamp == 10.0f, "the indirect clamp is 10 by default");
        }

        {
            // Light splits: a grey quad under a head-on distant light (no
            // dome): its light is all direct, diffuse and glossy add up to
            // the colour, and nothing is indirect. Accumulated over TAA's
            // still frames like the colour.
            vkr::Scene scene(&device);
            AddLight(scene, 0, 0, -1);
            AddQuad(scene, -1, -1, 1, 1, 0)->primId = 1;  // AOVs are 0 where no prim is
            scene.Commit();
            using A = vkr::Aovs;
            std::vector<float> splits[A::kCount];
            Image img;
            vkr::Renderer::Targets out;
            out.color = img.color.data();
            for (uint32_t k = A::kDirectDiffuse; k <= A::kCombinedGlossy; ++k) {
                splits[k].assign(64 * 64 * 3, -7.0f);
                out.aovs[k] = splits[k].data();
            }
            renderer.Render(scene, IdentityFrame(), out);
            auto at = [&](uint32_t k, int c) { return splits[k][(32 * 64 + 32) * 3 + c]; };
            const float sum = at(A::kCombinedDiffuse, 1) + at(A::kCombinedGlossy, 1);
            Expect(at(A::kDirectDiffuse, 1) > 0.3f && at(A::kDirectGlossy, 1) > 0.0f &&
                       std::abs(at(A::kIndirectDiffuse, 1)) < 1e-4f && std::abs(at(A::kIndirectGlossy, 1)) < 1e-4f,
                   "light splits: a distant light's light is direct");
            Expect(std::abs(sum - img.At(32, 32, 1)) < 2e-3f, "light splits add up to the colour");

            // Under a white dome alone (Karma's convention): its light is
            // direct; Frame::splitDomeDirect off: indirect. Both add up.
            vkr::Scene sky(&device);
            sky.AddDome();
            AddQuad(sky, -1, -1, 1, 1, 0)->primId = 1;
            sky.Commit();
            for (bool domeDirect : {true, false}) {
                vkr::Renderer::Frame frame = IdentityFrame();
                frame.splitDomeDirect = domeDirect;
                renderer.Render(sky, frame, out);
                const float direct = at(A::kDirectDiffuse, 1) + at(A::kDirectGlossy, 1);
                const float indirect = at(A::kIndirectDiffuse, 1) + at(A::kIndirectGlossy, 1);
                Expect((domeDirect ? direct : indirect) > 0.5f && std::abs(domeDirect ? indirect : direct) < 1e-3f &&
                           std::abs(direct + indirect - img.At(32, 32, 1)) < 2e-3f,
                       domeDirect ? "light splits: the dome's light is direct" : "splitDomeDirect off: the dome's is indirect");
            }

            // Under the film the splits average its jittered samples:
            // a slanted edge gets values between none and the inside's.
            vkr::Scene slanted(&device);
            AddLight(slanted, 0, 0, -1);
            vkr::Mesh *tri = AddQuad(slanted, 0, 0, 0, 0, 0);
            tri->points = {-1, -1, 0, 1, -1, 0, -1, 0.6f, 0};
            tri->uvs.clear();
            tri->primId = 1;
            slanted.Commit();
            vkr::Renderer taaRenderer(device);
            vkr::Renderer::Frame frame = IdentityFrame();
            frame.postProcess = true;
            int frames = 0;
            do {
                taaRenderer.Render(slanted, frame, out);
                ++frames;
            } while (!taaRenderer.IsConverged() && frames < 200);
            const float inside = splits[A::kDirectDiffuse][(4 * 64 + 4) * 3 + 1];
            int partial = 0;
            for (int i = 0; i < 64 * 64; ++i) {
                const float v = splits[A::kDirectDiffuse][i * 3 + 1];
                partial += v > 0.1f * inside && v < 0.9f * inside;
            }
            Expect(frames == int(frame.filmSamples) && inside > 0.3f && partial > 20,
                   "light splits average the film's samples");
        }

        {
            // Pipeline cache: what this run compiled is on disk, and a new
            // device starts with it.
            // Saving prunes the other shader builds' caches of this driver
            // (same UUID prefix) and leaves other drivers' alone.
            device.SavePipelineCache();
            std::filesystem::path current;
            for (const auto &entry : std::filesystem::directory_iterator(cacheDir, cacheError)) {
                if (entry.path().extension() == ".bin") {
                    current = entry.path();
                }
            }
            const std::string name = current.filename().string();
            const std::filesystem::path stale = cacheDir / (name.substr(0, name.rfind('-') + 1) + "0000000000000000.bin"),
                                        other = cacheDir / "pipelines-ffffffff-0000000000000000.bin";
            std::filesystem::copy_file(current, stale, cacheError);
            std::filesystem::copy_file(current, other, cacheError);
            device.SavePipelineCache();
            Expect(!std::filesystem::exists(stale) && std::filesystem::exists(current) &&
                       std::filesystem::exists(other),
                   "pipeline cache: saving removes this driver's older caches, not other drivers'");
            std::filesystem::remove(other, cacheError);
            uintmax_t bytes = 0;
            for (const auto &entry : std::filesystem::directory_iterator(cacheDir, cacheError)) {
                bytes = entry.path().extension() == ".bin" ? entry.file_size() : bytes;
            }
            Expect(bytes > 0, "pipeline cache: saved");
            vkr::Device second;
            vkr::Renderer secondRenderer(second);
            Expect(second.pipelineCacheBytes == bytes, "pipeline cache: a new device starts with it");
        }

        {
            // Pipeline prewarm: after a frame a worker compiles the shading
            // sets one toggle away; turning toon bands on then takes its set
            // (no compile), which draws as one compiled on the spot.
            vkr::Scene scene(&device);
            AddLight(scene, 0, 0, -1);
            AddQuad(scene, -1, -1, 1, 1, 0.5f);
            scene.Commit();
            vkr::Renderer warm(device), cold(device);
            vkr::Renderer::Frame frame = IdentityFrame();
            frame.pipelinePrewarm = true;
            Render(warm, scene, frame);
            warm.WaitForPrewarm();
            Expect(warm.GetStats().prewarmedSets == 0, "pipeline prewarm: nothing taken yet");
            frame.toonBands = 3;
            const Image prewarmed = Render(warm, scene, frame);
            Expect(warm.GetStats().prewarmedSets == 1, "pipeline prewarm: the toon set came from the worker");
            frame.pipelinePrewarm = false;
            const Image compiled = Render(cold, scene, frame);
            Expect(cold.GetStats().prewarmedSets == 0 && prewarmed.color == compiled.color,
                   "pipeline prewarm: the same image as a set compiled when needed");
        }

        {
            // Prewarm pairs: fog and SSGI turned on together take the set
            // the worker made for the pair (two toggles, one frame).
            vkr::Scene scene(&device);
            AddLight(scene, 0, 0, -1);
            AddQuad(scene, -1, -1, 1, 1, 0.5f);
            scene.Commit();
            vkr::Renderer warm(device);
            vkr::Renderer::Frame frame = IdentityFrame();
            frame.pipelinePrewarm = true;
            Render(warm, scene, frame);
            warm.WaitForPrewarm();
            frame.fog = frame.ssgi = true;
            Render(warm, scene, frame);
            Expect(warm.GetStats().prewarmedSets == 1, "pipeline prewarm: fog and SSGI together came from the worker");
        }

        {
            // Refraction from inside: a water surface (transmission 1, ior
            // 1.33) at z = -2 with its normal facing away from the camera,
            // i.e. seen from below, in front of a red (x < 0) and green wall.
            // Seen head-on it lets the wall through (and is drawn: its ids);
            // turned 60 degrees, past the critical angle (48.8), it reflects
            // the inside totally and lets nothing through; with
            // Frame::refractionBackFaces off it isn't drawn at all.
            vkr::Scene scene(&device);
            vkr::Mesh *left = AddEmitter(scene, -6, -6, 0, 6, -5, 1.0f);
            left->material->emissive[1] = left->material->emissive[2] = 0;
            vkr::Mesh *right = AddEmitter(scene, 0, -6, 6, 6, -5, 1.0f);
            right->material->emissive[0] = right->material->emissive[2] = 0;
            vkr::Mesh *water = AddQuad(scene, -1.5f, -1.5f, 1.5f, 1.5f, -2);
            water->primId = 7;
            auto material = std::make_shared<vkr::Material>();
            std::fill(material->baseColor, material->baseColor + 3, 1.0f);
            material->roughness = 0;
            material->transmission = 1;
            material->ior = 1.33f;
            water->material = material;
            auto turn = [&](float degrees) {
                const float c = std::cos(degrees * 3.14159265f / 180.0f), s = std::sin(degrees * 3.14159265f / 180.0f);
                water->points.clear();
                water->normals.clear();
                const float corners[6][2] = {{-1.5f, -1.5f}, {1.5f, -1.5f}, {1.5f, 1.5f},
                                             {-1.5f, -1.5f}, {1.5f, 1.5f}, {-1.5f, 1.5f}};
                for (const auto &q : corners) {
                    water->points.insert(water->points.end(), {q[0] * c, q[1], -2.0f - q[0] * s});
                    water->normals.insert(water->normals.end(), {s, 0, -c});  // away from the camera
                }
                water->geometryDirty = true;
                scene.Commit();
            };
            vkr::Renderer::Frame frame = PerspectiveFrame();
            turn(0);
            const Image under = Render(renderer, scene, frame);
            Expect(under.Prim(44, 32) == 7 && under.At(44, 32, 1) > 0.9f && under.At(44, 32, 0) < 0.05f,
                   "refraction from inside: water seen from below is drawn and lets the wall through");
            turn(60);
            const Image total = Render(renderer, scene, frame);
            frame.refractionBackFaces = false;
            const Image off = Render(renderer, scene, frame);
            // Past the critical angle the centre shows the mirrored inside (the
            // red half), not the green it would refract towards.
            Expect(total.Prim(32, 32) == 7 && total.At(32, 32, 0) > 0.9f && total.At(32, 32, 1) < 0.05f &&
                       off.At(32, 32, 1) > 0.9f,
                   "refraction from inside: total internal reflection past the critical angle");
            Expect(off.Prim(32, 32) != 7 && off.Prim(44, 32) != 7, "Frame::refractionBackFaces off: not drawn");
        }

        {
            // Refraction through layers: two thin-walled glass quads over a
            // white wall, the far one taking out green, the near one blue.
            // Where they overlap, light passes both (red) with
            // Frame::multiRefraction; without, the near one refracts the
            // opaque wall alone and hides the far one (yellow).
            vkr::Scene scene(&device);
            AddEmitter(scene, -1, -1, 1, 1, 0.5f, 1.0f);
            auto glass = [&](float x0, float x1, float z, float g, float b) {
                auto m = std::make_shared<vkr::Material>();
                m->roughness = 0;
                m->transmission = 1;
                m->thinWalled = true;
                m->transmissionColor[1] = g;
                m->transmissionColor[2] = b;
                AddQuad(scene, x0, -1, x1, 1, z)->material = m;
            };
            glass(-1, 0.5f, 0.25f, 0, 1);  // far: no green
            glass(-0.5f, 1, 0, 1, 0);      // near: no blue
            scene.Commit();
            const Image both = Render(renderer, scene);
            vkr::Renderer::Frame frame = IdentityFrame();
            frame.multiRefraction = false;
            const Image one = Render(renderer, scene, frame);
            Expect(both.At(32, 32, 0) > 0.8f && both.At(32, 32, 1) < 0.05f && both.At(32, 32, 2) < 0.05f,
                   "refraction through layers: the overlap passes both glasses");
            Expect(one.At(32, 32, 1) > 0.8f && one.At(32, 32, 2) < 0.05f,
                   "Frame::multiRefraction off: the near glass refracts the opaque wall only");
            Expect(both.At(8, 32, 1) < 0.05f && both.At(56, 32, 2) < 0.05f && both.At(56, 32, 1) > 0.8f,
                   "refraction through layers: each glass alone as before");
        }
    } catch (const std::exception &e) {
        std::printf("FAIL: %s\n", e.what());
        return 1;
    }

    std::filesystem::remove_all(cacheDir, cacheError);
    std::puts(failures ? "FAILED" : "OK");
    return failures ? 1 : 0;
}
