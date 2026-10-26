// MaterialProgram interpreter (scene.h): the material inputs a shading
// graph computes per pixel. Included by mesh.frag's GRAPH variant after
// material.glsl and scene.glsl; reads its inUv, inColor, inSlot,
// inPrimvars, inWorldPos and inNormal. With PROGRAM_VERTEX (displace.glsl,
// in mesh.vert): a displacement graph per vertex, its inputs set by
// Displace, its offset in programDisplacement.
// ponytail: implicit texture derivatives inside the op loop are only
// defined where a pixel quad runs the same program (not at the edge
// between two materials).

#include "noise.glsl"

layout(std430, set = 0, binding = 28) readonly buffer ProgramCode { uvec4 programCode[]; };
layout(std430, set = 0, binding = 29) readonly buffer ProgramConstants { vec4 programConstants[]; };
// Per draw, per primvar slot: rate (0 missing, 1 constant, 2 uniform,
// 3 instance, 4 vertex), components, first value, first vertex component.
layout(std430, set = 0, binding = 30) readonly buffer PrimvarTable { uvec4 primvarTable[]; };
layout(std430, set = 0, binding = 31) readonly buffer PrimvarData { float primvarData[]; };

// MaterialProgram::Op.
const uint kOpOutput = 0u, kOpTexcoord = 1u, kOpDisplayColor = 2u, kOpPrimvar = 3u, kOpImage = 4u,
           kOpAdd = 5u, kOpSubtract = 6u, kOpMultiply = 7u, kOpDivide = 8u, kOpMin = 9u, kOpMax = 10u,
           kOpPower = 11u, kOpModulo = 12u, kOpAbs = 13u, kOpFloor = 14u, kOpCeil = 15u, kOpSin = 16u,
           kOpCos = 17u, kOpSqrt = 18u, kOpExp = 19u, kOpLog = 20u, kOpInvert = 21u, kOpMix = 22u,
           kOpOverlay = 23u, kOpClamp = 24u, kOpSmoothstep = 25u, kOpRemap = 26u, kOpExtract = 27u,
           kOpCombine = 28u, kOpDot = 29u, kOpNormalize = 30u, kOpLuminance = 31u, kOpIfGreater = 32u,
           kOpIfGreaterEq = 33u, kOpIfEqual = 34u, kOpHsvAdjust = 35u, kOpRgbToHsv = 36u, kOpHsvToRgb = 37u,
           kOpAffine = 38u, kOpNormalMap = 39u, kOpInvLinear = 40u, kOpUniformRamp = 41u;
const uint kOpNoise = 42u, kOpFractal = 43u, kOpCellNoise = 44u, kOpWorleyNoise = 45u, kOpPosition = 46u,
           kOpGeomNormal = 47u;
const uint kOpConvert = 48u;
const uint kProgramRegisters = 8u;
const uint kFeatureProcedurals = 256u;  // frame.features.x: Procedural Textures on
// Pipelines for programs that run noise (Scene::Gpu::kProcedural); in the
// others the noise code compiles away and costs nothing.
layout(constant_id = 8) const bool kProcedurals = false;

vec4 programRegisters[kProgramRegisters];

#ifdef LAYERS
// The lobe inputs a program computes (MaterialProgram::Input from
// kSpecularColor on, in that order), for brdf.glsl's PrepareLayers: their
// values, and a bit each in programLobes.
const uint kProgramLobes = 22u;
vec4 programLobe[kProgramLobes];
uint programLobes = 0u;
#endif

#ifdef LAYERS
// `m` with the lobe inputs the program computed in place of its constants,
// where frame.styles.x's Textured Lobe (4) and Transmission (8) Inputs are
// on (brdf.glsl's ShadingMaterial; rt_refraction.rgen at ray hits). Call
// after RunProgram.
Material ProgramMaterial(Material m)
{
    uint lobes = (frame.styles.x & 4u) != 0u ? programLobes : 0u;
    if (lobes != 0u) {
        m.specularColor.rgb = (lobes & 1u) != 0u ? programLobe[0].rgb : m.specularColor.rgb;
        m.coat.x = (lobes & 2u) != 0u ? programLobe[1].x : m.coat.x;
        m.coat.y = (lobes & 4u) != 0u ? programLobe[2].x : m.coat.y;
        m.coatColor.rgb = (lobes & 8u) != 0u ? programLobe[3].rgb : m.coatColor.rgb;
        m.sheen.x = (lobes & 16u) != 0u ? programLobe[4].x : m.sheen.x;
        m.sheenColor.rgb = (lobes & 32u) != 0u ? programLobe[5].rgb : m.sheenColor.rgb;
        m.sheen.y = (lobes & 64u) != 0u ? programLobe[6].x : m.sheen.y;
        m.anisotropy.x = (lobes & 128u) != 0u ? programLobe[7].x : m.anisotropy.x;
        m.thinFilm.y = (lobes & 256u) != 0u ? programLobe[8].x : m.thinFilm.y;
        float r = (programLobe[9].x - 1.0) / (programLobe[9].x + 1.0);  // the coat's F0 from its IOR
        m.coat.z = (lobes & 512u) != 0u ? r * r : m.coat.z;
        m.thinFilm.x = (lobes & 1024u) != 0u ? programLobe[10].x : m.thinFilm.x;
        m.thinFilm.z = (lobes & 2048u) != 0u ? programLobe[11].x : m.thinFilm.z;
        m.anisotropy.y = (lobes & 4096u) != 0u ? programLobe[12].x : m.anisotropy.y;
        m.refraction.x = (lobes & 8192u) != 0u ? programLobe[13].x : m.refraction.x;
    }
    // Transmission and subsurface (transparency.glsl, subsurface.glsl):
    // the radius and its scale multiply the constant (their product).
    uint passes = (frame.styles.x & 8u) != 0u ? programLobes : 0u;
    if ((passes & 4177920u) != 0u) {
        m.transmission.w = (passes & 16384u) != 0u ? programLobe[14].x : m.transmission.w;
        m.transmission.rgb = (passes & 32768u) != 0u ? programLobe[15].rgb : m.transmission.rgb;
        m.subsurface.w = (passes & 65536u) != 0u ? programLobe[16].x : m.subsurface.w;
        m.subsurface.rgb = (passes & 131072u) != 0u ? programLobe[17].rgb : m.subsurface.rgb;
        m.subsurfaceRadius.xyz *= (passes & 262144u) != 0u ? programLobe[18].xyz : vec3(1.0);
        m.refraction.z = (passes & 524288u) != 0u ? programLobe[19].x : m.refraction.z;
        m.refraction.y = (passes & 1048576u) != 0u ? (programLobe[20].x > 0.5 ? 1.0 : 0.0) : m.refraction.y;
        m.subsurfaceRadius.xyz *= (passes & 2097152u) != 0u ? programLobe[21].xyz : vec3(1.0);
    }
    return m;
}
#endif

vec4 ProgramArg(uint b, uint constants)
{
    return b < kProgramRegisters ? programRegisters[b] : programConstants[constants + b - kProgramRegisters];
}

vec4 ReadPrimvar(uint slot, vec4 fallback)
{
    uint table = draws[instances[inSlot].draw].primvars;
    if (table == kFreeSlot) {
        return fallback;
    }
    uvec4 e = primvarTable[table + slot];
    if (e.x == 0u) {
        return fallback;
    }
    vec4 v = vec4(0.0);
    if (e.x == 4u) {
        for (uint i = 0u; i < e.y; ++i) {
            v[i] = inPrimvars[e.w + i];
        }
    } else {
#ifdef PROGRAM_VERTEX
        uint element = e.x == 3u ? uint(instances[inSlot].index) : 0u;  // ponytail: uniform reads face 0
#else
        uint element = e.x == 2u ? uint(gl_PrimitiveID) : e.x == 3u ? uint(instances[inSlot].index) : 0u;
#endif
        for (uint i = 0u; i < e.y; ++i) {
            v[i] = primvarData[e.z + element * e.y + i];
        }
    }
    return e.y == 1u ? v.xxxx : v;
}

vec3 RgbToHsv(vec3 c)
{
    float hi = max(c.r, max(c.g, c.b)), d = hi - min(c.r, min(c.g, c.b));
    float h = 0.0;
    if (d > 0.0) {
        h = c.r >= hi ? (c.g - c.b) / d : c.g >= hi ? 2.0 + (c.b - c.r) / d : 4.0 + (c.r - c.g) / d;
        h = fract(h / 6.0);
    }
    return vec3(h, hi > 0.0 ? d / hi : 0.0, hi);
}

vec3 HsvToRgb(vec3 hsv)
{
    vec3 k = clamp(abs(mod(6.0 * fract(hsv.x) + vec3(0.0, 4.0, 2.0), 6.0) - 3.0) - 1.0, 0.0, 1.0);
    return hsv.z * mix(vec3(1.0), k, clamp(hsv.y, 0.0, 1.0));
}

// The surface point (or its interpolated normal), in object space (space
// 0) or world space: object space is the instance's (a skinned mesh's
// posed points, as Storm's points primvar).
// ponytail: meshes without normals use the facet's normal, facing the
// camera.
#ifdef PROGRAM_VERTEX
// Per vertex: the rest point and normal (programPosition, programNormal)
// and the instance's transform (programModel).
vec3 Geometry(bool normal, uint space)
{
    vec3 v = normal ? programNormal : programPosition;
    if (space == 0u) {
        return normal && dot(v, v) > 1e-12 ? normalize(v) : v;
    }
    return normal ? normalize(transpose(inverse(mat3(programModel))) * v) : (programModel * vec4(v, 1.0)).xyz;
}
#else
vec3 Geometry(bool normal, uint space)
{
    vec3 n = dot(inNormal, inNormal) > 1e-12 ? inNormal : cross(dFdx(inWorldPos), dFdy(inWorldPos));
    n = dot(inNormal, inNormal) > 1e-12 || dot(n, frame.cameraPos.xyz - inWorldPos) >= 0.0 ? n : -n;
    if (space != 0u) {
        return normal ? normalize(n) : inWorldPos;
    }
    mat4 m = InstanceTransform(instances[inSlot]);
    return normal ? normalize(transpose(mat3(m)) * n) : inverse(mat3(m)) * (inWorldPos - m[3].xyz);
}
#endif

// A tangent-space vector in world space, by the frame mesh.frag's
// CotangentFrame builds (derivatives: not in the vertex or ray stages,
// where it stays as it is).
vec3 TangentToWorld(vec3 t)
{
#if defined(PROGRAM_VERTEX) || defined(RAY_TRACING)
    return t;
#else
    vec3 dp1 = dFdx(inWorldPos), dp2 = dFdy(inWorldPos);
    vec3 n = dot(inNormal, inNormal) > 1e-12 ? normalize(inNormal) : normalize(cross(dp1, dp2));
    vec2 duv1 = dFdx(inUv), duv2 = dFdy(inUv);
    vec3 dp2perp = cross(dp2, n), dp1perp = cross(n, dp1);
    vec3 tu = dp2perp * duv1.x + dp1perp * duv2.x, tv = dp2perp * duv1.y + dp1perp * duv2.y;
    float invmax = inversesqrt(max(max(dot(tu, tu), dot(tv, tv)), 1e-20));
    return normalize(mat3(tu * invmax, tv * invmax, n) * t);
#endif
}

// v as a value of `components` (1: x splat; vector2 and vector3 zero the rest).
vec4 Components(vec4 v, uint components)
{
    return components == 1u ? v.xxxx : components == 2u ? vec4(v.xy, 0.0, 0.0) : components == 3u ? vec4(v.xyz, 0.0) : v;
}

// kOpNoise .. kOpGeomNormal (scene.h has their operands), only in the
// kProcedurals pipelines.
vec4 ProceduralOp(uint code, uint imm0, uint imm1, vec4 a, vec4 b, vec4 x)
{
    if (code >= kOpPosition) {
        return vec4(Geometry(code == kOpGeomNormal, imm0), 0.0);
    }
    // Procedural Textures off: the noise reads 0.
    vec4 v = vec4(0.0);
    if ((frame.features.x & kFeatureProcedurals) == 0u) {
    } else if (code == kOpNoise || code == kOpFractal) {
        bool fractal = code == kOpFractal;
        v = MxLayered(a.xyz, imm0, imm1, fractal, fractal ? int(x.x) : 1, x.y, x.z);
    } else {
        v = code == kOpCellNoise ? vec4(MxCellNoise(a.xyz, imm0)) : MxWorley(a.xyz, imm0, imm1, b.x, int(x.x));
    }
    return Components(code == kOpNoise ? v * b + x.x : code == kOpFractal ? v * b : v, imm1);
}

// Element i of a float array packed four to a vec4.
float PackedFloat(uint base, uint i)
{
    return programConstants[base + i / 4u][i % 4u];
}

// Runs `program` (GpuMaterial::program) and overwrites the inputs it outputs.
void RunProgram(uvec4 program, inout vec3 baseColor, inout vec3 emissive, inout float metallic,
                inout float roughness, inout float occlusion, inout vec3 tangentNormal, inout bool hasNormalMap,
                inout float alpha)
{
    uint c = program.z;
    for (uint i = 0u; i < program.y; ++i) {
        uvec4 op = programCode[program.x + i];
        uint code = op.x & 0xFFu, dst = (op.x >> 8) & 0xFFu, imm1 = op.x >> 16, imm0 = op.z;
        vec4 a = ProgramArg(op.y & 0xFFu, c), b = ProgramArg((op.y >> 8) & 0xFFu, c);
        vec4 x = ProgramArg((op.y >> 16) & 0xFFu, c), y = ProgramArg(op.y >> 24, c);
        vec4 r = vec4(0.0);
        switch (code) {
        case kOpOutput:
            if (imm0 == 0u) {
                baseColor = a.rgb;
            } else if (imm0 == 1u) {
                emissive = a.rgb;
            } else if (imm0 == 2u) {
                metallic = a.x;
            } else if (imm0 == 3u) {
                roughness = a.x;
            } else if (imm0 == 4u) {
                occlusion = clamp(a.x, 0.0, 1.0);
            } else if (imm0 == 5u) {
                tangentNormal = a.xyz;
                hasNormalMap = true;
            } else if (imm0 == 6u) {
                alpha = clamp(a.x, 0.0, 1.0);
#ifdef PROGRAM_VERTEX
            } else if (imm0 == 29u) {  // MaterialProgram::kDisplacement
                programDisplacement = a.x;
#endif
#ifdef LAYERS
            } else if (imm0 - 7u < kProgramLobes) {
                programLobe[imm0 - 7u] = a;
                programLobes |= 1u << (imm0 - 7u);
#endif
            }
            continue;
        case kOpTexcoord: r = vec4(inUv, 0.0, 0.0); break;
        case kOpDisplayColor: r = vec4(inColor, 1.0); break;
        case kOpPrimvar: r = ReadPrimvar(imm0, a); break;
        case kOpImage: {
            uint t = program.w + 5u * imm0;
            vec4 h = programConstants[t];
            TexRef ref = TexRef(floatBitsToInt(h.x), floatBitsToInt(h.y), floatBitsToInt(h.z), floatBitsToInt(h.w),
                                programConstants[t + 1u], programConstants[t + 2u], programConstants[t + 3u],
                                programConstants[t + 4u]);
            r = Fetch(ref, b, a.xy);
            break;
        }
        case kOpAdd: r = a + b; break;
        case kOpSubtract: r = a - b; break;
        case kOpMultiply: r = a * b; break;
        case kOpDivide: r = a / mix(b, vec4(1e-8), equal(b, vec4(0.0))); break;
        case kOpMin: r = min(a, b); break;
        case kOpMax: r = max(a, b); break;
        case kOpPower: r = pow(max(a, vec4(0.0)), b); break;
        case kOpModulo: r = mod(a, b); break;
        case kOpAbs: r = abs(a); break;
        case kOpFloor: r = floor(a); break;
        case kOpCeil: r = ceil(a); break;
        case kOpSin: r = sin(a); break;
        case kOpCos: r = cos(a); break;
        case kOpSqrt: r = sqrt(max(a, vec4(0.0))); break;
        case kOpExp: r = exp(a); break;
        case kOpLog: r = log(max(a, vec4(1e-30))); break;
        case kOpInvert: r = b - a; break;
        case kOpMix: r = mix(a, b, x); break;
        case kOpOverlay: {
            // a = fg, b = bg (ND_overlay's nodegraph), mixed over bg by x.
            vec4 over = mix(2.0 * a * b, 1.0 - 2.0 * (1.0 - a) * (1.0 - b), greaterThanEqual(b, vec4(0.5)));
            r = mix(b, over, x);
            break;
        }
        case kOpClamp: r = clamp(a, b, x); break;
        case kOpSmoothstep: r = smoothstep(b, x, a); break;
        case kOpRemap: {
            vec4 outHigh = programConstants[c + imm0];
            r = y + (a - b) / mix(x - b, vec4(1e-8), equal(x, b)) * (outHigh - y);
            break;
        }
        case kOpExtract: r = vec4(a[min(imm1, 3u)]); break;
        case kOpCombine:
            if (imm1 == 0u) {
                r = vec4(a.x, b.x, x.x, y.x);
            } else {
                // Each input contributes its first (imm1 >> 3 i) & 7 components.
                vec4 inputs[4] = vec4[4](a, b, x, y);
                uint n = 0u;
                for (uint k = 0u; k < 4u; ++k) {
                    for (uint j = 0u; j < ((imm1 >> (3u * k)) & 7u) && n < 4u; ++j) {
                        r[n++] = inputs[k][j];
                    }
                }
            }
            break;
        case kOpDot: r = vec4(imm1 == 4u ? dot(a, b) : imm1 == 2u ? dot(a.xy, b.xy) : dot(a.xyz, b.xyz)); break;
        case kOpNormalize: r = imm1 == 4u ? normalize(a) : imm1 == 2u ? vec4(normalize(a.xy), 0.0, 0.0)
                                                                      : vec4(normalize(a.xyz), 0.0); break;
        case kOpLuminance: r = vec4(dot(a.rgb, b.rgb)); break;
        case kOpConvert: {
            // MaterialX convert from imm0 to imm1 components: the common ones
            // kept, the rest 0, a 4th (alpha) 1 where there was none.
            uint n = min(imm0, imm1);
            r = vec4(n > 0u ? a.x : 0.0, n > 1u ? a.y : 0.0, n > 2u ? a.z : 0.0,
                     n > 3u ? a.w : (imm1 == 4u && imm0 < 4u ? 1.0 : 0.0));
            break;
        }
        case kOpIfGreater: r = a.x > b.x ? x : y; break;
        case kOpIfGreaterEq: r = a.x >= b.x ? x : y; break;
        case kOpIfEqual: r = a.x == b.x ? x : y; break;
        case kOpHsvAdjust: {
            vec3 hsv = RgbToHsv(a.rgb);
            r = vec4(HsvToRgb(vec3(hsv.x + b.x, hsv.y * b.y, hsv.z * b.z)), a.w);
            break;
        }
        case kOpRgbToHsv: r = vec4(RgbToHsv(a.rgb), a.w); break;
        case kOpHsvToRgb: r = vec4(HsvToRgb(a.rgb), a.w); break;
        case kOpAffine: r = vec4(dot(b.xyz, vec3(a.xy, 1.0)), dot(x.xyz, vec3(a.xy, 1.0)), 0.0, 0.0); break;
        case kOpNormalMap: {
            // MaterialX normalmap: a texel, xy scaled by b (a float's splat, or a vector2).
            vec3 n = dot(a.xyz, a.xyz) == 0.0 ? vec3(0.0, 0.0, 1.0) : a.xyz * 2.0 - 1.0;
            r = vec4(normalize(vec3(n.xy * b.xy, n.z)), 0.0);
            if (imm0 == 1u) {  // not the surface normal: MaterialX's world-space result
                r.xyz = TangentToWorld(r.xyz);
            }
            break;
        }
        case kOpInvLinear: {
            // Houdini's hinvlinear (houdini/materialx/libraries/houdini): the
            // first of key1..key8 at or above a ends the segment, else
            // key8..key9; a is remapped (unclamped) from it onto
            // [(i - 1) / (num - 1), i / (num - 1)], the last onto
            // [8 / (num - 1), 1]. num isn't clamped here, unlike huniformramp.
            uint base = c + imm0, i = 1u;
            while (i < 9u && PackedFloat(base, i) < a.x) {
                ++i;
            }
            float k0 = PackedFloat(base, i - 1u), k1 = PackedFloat(base, i), d = b.x - 1.0;
            float lo = float(i - 1u) / d, hi = i == 9u ? 1.0 : float(i) / d;
            r = vec4(k1 != k0 ? lo + (a.x - k0) * (hi - lo) / (k1 - k0) : lo);
            break;
        }
        case kOpUniformRamp: {
            // Houdini's huniformramp: ten values over clamp(num, 0, 10)
            // evenly spaced keys, at clamp(a, 0, 1), linearly.
            float f = clamp(a.x, 0.0, 1.0) * (clamp(b.x, 0.0, 10.0) - 1.0), k = floor(f);
            uint base = c + imm0, k0 = uint(clamp(k, 0.0, 9.0)), k1 = uint(clamp(k + 1.0, 0.0, 9.0));
            r = mix(programConstants[base + k0], programConstants[base + k1], f - k);
            break;
        }
        default:
#ifdef PROGRAM_VERTEX
            r = ProceduralOp(code, imm0, imm1, a, b, x);  // displaced pipelines only (kDisplacement)
#else
            if (kProcedurals) {
                r = ProceduralOp(code, imm0, imm1, a, b, x);
            }
#endif
            break;
        }
        programRegisters[dst] = r;
    }
}
