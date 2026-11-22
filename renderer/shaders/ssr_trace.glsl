// Screen-space reflection rays against the prepass depth (TraceReflection),
// for mesh.frag's shading and SsrDenoise's lower-resolution trace
// (ssr_trace.frag). Include after frame.glsl and the samplers sceneDepth,
// previousColor, previousSplits and ambientOcclusion.

vec3 Unproject(vec3 ndc)
{
    vec4 p = frame.invViewProj * vec4(ndc, 1.0);
    return p.xyz / p.w;
}

float SceneDepth(vec2 ndc)
{
    ivec2 size = textureSize(sceneDepth, 0);
    return texelFetch(sceneDepth, clamp(ivec2((ndc * 0.5 + 0.5) * vec2(size)), ivec2(0), size - 1), 0).r;
}

// Whether a ray at window depth z is behind the depth buffer's `scene`
// (EEVEE 5.2's precision fix): by more than 7 steps of the depth's
// precision (2^-24 near 1, D32F as 24-bit), so a ray leaving its surface
// towards the camera doesn't hit it again where the depths round alike,
// nor where fast math puts an equal depth a hair behind.
bool Behind(float z, float scene)
{
    return scene < 1.0 && z - scene > 7.0 / 16777216.0;
}

// The depth of the nearest opaque back face (Frame::ssrBackfaceDepth),
// window [0, 1]: the far side of a closed object seen at a pixel. Without
// it the prepass depth is bound: every surface open.
layout(set = 0, binding = 78) uniform sampler2D backDepth;

float BackDepth(vec2 ndc)
{
    ivec2 size = textureSize(backDepth, 0);
    return texelFetch(backDepth, clamp(ivec2((ndc * 0.5 + 0.5) * vec2(size)), ivec2(0), size - 1), 0).r;
}

// Whether the surface at `ndc` (window depth `scene`) has a back behind
// it: a closed object's, not an open surface's (a plane, a card).
bool Thick(vec2 ndc, float scene)
{
    float back = BackDepth(ndc);
    return back < 1.0 && Behind(back, scene);
}

float ssrHitDistance = -1.0;  // TraceReflection's last hit, world units

// The visible surface's normal at `ndc` (world point p) from the depth:
// per axis the shorter of the two steps of `pixels`, so silhouettes don't
// bend it; facing the camera. Wider steps smooth a mesh's facets.
vec3 DepthNormal(vec2 ndc, vec3 p, float pixels)
{
    vec3 t[2];
    for (int i = 0; i < 2; ++i) {
        vec2 o = pixels * (i == 0 ? vec2(2.0 / frame.viewport.x, 0.0) : vec2(0.0, 2.0 / frame.viewport.y));
        vec3 a = Unproject(vec3(ndc + o, SceneDepth(ndc + o) * 2.0 - 1.0)) - p;
        vec3 b = p - Unproject(vec3(ndc - o, SceneDepth(ndc - o) * 2.0 - 1.0));
        t[i] = dot(a, a) < dot(b, b) ? a : b;
    }
    vec3 n = normalize(cross(t[0], t[1]));
    return dot(n, ViewDirection(p)) < 0.0 ? -n : n;
}

// Screen-space reflection: marches the ray linearly through NDC (where
// it stays a straight line with affine depth) against the prepass depth,
// refines the first crossing by bisection and shades it with last frame's
// result reprojected to the hit. Returns the hit's weight (0 = miss) and
// its premultiplied radiance in `hit`.
// Without the denoiser (Frame::ssrDenoise): mirror rays only, faded out
// from roughness 0.3 to 0.6. `cone`: the tangent of the half angle of the
// cone of directions the ray stands for (Frame::ssrHitFilter).
float TraceReflection(vec3 p, vec3 r, float cone, out vec4 hit)
{
    int steps = int(frame.features.y);
    hit = vec4(0.0);
    // A quad's helper pixels (beside a triangle's edge, shaded only for
    // derivatives) show nothing and nothing after takes a derivative of
    // the hit: no march. Small triangles have more helpers than pixels
    // (the Kitchen: two thirds of the shading pass).
    if (gl_HelperInvocation) {
        return 0.0;
    }
    vec4 c0 = frame.viewProj * vec4(p, 1.0);
    vec4 dc = frame.viewProj * vec4(r, 0.0);
    // End far along the ray but in front of the near plane (z >= -w).
    float t = 1000.0 * max(length(p - frame.cameraPos.xyz), 1e-3);
    if (dc.z + dc.w < 0.0) {
        t = min(t, 0.99 * (c0.z + c0.w) / -(dc.z + dc.w));
    }
    vec4 c1 = c0 + dc * t;
    vec3 s0 = c0.xyz / c0.w, s1 = c1.xyz / c1.w;
    // Only the part of the segment that is on screen.
    vec2 d = s1.xy - s0.xy;
    float uEnd = 1.0;
    for (int i = 0; i < 2; ++i) {
        if (abs(d[i]) > 1e-8) {
            uEnd = min(uEnd, ((d[i] > 0.0 ? 1.0 : -1.0) - s0[i]) / d[i]);
        }
    }
    float pixels = length(d * uEnd * 0.5 * frame.viewport.xy);
    if (pixels < 2.0) {
        return 0.0;  // heads (almost) straight into or out of the screen
    }
    float uStart = 1.5 * uEnd / pixels;  // past the starting pixel
    float noise = SampleNoise(kDimSsr, gl_FragCoord.xy, frame.ssr.y);
    float uPrev = 0.0;
    vec3 sPrev = s0;
    bool wasBehind = false;
    // The thickness estimator (Frame::ssgiThickness): every step at least
    // a pixel on; jumps in depth between steps searched pixel by pixel.
    // ponytail: EEVEE also spreads the steps quadratically; here that cost
    // the crowd 1.4 ms for nothing the pixel search doesn't already catch.
    bool estimator = frame.screenTrace.x > 0.0;
    float prevScene = 1.0;
    // Pixels searched between steps, per ray: 4 per step (128 at the
    // default 32), so fewer steps cost less. A fixed 128 let the search
    // take over at few steps, whose gaps are wider: 8 steps were slower
    // than 32.
    int budget = 4 * steps;
    vec2 pixelNdc = 2.0 * normalize(d * frame.viewport.xy) / frame.viewport.xy;  // one pixel along the ray
    float lo = 0.0, hi = 0.0;  // the crossing's bracket
    bool passed = false;  // the ray went behind a surface it didn't hit
    bool under = false;   // it is inside an object, hidden behind its front
    bool crossed = false;
    for (int i = 0; i < steps && !crossed; ++i) {
        float u = mix(uStart, uEnd, (float(i) + noise) / float(steps));
        if (estimator) {
            u = max(u, uPrev + uEnd / pixels);
            if (u > uEnd) {
                break;
            }
        }
        vec3 s = mix(s0, s1, u);
        float z = s.z * 0.5 + 0.5;
        if (z >= 1.0) {
            break;  // beyond the far plane: only sky, which the dome provides
        }
        float scene = SceneDepth(s.xy);
        if (estimator && i > 0 && !wasBehind && !Behind(z, scene)) {
            // Across a jump in depth between two steps (an edge, the end
            // of a thin strip of surface seen edge-on) a thin feature can
            // hide in front of the ray: search the pixels in between.
            float a = 1.0 - prevScene, b = 1.0 - scene;  // ~ 1 / distance
            if ((prevScene >= 1.0) != (scene >= 1.0) || abs(a - b) > 0.1 * max(a, b)) {
                int between = min(int((u - uPrev) * pixels / uEnd), budget);
                budget -= between;
                for (int k = 1; k <= between; ++k) {
                    float uk = mix(uPrev, u, float(k) / float(between + 1));
                    vec3 sk = mix(s0, s1, uk);
                    float dk = SceneDepth(sk.xy);
                    if (Behind(sk.z * 0.5 + 0.5, dk)) {
                        u = uk;
                        s = sk;
                        z = sk.z * 0.5 + 0.5;
                        scene = dk;
                        break;
                    }
                    uPrev = uk;
                    sPrev = sk;
                }
            }
        }
        float lastScene = prevScene;
        prevScene = scene;
        // A step that goes from in front of the depth to behind it is
        // refined first and judged where it crosses: at grazing angles a
        // whole step can land far behind a surface it did pass through
        // (otherwise a miss that TAA's clipping keeps as grain).
        bool behind = Behind(z, scene);
        lo = uPrev;
        hi = u;
        if (behind) {
            float stepLength = length(Unproject(s) - Unproject(sPrev));
            if (!wasBehind) {
                // To within a pixel (at most 12 halvings): wider, a grazing
                // floor's depth changes too much across the bracket to read
                // as one continuous surface (misses on a chrome ball's
                // lower half).
                for (int k = 0; k < 12 && (hi - lo) * pixels > uEnd; ++k) {
                    float mid = 0.5 * (lo + hi);
                    vec3 m = mix(s0, s1, mid);
                    if (Behind(m.z * 0.5 + 0.5, SceneDepth(m.xy))) {
                        hi = mid;
                    } else {
                        lo = mid;
                    }
                }
            }
            vec3 c = mix(s0, s1, hi);
            vec3 ray = Unproject(c);
            vec3 surface = Unproject(vec3(c.xy, SceneDepth(c.xy) * 2.0 - 1.0));
            // At least the depth one pixel of this surface spans along the
            // ray (the nearer neighbour: not across an edge). A refined
            // crossing of one continuous surface (no jump in depth between
            // the refined steps around it) went through it: with the
            // thickness estimator a hit at any depth (grazing surfaces
            // aren't porous), without it up to 5% of the distance behind.
            // Across a jump (a silhouette), or on a step that stays behind,
            // the ray may pass behind a separate object: only the footprint
            // counts, so a ray from the floor beside a ball doesn't take
            // the ball's colour (columns hanging from its sides).
            // Sampled a pixel and a half away: a diagonal pixel's step
            // can stay in the same texel (no footprint: a grazing floor's
            // crossings read as jumps, misses on a chrome ball's lower half).
            vec2 o = 1.5 * pixelNdc;
            vec3 ahead = Unproject(vec3(c.xy + o, SceneDepth(c.xy + o) * 2.0 - 1.0));
            vec3 back = Unproject(vec3(c.xy - o, SceneDepth(c.xy - o) * 2.0 - 1.0));
            float footprint = min(length(ahead - surface), length(back - surface)) / 1.5;
            vec3 l = mix(s0, s1, lo);
            vec3 before = Unproject(vec3(l.xy, SceneDepth(l.xy) * 2.0 - 1.0));
            bool continuous = !wasBehind && length(before - surface) <= 2.0 * footprint + 1e-6 * length(surface - frame.cameraPos.xyz);
            float behindBy = length(ray - surface);
            crossed = estimator ? continuous || behindBy <= footprint
                                : continuous && behindBy <= max(2.0 * stepLength, 0.05 * length(surface - frame.cameraPos.xyz));
            // Across a silhouette the hit is hidden where the ray meets it:
            // like inside an object, the front a pixel on stands in, not
            // the edge's antialiased pixel (specks on a chrome ball's lower
            // half, which reflects the floor at its contact).
            if (estimator && crossed && !continuous) {
                under = true;
                lo = min(hi + uEnd / pixels, u);
            }
            // Inside an object (Frame::ssrBackfaceDepth): between the
            // depth of its front and of its back (the nearest back face)
            // at a pixel. A ray that goes behind a silhouette into an
            // object hits its hidden side; one beyond its back passes
            // behind it and goes on. Checked pixel by pixel from the
            // crossing (a pixel on: the silhouette's antialiased pixel is
            // part background) or the last step: such a ray is inside only
            // briefly. Open surfaces have no back here: one pixel thick.
            if (estimator && !crossed) {
                float from = wasBehind ? uPrev : min(hi + uEnd / pixels, u);
                int n = min(int((u - from) * pixels / uEnd), budget);
                budget -= n;
                for (int k = 0; k <= n; ++k) {
                    float uk = n == 0 ? from : mix(from, u, float(k) / float(n));
                    vec3 sk = mix(s0, s1, uk);
                    float zk = sk.z * 0.5 + 0.5, back = BackDepth(sk.xy), fk = SceneDepth(sk.xy);
                    if (back < 1.0 && zk <= back && Behind(zk, fk)) {
                        // More than two pixels' width behind the front: a
                        // ray grazing its own surface dips behind a pixel's
                        // depth within the pixel (bright specks on a chrome
                        // ball's lower half).
                        vec3 sf = Unproject(vec3(sk.xy, fk * 2.0 - 1.0));
                        float pixel = length(Unproject(vec3(sk.xy + 1.5 * pixelNdc, fk * 2.0 - 1.0)) - sf) / 1.5;
                        if (length(Unproject(sk) - sf) <= 2.0 * pixel) {
                            continue;
                        }
                        lo = uk;
                        under = true;
                        break;
                    }
                }
                if (under) {
                    break;
                }
            }
            if (wasBehind) {
                lo = hi;  // a step that stays behind: it lands on this pixel's surface, not the last one's
            }
            passed = passed || (!crossed && !wasBehind);
        }
        // From behind one surface to behind a different, open one (a jump
        // in depth; no back to pass behind), the ray went through the
        // second where it's hidden: the floor under a ball. Its visible
        // part next to there stands in.
        float a = 1.0 - lastScene, b = 1.0 - scene;  // ~ 1 / distance
        bool jump = (lastScene >= 1.0) != (scene >= 1.0) || abs(a - b) > 0.1 * max(a, b);
        if (estimator && wasBehind && !crossed && passed && behind && jump && scene < 1.0 && !Thick(s.xy, scene)) {
            crossed = under = true;  // lo = hi = u: this pixel's surface
            break;
        }
        wasBehind = behind;
        uPrev = u;
        sPrev = s;
    }
    // Rays that end behind something they didn't hit are misses: the
    // probe or dome shows, never a guessed surface (a column of it).
    // Inside an object (under), its front where the ray is found inside
    // stands in, lit like its front: not the silhouette's pixel where the
    // ray went in, whose antialiased colour is part background (streaks
    // of floor along the sides of a ball's reflection).
    if (!crossed && !under) {
        return 0.0;
    }
    for (int k = 0; k < 5 && !under; ++k) {
        float mid = 0.5 * (lo + hi);
        vec3 m = mix(s0, s1, mid);
        if (Behind(m.z * 0.5 + 0.5, SceneDepth(m.xy))) {
            hi = mid;
        } else {
            lo = mid;
        }
    }
    vec2 ndc = mix(s0.xy, s1.xy, under ? lo : hi);
    vec3 hitPos = Unproject(vec3(ndc, SceneDepth(ndc) * 2.0 - 1.0));
    ssrHitDistance = length(hitPos - p);
    vec4 prev = frame.prevViewProj * vec4(hitPos, 1.0);
    vec2 prevUv = prev.xy / prev.w * 0.5 + 0.5;
    if (prev.w <= 0.0 || any(lessThan(prevUv, vec2(0.0))) || any(greaterThan(prevUv, vec2(1.0)))) {
        return 0.0;  // not in last frame's image: a miss (the probe or dome)
    }
    // Footprint-filtered (Frame::ssrHitFilter, EEVEE 5.2): the
    // history's mip whose pixels are as wide as the ray's cone at
    // the hit, so a rough ray doesn't pick one sparkling pixel.
    float lod = 0.0, prevEdge = 1.0;
    if (frame.screenTrace.z > 0.0) {
        float hitNdcZ = SceneDepth(ndc) * 2.0 - 1.0;
        float pixel = length(Unproject(vec3(ndc.x + 2.0 / frame.viewport.x, ndc.y, hitNdcZ)) - hitPos);
        lod = clamp(log2(2.0 * ssrHitDistance * cone / max(pixel, 1e-20)), 0.0, 5.0);
        // Faded out at the edges of last frame's image too, where
        // the footprint reaches past it.
        vec2 e = 1.0 - abs(prevUv * 2.0 - 1.0);
        prevEdge = smoothstep(0.0, 0.1, min(e.x, e.y));
    }
    if (lod > 1.0) {
        // A coarse mip's texel is a square: a bright hit (the sun's
        // highlight on a ball) showed as a block. Four taps a mip finer on
        // a rotated grid over the footprint round it off.
        vec2 texel = exp2(lod - 1.0) / vec2(textureSize(previousColor, 0));
        hit = vec4(0.0);
        for (int k = 0; k < 4; ++k) {
            vec2 o = vec2(k == 0 ? 0.25 : k == 1 ? -0.25 : k == 2 ? 0.75 : -0.75,
                          k == 0 ? 0.75 : k == 1 ? -0.75 : k == 2 ? -0.25 : 0.25);
            hit += textureLod(previousColor, prevUv + o * texel, lod - 1.0);
        }
        hit *= 0.25;
    } else {
        hit = textureLod(previousColor, prevUv, lod);
    }
    if (frame.ssr.w > 0.0) {
        // Last frame's colour carries the light path intensities; its
        // splits take them out, so the reflection is scaled once (as
        // indirect glossy) like traced reflections.
        // ponytail: the splits at the hit, not over the mip's footprint
        // nor TAA's blend over frames.
        vec3 direct = textureLod(previousSplits, vec3(prevUv, 0.0), 0.0).rgb +
                      textureLod(previousSplits, vec3(prevUv, 2.0), 0.0).rgb;
        vec3 indirect = textureLod(previousSplits, vec3(prevUv, 1.0), 0.0).rgb +
                        textureLod(previousSplits, vec3(prevUv, 3.0), 0.0).rgb;
        float fogged = textureLod(previousSplits, vec3(prevUv, 0.0), 0.0).a;  // its fog's transmittance
        hit.rgb += fogged * ((1.0 - frame.lightPath.x) * direct + (1.0 - frame.lightPath.y) * indirect);
    }
    hit.rgb = BrightnessClamp(hit.rgb, frame.lightClamp.y);
    // Met from behind (the ray went behind an object): the light of
    // its unseen side is guessed as a share of the front's
    // (Frame::screenBackface).
    // A small tolerance: grazing hits (normal from depth across
    // the ray, rounding either way) count as front faces.
    if (!under && frame.screenTrace.y < 1.0 && dot(DepthNormal(ndc, hitPos, 1.0), r) > 1e-3) {
        hit.rgb *= frame.screenTrace.y;
    }
    // Inside an object the hidden side it hits faces away from the front
    // that stands in (a ball's underside, the floor beneath it): as
    // occluded as the front's ambient occlusion says, not lit like it.
    if (under) {
        ivec2 size = textureSize(ambientOcclusion, 0);
        hit.rgb *= texelFetch(ambientOcclusion, clamp(ivec2((ndc * 0.5 + 0.5) * vec2(size)), ivec2(0), size - 1), 0).r;
    }
    // Fade towards the screen edges, beyond which the ray is blind.
    vec2 edge = 1.0 - abs(ndc);
    return smoothstep(0.0, 0.1, min(edge.x, edge.y)) * prevEdge;
}
