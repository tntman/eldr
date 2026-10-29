// Lights and the clustered light lists; include after frame.glsl.
// std430 mirror of GpuLight in scene.cpp. Distant lights come first
// (frame.counts.x of them), then frame.localLights.x sphere/rect lights.
#ifdef LIGHTS_WRITABLE
#define CLUSTER_ACCESS
#else
#define CLUSTER_ACCESS readonly
#endif

const uint kLightSphere = 1u, kLightRect = 2u, kLightDisk = 3u, kLightCylinder = 4u;

struct Light {
    vec4 position;    // distant: direction to the light; else center. w: type
    vec4 color;       // distant: irradiance; else radiance of the emitting surface. w: 1 drawn (light_shapes.frag),
                      // + 2 casts shadows
    vec4 axis;        // emission axis (light -Z); w: sphere/cylinder radius, distant: half its angle (radians)
    vec4 halfWidth;   // rect: center to the +X edge, disk: radius along X, cylinder: center to the +X end;
                      // w: sphere/cylinder shading radius (0 = point/line)
    vec4 halfHeight;  // rect: center to the +Y edge, disk: radius along Y; w: diffuse multiplier
    vec4 shaping;     // soft cone angle, cone angle (radians), focus, specular multiplier
    vec4 focusTint;   // w: rect/disk: the texture's bindless index (-1: none)
    vec4 ies;         // xyz: the light's +X (IES phi 180); w: its IES profile's bindless index (-1: none)
    vec4 link;        // x: IES angle scale; y, z: light, shadow link bit of Instance::linkMask (-1: everything)
};
const uint kLightTextures = 4u;  // frame.reflections.z: lights' textures on
layout(std430, set = 0, binding = 2) readonly buffer Lights { Light lights[]; };

// 16 x 9 screen tiles x 24 slices, logarithmic in depth. Each cluster is a
// count followed by up to kMaxClusterLights indices into lights[].
// ponytail: lights past 63 in one cluster are dropped; raise the cap (or
// sort by contribution) if dense light rigs need it.
const uint kClustersX = 16u, kClustersY = 9u, kClustersZ = 24u;
const uint kMaxClusterLights = 63u;
layout(std430, set = 0, binding = 14) CLUSTER_ACCESS buffer Clusters { uint clusterLights[]; };

// UsdLux lights have no range. A sphere/rect light is cut off where it
// would light a white surface facing it with less than the cutoff radiance
// (1% of middle gray at the current exposure), measured from its center.
float LightRange(Light light)
{
    float radiance = max(max(light.color.r, light.color.g), light.color.b) *
                     max(light.halfHeight.w, light.shaping.w);
    if (uint(light.position.w) == kLightSphere) {
        // Reflected radiance L R^2 / d^2.
        return light.axis.w * sqrt(radiance / frame.clusters.w);
    }
    if (uint(light.position.w) == kLightCylinder) {
        // Seen from the side: a 2r x 2h rect.
        float h = length(light.halfWidth.xyz);
        return sqrt(radiance * 4.0 * light.axis.w * h / (PI * frame.clusters.w)) + h;
    }
    // On-axis intensity of a Lambertian rect or disk is L * area.
    float area = (uint(light.position.w) == kLightRect ? 4.0 : PI) *
                 length(cross(light.halfWidth.xyz, light.halfHeight.xyz));
    return sqrt(radiance * area / (PI * frame.clusters.w)) +
           length(light.halfWidth.xyz + light.halfHeight.xyz);
}

// Polygon form factors (mesh.frag's rect lights, rt_shade.glsl's ShadeHit).
// One edge's term of a polygon's form factor vector (corners relative to
// the shading point); the sum over a closed loop, / 2 pi, is the vector.
vec3 EdgeFormFactor(vec3 a, vec3 b)
{
    vec3 v0 = normalize(a), v1 = normalize(b);
    vec3 c = cross(v0, v1);
    float len = length(c);
    return len > 1e-8 ? acos(clamp(dot(v0, v1), -1.0, 1.0)) * c / len : vec3(0.0);
}

// Lambert's form factor of the rect toLight +- hw +- hh clipped to the
// horizon of n: exact (irradiance / (pi L)), where the sphere approximation
// of the clipped form factor ((f^2 + F.n) / (f + 1)) was up to 4% bright on
// a rect above the horizon (Karma integrates it exactly).
float RectFormFactorClipped(vec3 toLight, vec3 hw, vec3 hh, vec3 n)
{
    vec3 corners[4] = vec3[4](toLight + hw + hh, toLight - hw + hh, toLight - hw - hh, toLight + hw - hh);
    vec3 poly[5];
    int count = 0;
    for (int i = 0; i < 4; ++i) {
        vec3 a = corners[i], b = corners[(i + 1) % 4];
        float da = dot(a, n), db = dot(b, n);
        if (da > 0.0) {
            poly[count++] = a;
        }
        if ((da > 0.0) != (db > 0.0)) {
            poly[count++] = a + (b - a) * (da / (da - db));
        }
    }
    float sum = 0.0;
    for (int i = 0; i < count; ++i) {
        sum += dot(EdgeFormFactor(poly[i], poly[(i + 1) % count]), n);
    }
    return abs(sum) / (2.0 * PI);
}

// Light linking (UsdLux collection:lightLink): whether the light lights a
// receiver of this link mask (Instance::linkMask, a volume's); and
// (collection:shadowLink) whether a caster of it shadows the light.
bool LightLinked(Light light, uint mask)
{
    int bit = int(light.link.y);
    return bit < 0 || (mask & (1u << uint(bit))) != 0u;
}

bool ShadowLinked(Light light, uint mask)
{
    int bit = int(light.link.z);
    return bit < 0 || (mask & (1u << uint(bit))) != 0u;
}

// The light's IES profile (UsdLux shaping:ies) towards p, relative to its
// peak (Scene folds the peak into the colour): theta from the emission
// axis, phi around it from the light's -X (Karma's), theta rescaled by
// shaping:ies:angleScale as UsdLux says. 1 without a profile.
float LightIes(Light light, vec3 p)
{
    int tex = int(light.ies.w);
    if (tex < 0) {
        return 1.0;
    }
    vec3 d = normalize(p - light.position.xyz);
    vec3 z = -light.axis.xyz, x = light.ies.xyz, y = cross(z, x);
    float theta = acos(clamp(-dot(d, z), -1.0, 1.0));
    float phi = atan(-dot(d, y), -dot(d, x));  // 0 at -X, as Karma
    float scale = light.link.x;
    theta = scale > 0.0 ? theta / scale : scale < 0.0 ? (theta - PI) / -scale + PI : theta;
    if (theta < 0.0) {
        return 0.0;
    }
    return textureLod(textures[nonuniformEXT(tex)], vec2(phi / (2.0 * PI), min(theta, PI) / PI), 0.0).r;
}

// A textured rect/disk light's colour as seen from p: the texel above p's
// foot on the light's plane, at the mip of the light seen from its height
// (mesh.frag's diffuseTint). White for other lights or with light textures
// off; times its IES profile (LightIes). Shared by the paths that treat
// the light as a point (traced hits, probes, fog, volumes).
// ponytail: one tap, no filtered polygon integral; mesh.frag has the same.
vec3 LightTint(Light light, vec3 p)
{
    int tex = int(light.focusTint.w);
    uint type = uint(light.position.w);
    float ies = LightIes(light, p);
    if (tex < 0 || (frame.reflections.z & kLightTextures) == 0u || (type != kLightRect && type != kLightDisk)) {
        return vec3(ies);
    }
    vec3 hw = light.halfWidth.xyz, hh = light.halfHeight.xyz, toLight = light.position.xyz - p;
    vec2 foot = vec2(dot(-toLight, hw) / dot(hw, hw), dot(-toLight, hh) / dot(hh, hh));
    foot = type == kLightRect ? clamp(foot, -1.0, 1.0) : foot / max(length(foot), 1.0);
    ivec2 size = textureSize(textures[nonuniformEXT(tex)], 0);
    float texels = float(max(size.x, size.y));
    float extent = length(hw) + length(hh);
    float height = abs(dot(toLight, light.axis.xyz));
    float lod = log2(texels * clamp(height / extent, 1.0 / texels, 1.0));
    return ies * textureLod(textures[nonuniformEXT(tex)], vec2(0.5) + foot * vec2(-0.5, 0.5), lod).rgb;
}

uint ClusterIndex(vec2 fragCoord, float viewDepth)
{
    uvec2 tile = min(uvec2(fragCoord / frame.viewport.xy * vec2(kClustersX, kClustersY)),
                     uvec2(kClustersX - 1u, kClustersY - 1u));
    float s = log2(max(viewDepth, frame.clusters.x) / frame.clusters.x) / frame.clusters.z;
    uint slice = min(uint(s * float(kClustersZ)), kClustersZ - 1u);
    return (slice * kClustersY + tile.y) * kClustersX + tile.x;
}
