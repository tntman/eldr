// Caustic photons (RayTracing::Work::caustics): rt_refraction.rgen built
// with PHOTONS, so they meet glass as its refracted rays do (Evaluate).
// Launch: x a photon, y which of rt.causticLights sends it. Each starts on
// its light aimed at the casters' bounding sphere, carrying the light's
// flux through that solid angle (a sun's: through the sphere's disc) as
// the shading pass lights surfaces (ShadeHit's intensities, the diffuse
// multiplier), and goes on through the casters (DrawInfo::caustics): their
// glass refracts it or reflects it by Fresnel (absorbing as
// rt_refraction.rgen), their metal reflects it (Schlick from the base
// colour), a GGX normal at no less than their causticsRoughness (Karma's
// roughness clamp); the rest of a caster is its diffuse surface. The first
// surface past a caster that isn't one, or a caster's diffuse surface,
// keeps it if it receives (caustics.glsl); photons that meet no caster
// first are the direct light the shading pass has already.
// ponytail: no light textures, ranges or cylinder lights; a sphere light
// sends from its centre.
// (rt_refraction.rgen defines CAUSTICS_WRITE before rt_shade.glsl includes caustics.glsl.)

const uint kMaxPhotonBounces = 8u;

// Where a photon of `light` starts, its direction and flux; false when it
// carries none.
bool EmitPhoton(Light light, vec2 u0, vec2 u1, out vec3 origin, out vec3 dir, out vec3 flux)
{
    vec3 c = rt.causticSphere.xyz;
    float radius = rt.causticSphere.w;
    float photons = float(rt.causticInfo.x);
    vec3 color = light.color.rgb * light.halfHeight.w;
    uint type = uint(light.position.w);
    if (type == 0u) {
        // A sun: through the sphere's disc, from rt.caustics.z before it,
        // within the sun's angle.
        dir = -ConeSample(light.position.xyz, cos(light.axis.w), u1);
        vec3 t, b;
        Basis(dir, t, b);
        float r = radius * sqrt(u0.x), phi = 2.0 * PI * u0.y;
        origin = c + (t * cos(phi) + b * sin(phi)) * r - dir * rt.caustics.z;
        flux = color * (PI * radius * radius / photons);
        return true;
    }
    // Intensity towards dir: radiance times projected area (ShadeHit's),
    // through the cone the sphere subtends from the light's point.
    float area;
    if (type == kLightSphere) {
        origin = light.position.xyz;
        area = PI * light.axis.w * light.axis.w;
    } else {
        vec3 hw = light.halfWidth.xyz, hh = light.halfHeight.xyz;
        origin = type == kLightRect ? light.position.xyz + (2.0 * u0.x - 1.0) * hw + (2.0 * u0.y - 1.0) * hh
                                    : light.position.xyz + sqrt(u0.x) * (cos(2.0 * PI * u0.y) * hw + sin(2.0 * PI * u0.y) * hh);
        area = (type == kLightRect ? 4.0 : PI) * length(cross(hw, hh));
    }
    vec3 toSphere = c - origin;
    float d = length(toSphere);
    float cosMax = d > radius ? sqrt(1.0 - radius * radius / (d * d)) : -1.0;
    dir = ConeSample(d > 0.0 ? toSphere / d : vec3(0.0, 0.0, 1.0), cosMax, u1);
    float cosLight = dot(dir, light.axis.xyz);
    float projected = type == kLightSphere ? area : area * cosLight;
    float theta = acos(clamp(cosLight, -1.0, 1.0));
    float cone = light.shaping.y <= light.shaping.x ? (theta <= light.shaping.y ? 1.0 : 0.0)
                                                     : 1.0 - smoothstep(light.shaping.x, light.shaping.y, theta);
    flux = color * (max(projected, 0.0) * cone * 2.0 * PI * (1.0 - cosMax) / photons);
    return projected > 0.0 && cone > 0.0;
}

void main()
{
    uint index = gl_LaunchIDEXT.x, sender = gl_LaunchIDEXT.y;
    Light light = lights[rt.causticLights[sender / 4u][sender % 4u]];
    uvec3 seed = uvec3(index, sender * 7919u + 1u, rt.params.x);
    vec3 origin, dir, flux;
    if (!EmitPhoton(light, Random2(seed, 0u), Random2(seed + uvec3(0u, 0u, 104729u), 0u), origin, dir, flux)) {
        return;
    }
    const uint kMaskAll = kRtMaskSurfaces | kRtMaskTranslucent;
    bool passed = false;  // through or off a caster
    bool inMedium = false;
    vec3 mediumColor = vec3(1.0);
    float mediumDepth = 0.0;
    for (uint bounce = 0u; bounce < kMaxPhotonBounces; ++bounce) {
        TraceMask(origin, dir, 0.0, kMaskAll);
        if (hit.t < 0.0) {
            return;
        }
        if (inMedium && mediumDepth > 0.0) {
            flux *= pow(max(mediumColor, vec3(1e-4)), vec3(hit.t / mediumDepth));
        }
        DrawInfo d = draws[hit.slot];
        bool receives = passed && (d.caustics & kCausticsReceive) != 0u;
        vec2 u = Random2(seed + uvec3(0u, 0u, 7907u * (bounce + 1u)), 0u);
        vec2 uh = Random2(seed + uvec3(0u, 0u, 15731u * (bounce + 1u)), 0u);
        if ((d.caustics & kCausticsCast) == 0u) {
            if (receives && (hit.flags & kHitTranslucent) == 0u) {
                StorePhoton(hit.position, flux, dir);
            }
            return;
        }
        vec3 v = -dir, t, b;
        if ((hit.flags & kHitTranslucent) != 0u) {
            Surface s = Evaluate(dir, 0.0);
            // Straight through, refracted, or its own diffuse surface, by
            // its fractions.
            if (u.x < s.straight) {
                origin = OffsetRay(hit.position, -s.ng);
                continue;
            }
            if (u.x >= s.straight + s.refracted) {
                if (receives) {
                    StorePhoton(hit.position, flux, dir);
                }
                return;
            }
            Material m = s.m;
            bool thin = m.refraction.y > 0.0;
            bool exiting = !thin && m.refraction.x != 1.0 && s.backface;
            float ior = max(m.refraction.x, 1e-3);
            float roughness = max(s.roughness, d.causticsRoughness);
            Basis(s.n, t, b);
            vec3 h = SampleGgxVndf(vec3(dot(v, t), dot(v, b), max(dot(v, s.n), 1e-4)), roughness * roughness, uh);
            vec3 n = normalize(t * h.x + b * h.y + s.n * h.z);
            vec3 r = thin ? dir : refract(dir, n, exiting ? ior : 1.0 / ior);
            bool total = dot(r, r) == 0.0;
            float f0 = (ior - 1.0) / (ior + 1.0);
            f0 *= f0;
            float cosine = exiting ? (total ? 0.0 : clamp(-dot(r, n), 0.0, 1.0)) : clamp(dot(n, v), 0.0, 1.0);
            float F = total ? 1.0 : f0 + (1.0 - f0) * pow(1.0 - cosine, 5.0);
            bool mirrored = total || u.y < F;
            if (!mirrored) {
                if (thin || (!exiting && m.refraction.z <= 0.0)) {
                    flux *= m.transmission.rgb;
                }
                if (!thin) {
                    inMedium = !exiting;
                    mediumColor = m.transmission.rgb;
                    mediumDepth = m.refraction.z;
                }
            }
            dir = mirrored ? reflect(dir, n) : r;
            origin = OffsetRay(hit.position, dot(dir, s.ng) > 0.0 ? s.ng : -s.ng);
        } else {
            // Opaque: its metal reflects, the rest is its diffuse surface.
            if (u.x >= hit.metallic) {
                if (receives) {
                    StorePhoton(hit.position, flux, dir);
                }
                return;
            }
            float roughness = max(hit.roughness, d.causticsRoughness);
            Basis(hit.normal, t, b);
            vec3 h = SampleGgxVndf(vec3(dot(v, t), dot(v, b), max(dot(v, hit.normal), 1e-4)), roughness * roughness, uh);
            vec3 n = normalize(t * h.x + b * h.y + hit.normal * h.z);
            dir = reflect(dir, n);
            if (dot(dir, hit.geometric) <= 0.0) {
                return;
            }
            flux *= hit.baseColor + (1.0 - hit.baseColor) * pow(1.0 - clamp(dot(n, v), 0.0, 1.0), 5.0);
            origin = OffsetRay(hit.position, hit.geometric);
            inMedium = false;
        }
        passed = true;
    }
}
