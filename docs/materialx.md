# MaterialX coverage

What Eldr compiles of real MaterialX, measured on a corpus: the MaterialX TestSuite and Examples that ship with USD, and Houdini 22's `basic_materials` / `quick_materials`. `tests/mtlx_corpus.py` renders each material on a sphere with Eldr and Karma CPU (husk), optionally Storm (MaterialX's own GLSL), and lists the nodes Eldr can't compile.

Run: `usdenv.cmd python tests\mtlx_corpus.py --out <dir> [--storm] [--only stdlib_math,...] [--skip Light_Bulb] [--refs <earlier out>]` (`--refs`: render Eldr only, compare with that run's Karma and Storm images). It writes `report.json`, the node table below on stdout, and `sheet_<group>.png` (Eldr | Karma per sphere).

TestSuite files mostly hold bare nodegraphs. The harness wraps each unused output as standard_surface emission (unlit, so pixels are the graph's value) and BSDF/EDF outputs through `surface`. Matrix, string and displacement-only outputs are skipped. Storm referees the unlit graphs only; under the harness's lights it renders lit surfaces black.

## Lane 38 (2026-10-29): compositing, glTF / Disney / unlit, Karma's VEX nodes

Against Karma CPU (husk, fresh renders for Houdini; the 10-27 Karma renders for the rest):

| Group | Materials | dE median before | dE median now | Notes |
|---|---|---|---|---|
| Houdini basic + quick | 121 | 4.9 (10-27) | 1.7 | 80 under 2, 104 under 5; no node Eldr warns about. The 38 with `kma_rampconst`, `kma_color_hextiling` or `kma_voronoinoise3d`: 6.9 -> 1.9 (other lanes' fixes count too). 103 compile whole by the audit's static rule, which doesn't follow `kma_camerafalloff`'s graph (Eldr expands it) |
| Examples glTF PBR | 6 | 26.9 | 0.6 | worst `Default` 1.2 |
| Examples Disney principled | 5 | 21.3 | 2.4 | car paint 2.9 (coat), plastic 2.4 (Burley vs Lambert) |
| TestSuite stdlib compositing | 45 | 22.1 | 0.0 | |
| TestSuite stdlib shader (unlit) | 4 | 47.6 | 0.4 | `unlit_mtrl2` 15: Karma ignores unlit `opacity`; Eldr follows genglsl (Storm differs from both) |

Houdini's ramps follow `hou.Ramp.lookup` within 1e-6 for every basis but hermite (read as Catmull-Rom).

## Result (2026-10-27)

957 materials with a surface. **747 compile whole** (558 on 2026-10-26, before the plan below); the rest bypass at least one node (its pass-through input or default stands in; the render stats count them) or have a surface Eldr doesn't read (a warning). With the coverage plan and the ten wrong-result fixes below (10-27 run), against Karma: 324 materials better by more than 1 dE, 63 worse; median dE of the TestSuite stdlib graphs 0.26 -> 0.0, of the lit Houdini materials 7.0 -> 2.7 (Ceramic, Leather, Wood, Concrete, Asphalt get their textures back; QuickSurfaceMaterial 0.07), of the Examples unchanged (2.1).

| Group | Materials | Whole (10-26) | Whole |
|---|---|---|---|
| Examples: standard_surface, OpenPBR, UsdPreviewSurface, hair | 74 | 72 | 72 |
| Examples: glTF PBR, Disney principled | 11 | 0 | 0 |
| Houdini basic + quick materials | 121 | 23 | 82 |
| TestSuite stdlib | 633 | 441 | 569 |
| TestSuite pbrlib (BSDF graphs, lama) | 110 | 20 | 20 |
| TestSuite nprlib, locale, libraries | 8 | 2 | 4 |

Houdini's remaining 39 only lacked Karma's own VEX nodes (`kma_rampconst`, `kma_color_hextiling`, `kma_voronoinoise3d`; read since lane 38). What still blocks most of the rest: pbrlib BSDF graphs, `hextiledimage`, `flake2d/3d`, matrix-valued graphs (`transformmatrix` with a connected matrix, `determinant`).

Changes where Karma and Storm agree Eldr got worse: compositing nodes (now bypassed: their pass-through, not the old constant), `transformmatrix` with a connected matrix (bypassed). Karma's Lama tests (pbrlib `surface` of `lama_*` BSDFs) render the default grey, as before the fixes (black was further from Karma). Where only Karma differs, Eldr follows MaterialX's genglsl: `round`, `dodge` and `color4` splits match Storm; `splittb` follows the formula (Karma swaps top and bottom; Storm reads uv 0 there). Silk, metal and soap materials moved by 1-3 against Karma from their neighbours' changed bounce light, not their own graphs (alone they render the same).

The 10-26 results:

Lit surfaces that compile whole, against Karma (CIEDE2000 of the sphere's mean): UsdPreviewSurface median 0.5, standard_surface 1.7 (32 of 56 under 2), OpenPBR without the light bulb: 5 of 9 under 1.4. The worst are car paint (coat, 13), subsurface (ketchup 9, jade 11), velvet/fuzz (5), thin film (19) and Houdini's absorbing glass and liquids (Eldr untinted, 30-60).

## Nodes that block materials

Blocked = materials using the node. Alone = materials it is the only blocker of. NG = defined in the MaterialX libraries as a nodegraph of other nodes.

| Node | Blocked | Alone | NG | Where |
|---|---|---|---|---|
| bump | 95 | 1 | yes | Houdini basic materials (94 of 120) |
| surface (pbrlib BSDF graph) | 75 | 59 | | pbrlib tests, hand-built BSDF graphs |
| colorcorrect | 59 | 0 | yes | Houdini basic materials |
| contrast | 59 | 0 | yes | Houdini basic materials |
| switch | 37 | 1 | | Houdini (texture set toggles) |
| document-defined node (local nodedef) | 27 | 21 | yes | custom nodes, simple hair, versioned nodes |
| transformmatrix | 27 | 22 | | stdlib math |
| tangent, bitangent | 20 | 2 | | Houdini, geometric tests |
| kma_rampconst | 19 | 1 | | Houdini (Karma-only) |
| kma_camerafalloff | 18 | 0 | yes | Houdini |
| kma_color_hextiling | 17 | 0 | | Houdini (Karma-only) |
| surface_unlit | 15 | 5 | | npr, texture tests |
| safepower | 12 | 11 | yes | stdlib math |
| triplanarprojection | 8 | 7 | yes | Houdini quick material, texture tests |
| ramp4, splitlr, splittb | 7 each | 7 | ramp4 | stdlib texture |
| round, sign, fract, tan, asin, acos, atan2, magnitude | 3-7 | all | | stdlib math |
| compositing: plus, minus, difference, screen, burn, dodge, inside, outside, over, premult, ... | 1-4 | all | | stdlib compositing |
| gltf_pbr, disney_principled | 6, 5 | all | yes | Examples |
| lama_* (conductor, dielectric, emission, add, mix, layer, ...) | 1-5 | | yes | Houdini MtlX Lama |
| hextiledimage, heighttonormal, geomcolor, logical and/or/not/xor, time, frame | 1-3 | | | stdlib |

Most used supported nodes: standard_surface, convert, image, multiply, UsdPreviewSurface, texcoord, mix, constant, extract, normalmap, add, invert, clamp, UsdUVTexture, ifgreater, primvar readers, normal, position, remap, tiledimage, place2d, the noises.

## Wrong results in graphs Eldr compiled whole (fixed 2026-10-26)

Eldr differed where Karma and Storm agreed. All ten are fixed in the graph compiler (`hydra/material.cpp`) and the interpreter (`renderer/shaders/program.glsl`), and `tests/test_mtlx_graphs.py` (ctest `mtlx_graphs`) asserts each against small inline graphs. Karma dE of the affected TestSuite materials went from 8-100 to 0 (combine, boolean conditionals, vector2 geompropvalue); `normalmap_cm` from 17.7 to 11.1 (the tangent frame is derived from derivatives, MaterialX's from a tangent stream).

| Case | Was | Now |
|---|---|---|
| vector2 to vector3/color3 (`convert`, `image`/`tiledimage` vector2 outputs, vector2 ops with a float operand) | third component left over | 0: `convert` is `kConvert` (from, to components), vector2 results are masked to two |
| `combine2/3/4` with vector inputs (VV, VF, CF) | one component per input | inputs unpacked (the variant's suffix gives their sizes) |
| `dotproduct`, `normalize` on vector4 | xyz only | all four (vector2: xy) |
| `power_vector2`, `modulo_vector2` | black (`pow(0, 0)`, `mod(x, 0)` in the unused components) | per component (the mask clears them) |
| `ifgreater`, `ifgreatereq`, `ifequal` with boolean results | 0 (their nodedefs have no in1/in2) | 1 or 0 from value1, value2 |
| float `tiledimage`/`image` read straight into an input | RGB | red, splat |
| `geompropvalue` vector2 of a missing primvar | the mesh's uv | its default; only `st`, `uv`, `st<n>`, `uv<n>` are uv sets (`UsdPrimvarReader_float2` keeps the first-uv-set rule) |
| `image` address mode `constant` | ignored (black) | the image's `default` outside [0, 1] (`TextureRef::constantWrap`, tested in `Fetch`); the direct texture path hands such images to the graph compiler |
| `normalmap` output used as a colour | tangent-space, about 2x | world-space normal (T, B, N from derivatives); only the surface's normal input keeps the tangent-space one |
| `surface` of BSDFs and EDFs only | default grey, a warning once (deliberately: Karma shades them, black is further off; until pbrlib BSDFs compose) | black |

The TestSuite's `image_addressing` constant cases (`uborder_color`, `vborder_color`) now show the `default` colour outside [0, 1], as the MaterialX spec says; Storm renders it black and Karma grey, so their dE against Eldr is 20+. The other addressing cases (clamp, mirror, decal) now match Storm (dE 0.1-0.3); Karma is darker there.

Also found and fixed: an `image` whose texcoord comes through any node but place2d/texcoord/primvar readers (a `multiply`, a `constant`...) read the mesh's `st` instead; those images now go to the graph compiler.

## Where Karma differs from MaterialX

Eldr matches Storm here.

| Case | Note |
|---|---|
| noise2d/3d, fractal2d/3d | Karma's own noise: other mean and pattern |
| smoothstep on vectors, `max_color4` | Karma returns 0 or wrong channels |
| color4 `image` outputs, HDR images, place2d | Karma darker or offset |
| OpenPBR `emission_luminance` | Karma scales nits down (10000 renders as about 20) |
| negative values | Karma keeps negative radiance; Eldr clamps at 0 (the harness clamps both) |
| `kma_*` nodes | Karma-only VEX shaders, not MaterialX |

## Plan status

| Item | Status |
|---|---|
| partialFallback | Done. An uncompilable node is bypassed (nodedef output `defaultinput`, else first input of the output's type, else the output's default) instead of dropping the whole input. Houdini basic + quick materials: median dE vs Karma 7.0 -> 2.7; Ceramic, Leather, Wood, Concrete, Asphalt get their textures back (dE 20-48 -> 1-9). |
| fallbackSignal | Done. Render stats `materialFallbackInputs` (inputs with a bypassed or dropped node, plus 1 per unknown surface) and `materialFallbackNodes` ("material: node (id)", first 12), shown under Materials in usdview's Eldr > Render Statistics and Houdini's viewport stats (gen_ui reads the same table). Unknown surface types warn once. |
| nodegraphInliner | Done. A node with a nodegraph implementation (HdMtlx's libraries, Houdini's `$HFS/houdini/materialx/libraries/houdini`, a document's own nodedef) is expanded into Hydra nodes inside the compiler, recursively; interface inputs take the node's connections, values, nodedef defaults or defaultgeomprops; filename defaults are resolved (tokens filled in). Nodes whose nodegraph needs a missing primitive (sign, rotate3d, heighttonormal, ...) are bypassed until it exists. Houdini basic materials now only miss heighttonormal, sign, rotate3d and Karma's kma_rampconst. |
| registers | Done. The compiler drops ops no output reads (bypassed partial compiles, unused nodegraph outputs) and shares equal constants; registers were already reused from a value's last reader. Of the corpus' 842 programs 5 still needed more than 8 registers (Houdini's QuickSurfaceMaterial 14, two stdlib_procedural and two color_management graphs 12), so the interpreter has 16: at 1920x1080, full screen, the shading pass is unchanged (Ceramic 9.5-10.0 ms at 8, 9.2-9.6 at 16; CarPaintMetallic, 205 ops with noise, 47-48 vs 44 ms; graphs off: 6.1 and 5.9 ms). Constants: at most 114 per program after sharing, of 240 reachable. |
| bump + heighttonormal | Done, in program.glsl (no mesh.frag change). Materials Hydra hands over from Karma's render context (Houdini's) read bump heights in scene units, as Karma: height times scale along the normal, its surface gradient in the uv frame. Others follow MaterialX: heighttonormal is MaterialX's genglsl formula on screen derivatives of the height and uv (1/16 Sobel parity); bump is its nodegraph (heighttonormal into normalmap), so it lands on the normal like a normal map. Per vertex (displacement graphs) and at ray hits (rt_refraction) it is flat. All 95 Houdini materials with bump now compile it, but their bump scales are tiny in uv units (-0.00025 to -0.004): Eldr and Storm (same formula) show almost no relief, Karma reads the height in scene units and shows it. Mean dE vs Karma unchanged (median 3.0) in uv units. |
| missingOps | Done, with genglsl's formulas: fract, round, sign, tan, asin, acos, atan2, magnitude, crossproduct in the main interpreter; rotate2d/3d, splitlr/splittb (aastep), transformmatrix (constant matrix33/44, v * M, vector2M3 and vector3M4 as points), transformpoint/vector/normal (object/model and world), tangent/bitangent (uv frame from derivatives; per vertex any perpendicular), viewdirection, time/frame (the scene globals' current frame and time codes per second; 0 at the default time), artistic_ior in the procedural pipelines (no cost to other programs). switch picks its input at compile time when `which` is constant, else a chain of ifgreatereq; and/or/not/xor on 0/1 are min/max/1-x/|a-b|; geomcolor is displayColor. Karma's `dot` notes ray:direction and ray:hitN (kma_camerafalloff) read the view direction and world normal. |
