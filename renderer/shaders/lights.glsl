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
    vec4 color;       // distant: irradiance; else radiance of the emitting surface
    vec4 axis;        // emission axis (light -Z); w: sphere/cylinder radius, distant: tan of its angular radius
    vec4 halfWidth;   // rect: center to the +X edge, disk: radius along X, cylinder: center to the +X end;
                      // w: sphere/cylinder shading radius (0 = point/line)
    vec4 halfHeight;  // rect: center to the +Y edge, disk: radius along Y; w: diffuse multiplier
    vec4 shaping;     // soft cone angle, cone angle (radians), focus, specular multiplier
    vec4 focusTint;
};
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

uint ClusterIndex(vec2 fragCoord, float viewDepth)
{
    uvec2 tile = min(uvec2(fragCoord / frame.viewport.xy * vec2(kClustersX, kClustersY)),
                     uvec2(kClustersX - 1u, kClustersY - 1u));
    float s = log2(max(viewDepth, frame.clusters.x) / frame.clusters.x) / frame.clusters.z;
    uint slice = min(uint(s * float(kClustersZ)), kClustersZ - 1u);
    return (slice * kClustersY + tile.y) * kClustersX + tile.x;
}
