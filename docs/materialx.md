# MaterialX coverage

What Eldr compiles of real MaterialX, measured on a corpus: the MaterialX TestSuite and Examples that ship with USD, and Houdini 22's `basic_materials` / `quick_materials`. `tests/mtlx_corpus.py` renders each material on a sphere with Eldr and Karma CPU (husk), optionally Storm (MaterialX's own GLSL), and lists the nodes Eldr can't compile.

Run: `usdenv.cmd python tests\mtlx_corpus.py --out <dir> [--storm] [--only stdlib_math,...] [--skip Light_Bulb]`. It writes `report.json`, the node table below on stdout, and `sheet_<group>.png` (Eldr | Karma per sphere).

TestSuite files mostly hold bare nodegraphs. The harness wraps each unused output as standard_surface emission (unlit, so pixels are the graph's value) and BSDF/EDF outputs through `surface`. Matrix, string and displacement-only outputs are skipped. Storm referees the unlit graphs only; under the harness's lights it renders lit surfaces black.

## Result (2026-10-26)

957 materials with a surface. **558 compile whole**; 399 lose at least one input to its constant, or render the default grey without a warning when the surface is unknown.

| Group | Materials | Whole |
|---|---|---|
| Examples: standard_surface, OpenPBR, UsdPreviewSurface, hair | 74 | 72 |
| Examples: glTF PBR, Disney principled | 11 | 0 |
| Houdini basic + quick materials | 121 | 23 |
| TestSuite stdlib | 633 | 441 |
| TestSuite pbrlib (BSDF graphs, lama) | 110 | 20 |
| TestSuite nprlib, locale, libraries | 8 | 2 |

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

## Wrong results in graphs Eldr compiles whole

Eldr differs where Karma and Storm agree.

| Case | Eldr | MaterialX |
|---|---|---|
| vector2 to vector3/color3 (`convert`, `image`/`tiledimage` vector2 outputs, vector2 ops with a float operand) | third component left over | 0 |
| `combine4` / `combine3` with vector inputs (VV, VF, CF) | one component per input | inputs unpacked |
| `dotproduct`, `normalize` on vector4 | xyz only | all four |
| `power_vector2`, `modulo_vector2` | black | per component |
| `ifgreater`, `ifgreatereq`, `ifequal` with boolean values | 0 | 1 |
| float `tiledimage` read straight into an input | RGB | red, splat |
| `geompropvalue` vector2 of a missing primvar | the mesh's uv | its default |
| `image` address mode `constant` (border colour) | ignored | border colour |
| `normalmap` output used as a colour | about 2x | |
| BSDF-only nodegraph with nothing Eldr reads | default grey | black |

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
