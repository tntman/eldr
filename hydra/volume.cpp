#include "volume.h"

#include "field.h"
#include "instancer.h"
#include "material.h"
#include "mesh.h"
#include "renderDelegate.h"
#include "scene.h"

#include "pxr/base/gf/vec3f.h"
#include "pxr/imaging/hd/material.h"
#include "pxr/imaging/hd/renderIndex.h"
#include "pxr/imaging/hd/sceneGlobalsSchema.h"
#include "pxr/imaging/hd/sceneDelegate.h"
#include "pxr/imaging/hd/tokens.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

TF_DEFINE_PRIVATE_TOKENS(
    _tokens,
    // MaterialX (pbrlib, stdlib) and UsdPreviewSurface nodes and inputs.
    (ND_volume)
    (ND_anisotropic_vdf)
    (ND_absorption_vdf)
    (ND_uniform_edf)
    (ND_blackbody)
    (vdf)
    (edf)
    (absorption)
    (scattering)
    (anisotropy)
    (color)
    (temperature)
    (geomprop)
    (varname)
    (in1)
    (in2)
    (in)
    (value)
    (emission)
    (kma_volume));

bool StartsWith(std::string const &s, const char *prefix)
{
    return s.rfind(prefix, 0) == 0;
}

// A value of a volume graph: a constant, or a function of one float
// field's value (its geompropvalue), as scalars splat to four components.
// Baked into a table per field over its range (vkr::VolumeShading::Factor).
struct Expr {
    std::string field;  // empty: a constant (f's argument is ignored)
    std::function<GfVec4f(float)> f;
};

GfVec4f Mul(GfVec4f const &a, GfVec4f const &b)
{
    return GfVec4f(a[0] * b[0], a[1] * b[1], a[2] * b[2], a[3] * b[3]);
}

GfVec4f Map(GfVec4f const &a, float (*op)(float))
{
    return GfVec4f(op(a[0]), op(a[1]), op(a[2]), op(a[3]));
}

GfVec4f Lerp(GfVec4f const &a, GfVec4f const &b, GfVec4f const &t)
{
    return a + Mul(b - a, t);
}

float Smoothstep(float low, float high, float x)  // mx_smoothstep_float
{
    if (x >= high) {
        return 1.0f;
    }
    if (x <= low) {
        return 0.0f;
    }
    const float t = (x - low) / (high - low);
    return t * t * (3.0f - 2.0f * t);
}

// Houdini's hcatmullrom (houdini/materialx/libraries/houdini) at t, its
// tension 0.5: the Catmull-Rom segment from k1 to k2.
GfVec4f CatmullRom(GfVec4f const k[4], float t)
{
    const float t2 = t * t, t3 = t2 * t;
    return 0.5f * ((2 * t2 - t3 - t) * k[0] + (3 * t3 - 5 * t2 + 2) * k[1] + (4 * t2 - 3 * t3 + t) * k[2] +
                   (t3 - t2) * k[3]);
}

// The functions of a field that volume graphs compute, MaterialX's stdlib
// nodes as its genglsl implementations and nodegraphs define them (math,
// remap, range, smoothstep, clamp, mix, conditionals, ramp, ramplr/tb,
// ramp4, splitlr/tb) and Houdini's ramps (hinvlinear, huniformramp,
// huniformcubic, hcatmullrom: what the MtlX Color/Float (Cubic) Ramp VOPs
// write; Karma's kma_rampconst). Evaluated on the CPU only, to bake
// tables, so there is no compiling; nodes with inputs from two fields
// aren't read (multiply nodes above them are, Coefficient).
class FieldGraph
{
public:
    explicit FieldGraph(HdMaterialNetwork2 const &net)
        : _net(net)
    {
    }

    // The node type it can't read (one of them), after Value failed.
    std::string const &Failed() const { return _failed; }

    // `node`'s value.
    std::optional<Expr> Value(HdMaterialNode2 const &node)
    {
        const std::string &id = node.nodeTypeId.GetString();
        auto in = [&](const char *name) { return Input(node, TfToken(name)); };
        if (StartsWith(id, "ND_geompropvalue_float") || id == "UsdPrimvarReader_float") {
            const VtValue *v = Find(node, TfToken(id[0] == 'N' ? "geomprop" : "varname"));
            std::string name = v && v->IsHolding<TfToken>() ? v->UncheckedGet<TfToken>().GetString()
                               : v && v->IsHolding<std::string>() ? v->UncheckedGet<std::string>()
                                                                  : std::string();
            return Expr{name, [](float x) { return GfVec4f(x); }};
        }
        if (StartsWith(id, "ND_constant_")) {
            return in("value");
        }
        if (StartsWith(id, "ND_convert_") || StartsWith(id, "ND_dot_")) {
            return in("in");  // scalars splat; colour3 to colour4 keeps the alpha it has
        }
        struct Binary {
            const char *prefix;
            GfVec4f (*op)(GfVec4f const &, GfVec4f const &);
        };
        static const Binary binary[] = {
            {"ND_add_", [](GfVec4f const &a, GfVec4f const &b) { return a + b; }},
            {"ND_subtract_", [](GfVec4f const &a, GfVec4f const &b) { return a - b; }},
            {"ND_multiply_", Mul},
            {"ND_divide_",
             [](GfVec4f const &a, GfVec4f const &b) {
                 return GfVec4f(a[0] / b[0], a[1] / b[1], a[2] / b[2], a[3] / b[3]);
             }},
            {"ND_power_",
             [](GfVec4f const &a, GfVec4f const &b) {
                 return GfVec4f(std::pow(a[0], b[0]), std::pow(a[1], b[1]), std::pow(a[2], b[2]),
                                std::pow(a[3], b[3]));
             }},
            {"ND_min_",
             [](GfVec4f const &a, GfVec4f const &b) {
                 return GfVec4f(std::min(a[0], b[0]), std::min(a[1], b[1]), std::min(a[2], b[2]),
                                std::min(a[3], b[3]));
             }},
            {"ND_max_",
             [](GfVec4f const &a, GfVec4f const &b) {
                 return GfVec4f(std::max(a[0], b[0]), std::max(a[1], b[1]), std::max(a[2], b[2]),
                                std::max(a[3], b[3]));
             }},
            {"ND_modulo_",
             [](GfVec4f const &a, GfVec4f const &b) {
                 GfVec4f r;
                 for (int c = 0; c < 4; ++c) {
                     r[c] = a[c] - b[c] * std::floor(a[c] / b[c]);
                 }
                 return r;
             }},
        };
        for (const Binary &b : binary) {
            if (StartsWith(id, b.prefix)) {
                return Combine({in("in1"), in("in2")}, [op = b.op](V const &v) { return op(v[0], v[1]); });
            }
        }
        struct Unary {
            const char *prefix;
            float (*op)(float);
        };
        static const Unary unary[] = {
            {"ND_absval_", [](float x) { return std::abs(x); }},
            {"ND_floor_", [](float x) { return std::floor(x); }},
            {"ND_ceil_", [](float x) { return std::ceil(x); }},
            {"ND_sign_", [](float x) { return float((x > 0) - (x < 0)); }},
            {"ND_sqrt_", [](float x) { return std::sqrt(x); }},
            {"ND_exp_", [](float x) { return std::exp(x); }},
            {"ND_ln_", [](float x) { return std::log(x); }},
        };
        for (const Unary &u : unary) {
            if (StartsWith(id, u.prefix)) {
                return Combine({in("in")}, [op = u.op](V const &v) { return Map(v[0], op); });
            }
        }
        if (StartsWith(id, "ND_invert_")) {
            return Combine({in("in"), in("amount")}, [](V const &v) { return v[1] - v[0]; });
        }
        if (StartsWith(id, "ND_clamp_")) {
            return Combine({in("in"), in("low"), in("high")}, [](V const &v) {
                GfVec4f r;
                for (int c = 0; c < 4; ++c) {
                    r[c] = std::min(std::max(v[0][c], v[1][c]), v[2][c]);
                }
                return r;
            });
        }
        if (StartsWith(id, "ND_mix_")) {
            return Combine({in("bg"), in("fg"), in("mix")}, [](V const &v) { return Lerp(v[0], v[1], v[2]); });
        }
        if (StartsWith(id, "ND_remap_")) {
            return Combine({in("in"), in("inlow"), in("inhigh"), in("outlow"), in("outhigh")}, [](V const &v) {
                return Remap(v[0], v[1], v[2], v[3], v[4]);
            });
        }
        if (StartsWith(id, "ND_range_")) {
            // NG_range: remap to [0, 1], gamma (sign kept), remap out, clamp.
            return Combine({in("in"), in("inlow"), in("inhigh"), in("gamma"), in("outlow"), in("outhigh"),
                            in("doclamp")},
                           [](V const &v) {
                               GfVec4f r = Remap(v[0], v[1], v[2], GfVec4f(0.0f), GfVec4f(1.0f));
                               for (int c = 0; c < 4; ++c) {
                                   r[c] = float((r[c] > 0) - (r[c] < 0)) * std::pow(std::abs(r[c]), 1.0f / v[3][c]);
                               }
                               r = Remap(r, GfVec4f(0.0f), GfVec4f(1.0f), v[4], v[5]);
                               for (int c = 0; c < 4 && v[6][0] != 0.0f; ++c) {
                                   r[c] = std::min(std::max(r[c], v[4][c]), v[5][c]);
                               }
                               return r;
                           });
        }
        if (StartsWith(id, "ND_smoothstep_")) {
            return Combine({in("in"), in("low"), in("high")}, [](V const &v) {
                GfVec4f r;
                for (int c = 0; c < 4; ++c) {
                    r[c] = Smoothstep(v[1][c], v[2][c], v[0][c]);
                }
                return r;
            });
        }
        const int compare = StartsWith(id, "ND_ifgreatereq_") ? 1 : StartsWith(id, "ND_ifgreater_") ? 0
                          : StartsWith(id, "ND_ifequal_")                                 ? 2 : -1;
        if (compare >= 0) {
            return Combine({in("value1"), in("value2"), in("in1"), in("in2")}, [compare](V const &v) {
                const float a = v[0][0], b = v[1][0];
                return (compare == 0 ? a > b : compare == 1 ? a >= b : a == b) ? v[2] : v[3];
            });
        }
        if (StartsWith(id, "ND_switch_")) {
            std::vector<std::optional<Expr>> ins{in("which")};
            for (int i = 1; i <= 10; ++i) {
                ins.push_back(Input(node, TfToken("in" + std::to_string(i))));
            }
            return Combine(std::move(ins), [](V const &v) {
                return v[1 + std::min(std::max(int(std::floor(v[0][0])), 0), 9)];
            });
        }
        if (StartsWith(id, "ND_extract_")) {
            return Combine({in("in"), in("index")},
                           [](V const &v) { return GfVec4f(v[0][std::min(std::max(int(v[1][0]), 0), 3)]); });
        }
        if (StartsWith(id, "ND_combine")) {
            return Combine({in("in1"), in("in2"), in("in3"), in("in4")},
                           [](V const &v) { return GfVec4f(v[0][0], v[1][0], v[2][0], v[3][0]); });
        }
        if (StartsWith(id, "ND_luminance_")) {
            return Combine({in("in"), in("lumacoeffs")}, [](V const &v) {
                const float l = v[0][0] * v[1][0] + v[0][1] * v[1][1] + v[0][2] * v[1][2];
                return GfVec4f(l, l, l, v[0][3]);
            });
        }
        // The 2D ramps on texcoord (a field through convert or combine).
        if (StartsWith(id, "ND_ramplr_") || StartsWith(id, "ND_ramptb_")) {
            const bool lr = id[7] == 'l';
            return Combine({in(lr ? "valuel" : "valueb"), in(lr ? "valuer" : "valuet"), in("texcoord")},
                           [lr](V const &v) {
                               return Lerp(v[0], v[1], GfVec4f(std::min(std::max(v[2][lr ? 0 : 1], 0.0f), 1.0f)));
                           });
        }
        if (StartsWith(id, "ND_splitlr_") || StartsWith(id, "ND_splittb_")) {
            // mx_aastep without derivatives (none along a march): a step.
            const bool lr = id[8] == 'l';
            return Combine({in(lr ? "valuel" : "valueb"), in(lr ? "valuer" : "valuet"), in("center"), in("texcoord")},
                           [lr](V const &v) { return v[3][lr ? 0 : 1] >= v[2][0] ? v[1] : v[0]; });
        }
        if (StartsWith(id, "ND_ramp4_")) {
            return Combine({in("valuetl"), in("valuetr"), in("valuebl"), in("valuebr"), in("texcoord")},
                           [](V const &v) {
                               const GfVec4f s(std::min(std::max(v[4][0], 0.0f), 1.0f));
                               const GfVec4f t(std::min(std::max(v[4][1], 0.0f), 1.0f));
                               return Lerp(Lerp(v[2], v[3], s), Lerp(v[0], v[1], s), t);
                           });
        }
        if (id == "ND_ramp") {
            return Ramp(node);
        }
        if (StartsWith(id, "ND_hinvlinear_") || StartsWith(id, "ND_huniformramp_") ||
            StartsWith(id, "ND_huniformcubic_")) {
            const bool keys = StartsWith(id, "ND_hinvlinear_"), cubic = StartsWith(id, "ND_huniformcubic_");
            std::vector<std::optional<Expr>> ins{in("ramp"), in("num")};
            for (int i = 0; i < 10; ++i) {
                ins.push_back(Input(node, TfToken((keys ? "key" : "value") + std::to_string(i))));
            }
            if (keys) {
                return Combine(std::move(ins), InvLinear);
            }
            return Combine(std::move(ins), cubic ? UniformCubic : UniformRamp);
        }
        if (StartsWith(id, "ND_hcatmullrom_")) {
            return Combine({in("t"), in("key0"), in("key1"), in("key2"), in("key3"), in("tension")}, [](V const &v) {
                const GfVec4f k[4] = {v[1], v[2], v[3], v[4]};
                return CatmullRom(k, v[0][0]) * (2.0f * v[5][0]);  // weights times the tension
            });
        }
        if (StartsWith(id, "kma_rampconst")) {
            return RampConst(node, id != "kma_rampconst_float");
        }
        _failed = id;
        return std::nullopt;
    }

    // Input `name` of `node`: the upstream node's value, else its parameter.
    std::optional<Expr> Input(HdMaterialNode2 const &node, TfToken const &name)
    {
        const auto c = node.inputConnections.find(name);
        if (c == node.inputConnections.end() || c->second.empty()) {
            const GfVec4f v = HdVkParameter(node, name);
            return Expr{std::string(), [v](float) { return v; }};
        }
        const auto it = _net.nodes.find(c->second.front().upstreamNode);
        if (it == _net.nodes.end()) {
            return Expr{std::string(), [](float) { return GfVec4f(0.0f); }};
        }
        if (++_depth > 64) {
            _failed = "a graph deeper than 64 nodes";
            return std::nullopt;
        }
        std::optional<Expr> e = Value(it->second);
        --_depth;
        return e;
    }

private:
    using V = std::vector<GfVec4f>;

    VtValue const *Find(HdMaterialNode2 const &node, TfToken const &name) const
    {
        const auto it = node.parameters.find(name);
        return it == node.parameters.end() ? nullptr : &it->second;
    }

    static GfVec4f Remap(GfVec4f const &x, GfVec4f const &inLow, GfVec4f const &inHigh, GfVec4f const &outLow,
                         GfVec4f const &outHigh)
    {
        GfVec4f r;
        for (int c = 0; c < 4; ++c) {
            r[c] = outLow[c] + (x[c] - inLow[c]) * (outHigh[c] - outLow[c]) / (inHigh[c] - inLow[c]);
        }
        return r;
    }

    // Inputs of one field (or constants) through `op`; a constant if all are.
    std::optional<Expr> Combine(std::vector<std::optional<Expr>> ins, std::function<GfVec4f(V const &)> op)
    {
        std::string field;
        for (const auto &e : ins) {
            if (!e) {
                return std::nullopt;
            }
            if (!e->field.empty() && !field.empty() && e->field != field) {
                _failed = "a node of two fields (" + field + ", " + e->field + ")";
                return std::nullopt;
            }
            field = e->field.empty() ? field : e->field;
        }
        std::vector<std::function<GfVec4f(float)>> fs;
        for (auto &e : ins) {
            fs.push_back(std::move(e->f));
        }
        auto f = [fs = std::move(fs), op = std::move(op)](float x) {
            V v;
            v.reserve(fs.size());
            for (const auto &g : fs) {
                v.push_back(g(x));
            }
            return op(v);
        };
        if (field.empty()) {
            const GfVec4f c = f(0.0f);
            return Expr{field, [c](float) { return c; }};
        }
        return Expr{field, std::move(f)};
    }

    // Houdini's hinvlinear: v = ramp, num, key0..key9 (program.glsl's kOpInvLinear).
    static GfVec4f InvLinear(V const &v)
    {
        const float a = v[0][0], d = v[1][0] - 1.0f;
        int i = 1;
        while (i < 9 && v[2 + i][0] < a) {
            ++i;
        }
        const float k0 = v[2 + i - 1][0], k1 = v[2 + i][0];
        const float lo = float(i - 1) / d, hi = i == 9 ? 1.0f : float(i) / d;
        return GfVec4f(k1 != k0 ? lo + (a - k0) * (hi - lo) / (k1 - k0) : lo);
    }

    // Houdini's huniformramp: ten values over clamp(num, 0, 10) evenly
    // spaced keys, at clamp(ramp, 0, 1), linearly (kOpUniformRamp).
    static GfVec4f UniformRamp(V const &v)
    {
        const float f = std::min(std::max(v[0][0], 0.0f), 1.0f) * (std::min(std::max(v[1][0], 0.0f), 10.0f) - 1.0f);
        const float k = std::floor(f);
        const int k0 = int(std::min(std::max(k, 0.0f), 9.0f)), k1 = int(std::min(std::max(k + 1.0f, 0.0f), 9.0f));
        return Lerp(v[2 + k0], v[2 + k1], GfVec4f(f - k));
    }

    // Houdini's huniformcubic (NG_huniformcubic): more than three values
    // are Catmull-Rom between value1 and value[num - 2] (the ends steer),
    // else linear over value0..value2.
    static GfVec4f UniformCubic(V const &v)
    {
        const float t = std::min(std::max(v[0][0], 0.0f), 1.0f), num = v[1][0];
        auto value = [&](int i) { return v[2 + std::min(std::max(i, 0), 9)]; };
        if (!(num > 3.0f)) {
            const float s = t * (num - 1.0f), i = std::floor(s);
            const int which = std::min(std::max(int(i), 0), 9);
            return which == 0 ? Lerp(value(0), value(1), GfVec4f(s - i))
                 : which == 1 ? Lerp(value(1), value(2), GfVec4f(s - i))
                 : which == 2 ? value(2) : GfVec4f(0.0f);
        }
        const float s = 1.0f + t * (num - 3.0f), i = std::floor(s);
        const int seg = std::max(int(i) - 1, 0);  // hcatmullrom1..7: values seg..seg + 3; past them 0
        if (seg > 6) {
            return GfVec4f(0.0f);
        }
        const GfVec4f k[4] = {value(seg), value(seg + 1), value(seg + 2), value(seg + 3)};
        return CatmullRom(k, s - i);
    }

    // MaterialX's ramp (NG_ramp, ramp_gradient): up to ten intervals and
    // colours, linear, smooth (smoothstep) or step, at texcoord's x (type
    // standard) or its radial, circular or box function.
    std::optional<Expr> Ramp(HdMaterialNode2 const &node)
    {
        std::vector<std::optional<Expr>> ins;
        for (const char *name : {"texcoord", "type", "interpolation", "num_intervals"}) {
            ins.push_back(Input(node, TfToken(name)));
        }
        for (int i = 1; i <= 10; ++i) {
            ins.push_back(Input(node, TfToken("interval" + std::to_string(i))));
            ins.push_back(Input(node, TfToken("color" + std::to_string(i))));
        }
        return Combine(std::move(ins), [](V const &v) {
            const float u = v[0][0] - 0.5f, w = v[0][1] - 0.5f;
            const int type = int(v[1][0]), interpolation = int(v[2][0]), count = int(v[3][0]);
            const float x = type == 1 ? std::atan2(u, w) / 6.28319f + 0.5f  // atan2(inx = y, iny = x)
                          : type == 2 ? std::sqrt(u * u + w * w) * 1.414f
                          : type == 3 ? (std::abs(u) > std::abs(w) ? 2 * std::abs(u) : 2 * std::abs(w))
                                      : v[0][0];
            auto interval = [&](int i) { return v[4 + 2 * (i - 1)][0]; };
            auto color = [&](int i) { return v[5 + 2 * (i - 1)]; };
            GfVec4f result = color(1);
            for (int i = 1; i < 10 && i < count; ++i) {
                const float lo = interval(i), hi = interval(i + 1);
                if (!(x > lo)) {
                    continue;
                }
                if (interpolation == 2) {
                    result = x >= hi ? color(i + 1) : color(i);
                } else {
                    const float t = interpolation == 1 ? Smoothstep(lo, hi, x)
                                  : hi > lo            ? (std::min(std::max(x, lo), hi) - lo) / (hi - lo) : 1.0f;
                    result = Lerp(color(i), color(i + 1), GfVec4f(t));
                }
            }
            return result;
        });
    }

    // Karma's kma_rampconst: keys, values and a basis per key (arrays, as
    // Houdini's ramp parameters), at input t: constant, linear, else
    // Catmull-Rom (the end keys repeated).
    // ponytail: bspline, bezier, hermite and monotone cubic bases are read as Catmull-Rom.
    std::optional<Expr> RampConst(HdMaterialNode2 const &node, bool color)
    {
        auto floats = [&](const char *name, std::vector<float> *out) {
            uint32_t n = 0;
            const VtValue *v = Find(node, TfToken(name));
            return v && HdVkToFloats(*v, out, &n) ? n : 0u;
        };
        std::vector<float> keys, values;
        floats(color ? "vkeys" : "fkeys", &keys);
        const uint32_t n = floats(color ? "vvalues" : "fvalues", &values);
        std::vector<int> basis;
        if (const VtValue *b = Find(node, TfToken(color ? "vbasis" : "fbasis"))) {
            if (b->IsHolding<VtArray<TfToken>>()) {
                for (const TfToken &t : b->UncheckedGet<VtArray<TfToken>>()) {
                    basis.push_back(t == "constant" ? 0 : t == "linear" ? 1 : 2);
                }
            }
        }
        const size_t count = n ? std::min(keys.size(), values.size() / n) : 0;
        std::vector<GfVec4f> points;
        for (size_t i = 0; i < count; ++i) {
            points.push_back(n == 1 ? GfVec4f(values[i]) : GfVec4f(values[i * n], values[i * n + 1], values[i * n + 2], 1));
        }
        basis.resize(count, 1);
        return Combine({Input(node, TfToken("t"))}, [keys, points, basis](V const &v) {
            const float t = v[0][0];
            if (points.empty()) {
                return GfVec4f(0.0f);
            }
            if (t <= keys.front() || points.size() == 1) {
                return points.front();
            }
            size_t i = 0;
            while (i + 2 < points.size() && t >= keys[i + 1]) {
                ++i;
            }
            if (t >= keys[i + 1]) {
                return points.back();
            }
            const float u = keys[i + 1] > keys[i] ? (t - keys[i]) / (keys[i + 1] - keys[i]) : 0.0f;
            if (basis[i] == 0) {
                return points[i];
            }
            if (basis[i] == 1) {
                return Lerp(points[i], points[i + 1], GfVec4f(u));
            }
            const GfVec4f k[4] = {points[i ? i - 1 : 0], points[i], points[i + 1],
                                  points[std::min(i + 2, points.size() - 1)]};
            return CatmullRom(k, u);
        });
    }

    HdMaterialNetwork2 const &_net;
    std::string _failed;
    int _depth = 0;
};

// A coefficient of a volume material: a constant, a field reader (MaterialX
// geompropvalue, UsdPrimvarReader: the field by its binding name, 1 per
// unit of it), or a product of those (multiply nodes; constant, convert and
// dot nodes pass through). A float field is the density, a vector or colour
// one the colour field. Any other node: `unread` names it.
struct Coefficient {
    GfVec3f value{1.0f};
    std::string density, color;
    std::vector<Expr> factors;  // other nodes: functions of a field each (FieldGraph), times the rest
    std::string unread;
};

Coefficient Read(HdMaterialNetwork2 const &net, HdMaterialNode2 const &node, TfToken const &input, float fallback)
{
    Coefficient c;
    c.value = GfVec3f(fallback);
    const auto conn = node.inputConnections.find(input);
    if (conn == node.inputConnections.end() || conn->second.empty()) {
        const auto p = node.parameters.find(input);
        if (p != node.parameters.end()) {
            if (p->second.IsHolding<GfVec3f>()) {
                c.value = p->second.UncheckedGet<GfVec3f>();
            } else if (p->second.IsHolding<float>()) {
                c.value = GfVec3f(p->second.UncheckedGet<float>());
            } else if (p->second.IsHolding<double>()) {
                c.value = GfVec3f(float(p->second.UncheckedGet<double>()));
            }
        }
        return c;
    }
    const auto it = net.nodes.find(conn->second.front().upstreamNode);
    if (it == net.nodes.end()) {
        return c;
    }
    const HdMaterialNode2 &up = it->second;
    const std::string &id = up.nodeTypeId.GetString();
    c.value = GfVec3f(1.0f);
    if (id.rfind("ND_geompropvalue_", 0) == 0 || id.rfind("UsdPrimvarReader_", 0) == 0) {
        const bool mtlx = id[0] == 'N';
        std::string name;
        const auto p = up.parameters.find(mtlx ? _tokens->geomprop : _tokens->varname);
        if (p != up.parameters.end()) {
            name = p->second.IsHolding<TfToken>() ? p->second.UncheckedGet<TfToken>().GetString()
                   : p->second.IsHolding<std::string>() ? p->second.UncheckedGet<std::string>() : name;
        }
        const bool scalar = id.find("_float") != std::string::npos;
        (scalar ? c.density : c.color) = name;
    } else if (id.rfind("ND_multiply_", 0) == 0) {
        // MaterialX's defaults: in1 0, in2 1.
        const Coefficient a = Read(net, up, _tokens->in1, 0.0f), b = Read(net, up, _tokens->in2, 1.0f);
        c.value = GfCompMult(a.value, b.value);
        c.density = a.density.empty() ? b.density : a.density;
        c.color = a.color.empty() ? b.color : a.color;
        c.factors = a.factors;
        c.factors.insert(c.factors.end(), b.factors.begin(), b.factors.end());
        c.unread = a.unread.empty() ? b.unread : a.unread;
    } else if (id.rfind("ND_constant_", 0) == 0) {
        c = Read(net, up, _tokens->value, 0.0f);
    } else if (id.rfind("ND_convert_", 0) == 0 || id.rfind("ND_dot_", 0) == 0) {
        c = Read(net, up, _tokens->in, 0.0f);  // a float splats to all channels
    } else {
        FieldGraph graph(net);
        if (const std::optional<Expr> e = graph.Value(up)) {
            if (e->field.empty()) {
                const GfVec4f v = e->f(0.0f);
                c.value = GfVec3f(v[0], v[1], v[2]);
            } else {
                c.factors.push_back(*e);
            }
        } else {
            c.unread = graph.Failed();
        }
    }
    return c;
}

// Adds `e` to the shading's factors and to `mask`.
void AddFactor(Expr const &e, vkr::VolumeShading *s, uint32_t *mask, std::string *unread)
{
    if (s->factors.size() == vkr::VolumeShading::kMaxFactors) {
        if (unread->empty()) {
            *unread = "more than three functions of fields (ramps, remaps)";
        }
        return;
    }
    s->factors.push_back({e.field, [f = e.f](float x) {
                              const GfVec4f v = f(x);
                              return std::array<float, 3>{v[0], v[1], v[2]};
                          }});
    *mask |= 1u << (s->factors.size() - 1);
}

// The factors multiplying coefficient `c`: its own, and the density field
// read as it is, or implicitly when nothing in `c` reads it.
uint32_t Factors(Coefficient const &c, vkr::VolumeShading *s, std::string *unread)
{
    uint32_t mask = 0;
    bool density = !c.density.empty();
    for (const Expr &e : c.factors) {
        AddFactor(e, s, &mask, unread);
        density = density || e.field == s->densityField;
    }
    return mask | (!c.density.empty() || !density ? vkr::VolumeShading::kDensity : 0u);
}

HdMaterialNode2 const *Upstream(HdMaterialNetwork2 const &net, HdMaterialNode2 const &node, TfToken const &input)
{
    const auto conn = node.inputConnections.find(input);
    if (conn == node.inputConnections.end() || conn->second.empty()) {
        return nullptr;
    }
    const auto it = net.nodes.find(conn->second.front().upstreamNode);
    return it == net.nodes.end() ? nullptr : &it->second;
}

void Note(std::string *unread, Coefficient const &c)
{
    if (unread->empty()) {
        *unread = c.unread;
    }
}

// The volume's absorption, scattering and anisotropy from `node`'s inputs
// (anisotropic_vdf, absorption_vdf and kma_volume name them alike).
void ReadMedium(HdMaterialNetwork2 const &net, HdMaterialNode2 const &node, bool scatters, vkr::VolumeShading *s,
                std::string *unread)
{
    const Coefficient absorption = Read(net, node, _tokens->absorption, 0.0f);
    const Coefficient scattering = scatters ? Read(net, node, _tokens->scattering, 0.0f) : Coefficient{};
    const Coefficient anisotropy = scatters ? Read(net, node, _tokens->anisotropy, 0.0f) : Coefficient{};
    for (int c = 0; c < 3; ++c) {
        s->absorption[c] = absorption.value[c];
        s->scattering[c] = scatters ? scattering.value[c] : 0.0f;
    }
    std::string field = !scattering.density.empty() ? scattering.density : absorption.density;
    // Without a plain read of a field, a ramp's or remap's field is the density.
    for (const Coefficient *c : {&scattering, &absorption}) {
        field = field.empty() && !c->factors.empty() ? c->factors.front().field : field;
    }
    if (!field.empty()) {
        s->densityField = field;
    }
    s->scatteringFactors = Factors(scattering, s, unread);
    s->absorptionFactors = Factors(absorption, s, unread);
    if (!anisotropy.factors.empty() && unread->empty()) {
        *unread = "an anisotropy varying with a field";
    }
    s->colorField = scattering.color;
    s->anisotropy = scatters ? anisotropy.value[0] : 0.0f;
    Note(unread, absorption);
    Note(unread, scattering);
    Note(unread, anisotropy);
}

// The emission of `node`'s `input` (uniform_edf's colour, kma_volume's
// emission): a constant per unit of density, a field other than the
// density times a colour, or blackbody(temperature field times Kelvin)
// times an intensity.
void ReadEmission(HdMaterialNetwork2 const &net, HdMaterialNode2 const &node, TfToken const &input,
                  vkr::VolumeShading *s, std::string *unread)
{
    const HdMaterialNode2 *blackbody = Upstream(net, node, input);
    float intensity = 1.0f;
    if (blackbody && blackbody->nodeTypeId.GetString().rfind("ND_multiply_", 0) == 0) {
        const HdMaterialNode2 *in1 = Upstream(net, *blackbody, _tokens->in1);
        const HdMaterialNode2 *in2 = Upstream(net, *blackbody, _tokens->in2);
        const bool first = in1 && in1->nodeTypeId == _tokens->ND_blackbody;
        if (first || (in2 && in2->nodeTypeId == _tokens->ND_blackbody)) {
            intensity = Read(net, *blackbody, first ? _tokens->in2 : _tokens->in1, 1.0f).value[0];
            blackbody = first ? in1 : in2;
        }
    }
    if (blackbody && blackbody->nodeTypeId == _tokens->ND_blackbody) {
        const Coefficient kelvin = Read(net, *blackbody, _tokens->temperature, 5000.0f);
        if (!kelvin.factors.empty() && unread->empty()) {
            *unread = "a blackbody of a ramp or remap";  // ponytail: blackbody isn't a FieldGraph node
        }
        s->temperatureField = kelvin.density;
        s->kelvinScale = kelvin.value[0];
        s->blackbodyIntensity = intensity;
        Note(unread, kelvin);
        if (s->temperatureField.empty()) {
            TF_WARN("HdVk: a volume's blackbody emission needs a temperature field");
        }
        return;
    }
    const Coefficient color = Read(net, node, input, 0.0f);
    Note(unread, color);
    if (!color.factors.empty()) {
        // Ramps and remaps of fields (a fire's temperature to colour):
        // per unit length, times the density only where it is read.
        uint32_t mask = 0;
        if (!color.density.empty() && color.density != s->densityField) {
            AddFactor(Expr{color.density, [](float x) { return GfVec4f(x); }}, s, &mask, unread);
        }
        for (const Expr &e : color.factors) {
            AddFactor(e, s, &mask, unread);
        }
        if (!color.color.empty() && unread->empty()) {
            *unread = "a colour field times a ramp or remap in emission";
        }
        s->emissionFactors = mask | (color.density == s->densityField ? vkr::VolumeShading::kDensity : 0u);
        for (int c = 0; c < 3; ++c) {
            s->emission[c] = color.value[c];
        }
        return;
    }
    const std::string &field = !color.color.empty() ? color.color : color.density;
    if (!field.empty() && field != s->densityField) {
        s->emissionField = field;  // radiance per unit length: the field times the colour
        for (int c = 0; c < 3; ++c) {
            s->emissionTint[c] = color.value[c];
        }
    } else {
        for (int c = 0; c < 3; ++c) {
            s->emission[c] = color.value[c];
        }
    }
}

} // namespace

// The volume terminal (MaterialX's, else Karma's kma context; Storm's
// volumes are glslfx code HdVk can't read): MaterialX's volume node, its
// vdf an anisotropic_vdf (absorption, scattering, anisotropy) or
// absorption_vdf, its edf a uniform_edf; or Karma's kma_volume (the same
// inputs and its emission). Coefficients are per unit of the density field
// (UsdVol: field:density), as MaterialX's are per unit length: constants,
// or a field reader times constants, which also names the density field
// (and a colour field scaling the scattering). Other nodes (remaps, ramps,
// math) are functions of one float field each (FieldGraph), times the
// rest: such a function of the density replaces it (a ramp of the density
// is the coefficient, not per unit of density), of another field it
// multiplies the coefficient (per unit of density as above). Emission: see
// ReadEmission. A graph with a node FieldGraph doesn't read isn't read at
// all: the volume renders as without a material (warned), never with a
// part of the graph.
// ponytail: no layered vdfs or Karma's Pyro Shader (its nodes), at most
// three field functions; Arnold's standard_volume and RenderMan's PxrVolume live in
// render contexts HdVk doesn't request (asking would switch their surfaces too).
void HdVkReadVolumeShading(HdMaterialNetwork2 const &net, vkr::VolumeShading *v)
{
    const auto terminal = net.terminals.find(HdMaterialTerminalTokens->volume);
    const auto it = terminal == net.terminals.end() ? net.nodes.end() : net.nodes.find(terminal->second.upstreamNode);
    if (it == net.nodes.end()) {
        return;
    }
    const HdMaterialNode2 &volume = it->second;
    vkr::VolumeShading s;  // MaterialX's defaults: no medium, no emission
    s.scattering[0] = s.scattering[1] = s.scattering[2] = 0.0f;
    s.absorption[0] = s.absorption[1] = s.absorption[2] = 0.0f;
    s.anisotropy = 0.0f;
    s.temperatureField = s.colorField = s.emissionField = "";
    s.physical = false;  // MaterialX's blackbody is luminance 1
    s.material = true;
    std::string unread;
    if (volume.nodeTypeId == _tokens->kma_volume) {
        ReadMedium(net, volume, true, &s, &unread);
        ReadEmission(net, volume, _tokens->emission, &s, &unread);
    } else if (volume.nodeTypeId == _tokens->ND_volume) {
        if (const HdMaterialNode2 *vdf = Upstream(net, volume, _tokens->vdf)) {
            const bool anisotropic = vdf->nodeTypeId == _tokens->ND_anisotropic_vdf;
            if (anisotropic || vdf->nodeTypeId == _tokens->ND_absorption_vdf) {
                ReadMedium(net, *vdf, anisotropic, &s, &unread);
            } else {
                unread = vdf->nodeTypeId.GetString();
            }
        }
        if (const HdMaterialNode2 *edf = Upstream(net, volume, _tokens->edf)) {
            if (edf->nodeTypeId == _tokens->ND_uniform_edf) {
                ReadEmission(net, *edf, _tokens->color, &s, &unread);
            } else if (unread.empty()) {
                unread = edf->nodeTypeId.GetString();
            }
        }
    } else {
        unread = volume.nodeTypeId.GetString();
    }
    if (!unread.empty()) {
        TF_WARN("HdVk: a volume material with %s renders as without one (volume graphs read constants, fields, "
                "their products, functions of one field each (math, remap, ramps) and blackbody)",
                unread.c_str());
        return;
    }
    *v = s;
}

HdVkVolume::HdVkVolume(SdfPath const &id, vkr::Scene *scene)
    : HdVolume(id)
    , _scene(scene)
    , _volume(scene->AddVolume())
{
}

HdVkVolume::~HdVkVolume()
{
    _scene->RemoveVolume(_volume);
}

HdDirtyBits HdVkVolume::GetInitialDirtyBitsMask() const
{
    return HdChangeTracker::InitRepr | HdChangeTracker::DirtyVolumeField | HdChangeTracker::DirtyTransform |
           HdChangeTracker::DirtyVisibility | HdChangeTracker::DirtyInstancer | HdChangeTracker::DirtyInstanceIndex |
           HdChangeTracker::DirtyMaterialId | HdChangeTracker::DirtyPrimvar;
}

void HdVkVolume::UpdateRenderTag(HdSceneDelegate *delegate, HdRenderParam *renderParam)
{
    HdVolume::UpdateRenderTag(delegate, renderParam);
    _volume->tag = HdVkRenderTagBit(GetRenderTag());
}

void HdVkVolume::Sync(HdSceneDelegate *sceneDelegate, HdRenderParam *, HdDirtyBits *dirtyBits, TfToken const &)
{
    HdVkRenderDelegate::NoteSceneEdit(sceneDelegate);
    const SdfPath &id = GetId();
    vkr::Volume &v = *_volume;
    // Fields sync before rprims (bprims first), so their grids are there.
    if (*dirtyBits & HdChangeTracker::DirtyVolumeField) {
        v.fields.clear();
        for (const HdVolumeFieldDescriptor &d : sceneDelegate->GetVolumeFieldDescriptors(id)) {
            // The field types the delegate makes HdVkFields of: openvdbAsset,
            // and houdiniFieldAsset in the Houdini build (no Field3D).
            if (!sceneDelegate->GetRenderIndex().IsBprimTypeSupported(d.fieldPrimType)) {
                continue;
            }
            if (const auto *field = static_cast<const HdVkField *>(
                    sceneDelegate->GetRenderIndex().GetBprim(d.fieldPrimType, d.fieldId))) {
                v.fields[d.fieldName.GetString()] = field->Grid();
            }
        }
    }
    // Materials sync before rprims too; the shading follows theirs in place.
    if (*dirtyBits & HdChangeTracker::DirtyMaterialId) {
        const auto *material = static_cast<const HdVkMaterial *>(sceneDelegate->GetRenderIndex().GetSprim(
            HdPrimTypeTokens->material, sceneDelegate->GetMaterialId(id)));
        v.shading = material ? material->Volume() : nullptr;
    }

    // As HdVkMesh: instancers are synced by the rprims that use them.
    _UpdateInstancer(sceneDelegate, dirtyBits);
    HdInstancer::_SyncInstancerAndParents(sceneDelegate->GetRenderIndex(), GetInstancerId());
    if (HdChangeTracker::IsTransformDirty(*dirtyBits, id) || HdChangeTracker::IsInstancerDirty(*dirtyBits, id) ||
        HdChangeTracker::IsInstanceIndexDirty(*dirtyBits, id)) {
        v.transforms = HdVkWorldTransforms(sceneDelegate, id, GetInstancerId());
    }
    if (HdChangeTracker::IsVisibilityDirty(*dirtyBits, id)) {
        _UpdateVisibility(sceneDelegate, dirtyBits);
        v.visible = IsVisible();
    }
    // Volume Limit (eldr:object:volumelimit, else karma:object:volumelimit).
    if (HdChangeTracker::IsAnyPrimvarDirty(*dirtyBits, id)) {
        v.volumeLimit = std::max(HdVkObjectPrimvar<float>(sceneDelegate, id, "object:volumelimit", 0.0f), 0.0f);
        // Velocity blur, as Karma's: Velocity Blur on (eldr:object:vblur,
        // else karma:object:vblur, above 0), motion blur on (object:mblur),
        // times Volume Velocity Blur Scale (object:volumevblurscale); the
        // field is per second, the renderer's shutter in frames.
        auto *vk = static_cast<HdVkRenderDelegate *>(sceneDelegate->GetRenderIndex().GetRenderDelegate());
        double fps = 24.0;
        if (HdSceneIndexBaseRefPtr si = sceneDelegate->GetRenderIndex().GetTerminalSceneIndex()) {
            if (const HdDoubleDataSourceHandle rate = HdSceneGlobalsSchema::GetFromSceneIndex(si).GetTimeCodesPerSecond()) {
                fps = rate->GetTypedValue(0.0f) > 0.0 ? rate->GetTypedValue(0.0f) : fps;
            }
        }
        const bool blur = vk->GetRenderSetting<bool>(HdVkRenderSettingsTokens2->volumeVelocityBlur, true) &&
                          HdVkObjectPrimvar<int>(sceneDelegate, id, "object:vblur", 0) > 0 &&
                          HdVkMotionBlur(sceneDelegate, id);
        v.velocityScale =
            blur ? float(HdVkObjectPrimvar<float>(sceneDelegate, id, "object:volumevblurscale", 1.0f) / fps) : 0.0f;
    }
    *dirtyBits &= ~HdChangeTracker::AllSceneDirtyBits;
}

PXR_NAMESPACE_CLOSE_SCOPE
