// Standalone check of the renderer layer, no USD needed: renders small
// scenes and checks pixels, depth, row order, PBR normalization, textures,
// instancing, image-based lighting, shadows and the sky orientation.
#include "renderer.h"

#include <algorithm>
#include <array>
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
            Expect(sharp < 0.01f && focused < 0.01f && blurred > 0.05f,
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
            AddQuad(scene, -1, -1, 1, 1, 0.5f)->material = white;
            vkr::Mesh *panel = scene.AddMesh();
            panel->points = {0, -0.6f, -0.5f, 0, 0.6f, -0.5f, 0, 0.6f, 0.45f,
                             0, -0.6f, -0.5f, 0, 0.6f, 0.45f, 0, -0.6f, 0.45f};
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
            const float r = img.At(28, 32, 0), g = img.At(28, 32, 1);
            Expect(std::abs(direct.At(28, 32, 0) - direct.At(28, 32, 1)) < 1e-3f && r > 1.2f * g && g > 0.0f,
                   "GI bounces the red panel's light onto the white wall");
            Expect(frames > 8 && frames < 200, "GI holds convergence back while its probes settle");
            Expect(std::abs(img.At(28, 32, 0) - last.At(28, 32, 0)) < 0.02f * r, "settled GI is stable frame to frame");
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
        } else {
            std::puts("no ray tracing on this GPU: its tests skipped");
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
    } catch (const std::exception &e) {
        std::printf("FAIL: %s\n", e.what());
        return 1;
    }

    std::puts(failures ? "FAILED" : "OK");
    return failures ? 1 : 0;
}
