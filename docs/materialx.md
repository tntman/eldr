# MaterialX coverage

What Eldr compiles of real MaterialX, measured on a corpus: the MaterialX TestSuite and Examples that ship with USD, and Houdini 22's `basic_materials` / `quick_materials`. `tests/mtlx_corpus.py` renders each material on a sphere with Eldr and Karma CPU (husk), optionally Storm (MaterialX's own GLSL), and lists the nodes Eldr can't compile.

Run: `usdenv.cmd python tests\mtlx_corpus.py --out <dir> [--storm] [--only stdlib_math,...] [--skip Light_Bulb] [--refs <earlier out>]` (`--refs`: render Eldr only, compare with that run's Karma and Storm images). It writes `report.json`, the node table below on stdout, and `sheet_<group>.png` (Eldr | Karma per sphere).

TestSuite files mostly hold bare nodegraphs. The harness wraps each unused output as standard_surface emission (unlit, so pixels are the graph's value) and BSDF/EDF outputs through `surface`. Matrix, string and displacement-only outputs are skipped. Storm referees the unlit graphs only; under the harness's lights it renders lit surfaces black.

## Results against Karma

Colour distance (CIEDE2000, dE) of each sphere's mean colour against Karma CPU (husk):

| Group | Materials | dE median | Notes |
|---|---|---|---|
| Houdini basic + quick | 121 | 1.6 | 91 under 2, all under 5; no node Eldr warns about. The 38 that use `kma_rampconst`, `kma_color_hextiling` or `kma_voronoinoise3d` have a median of 1.8. The worst: GlassBottleCobalt 4.9 (its coat tints light once on the way through, Karma twice), the Silk materials 2.5-4.7 (about 1.2x bright: Eldr's anisotropic specular doesn't darken under the dome as Karma's), ThinPlastic 4.5. 103 compile whole by the audit's static rule, which doesn't follow `kma_camerafalloff`'s graph (Eldr expands it) |
| Examples glTF PBR | 6 | 0.6 | worst `Default` 1.2 |
| Examples Disney principled | 5 | 2.4 | car paint 2.9 (coat), plastic 2.4 (Burley vs Lambert) |
| TestSuite pbrlib BSDF and EDF graphs | 51 | 2.8 | 22 under 2, 31 under 5 (graphs of library nodes; the 10 with document-defined nodedefs or hair left out: Karma renders most of those black or grey). The worst: subsurface with a large radius (radius 5: 31, Eldr keeps the colour, Karma washes it towards white), translucent (18, about 2x bright: translucent maps to subsurface), thin film under a tinted coat (22: the coat's own tint isn't read), dielectric in RT mode (7-9, about 1.4x bright), energy-compensated Oren-Nayar at roughness 0.75-1 (6-7.5, about 0.75x: standard_surface's Oren-Nayar is uncompensated) |
| TestSuite Lama (`lama_*` BSDFs) | 16 | 0.7 | 13 under 2. LamaLayer of a dielectric over a conductor 17: Karma lets the top's transmission leave the surface (a glass ball), Eldr layers it over the metal as MaterialX's `layer` does; LamaTranslucent 18 (translucent, above); LamaMix of conductor and glass 5 |
| TestSuite stdlib compositing | 45 | 0.0 | |
| TestSuite stdlib shader (unlit) | 4 | 0.4 | `unlit_mtrl2` 15: Karma ignores unlit `opacity`; Eldr follows genglsl (Storm differs from both) |

Houdini's ramps follow `hou.Ramp.lookup` within 1e-6 for every basis but hermite (read as Catmull-Rom).

The corpus has 957 materials with a surface. At the last full static audit 890 compile whole (of 93 Examples, 90; of 633 stdlib, 609; of 110 pbrlib, 88; of 121 Houdini materials, 103); the rest bypass at least one node (its pass-through input or default stands in; the render stats count them) or have a surface Eldr doesn't read (a warning). Rerun `tests/mtlx_corpus.py` for current counts.

Lit surfaces that compile whole, against Karma: UsdPreviewSurface median 0.5, standard_surface 1.7 (32 of 56 under 2), OpenPBR without the light bulb: 5 of 9 under 1.4. The worst are car paint (coat, 13), subsurface (ketchup 9, jade 11), velvet/fuzz (5), thin film (19).

## BSDF graphs

A MaterialX `surface` of pbrlib BSDFs and EDFs renders as one standard_surface whose inputs come from the graph's leaves (`BsdfMapper` in `hydra/material.cpp`). Nodes defined as nodegraphs of BSDFs (Lama's `lama_*`, a document's own BSDF or surface shader nodes) are expanded first, so they map the same way. The mapping runs once when the material syncs; the result costs what that standard_surface costs. Graph-driven weights and the Lama nodes' roughness and Fresnel graphs run in the material program: on the 12-sphere showcase (`showcase/2026-10-31_05_bsdf_graphs`) at 1920x1080 on a GTX 1080 Ti, the shading pass takes 3.6-3.7 ms, 3.3 ms with `eldr:materialGraphs` off.

| Node | Becomes |
|---|---|
| `oren_nayar_diffuse_bsdf`, `burley_diffuse_bsdf` | base colour (the leaves' weighted sum), `diffuse_roughness` |
| `dielectric_bsdf` | specular (`tint`, `ior`, roughness, thin film). On top of a layer whose base reflects (a dielectric or metal): the coat. In `T` or `RT` mode, not on top of a layer: transmission tinted by `tint`; `T` on top of a layer passes light to the base, as MaterialX's layer does |
| `conductor_bsdf` | metal: its F0 from `ior` and `extinction` (or `artistic_ior`'s reflectivity and edge colour, as in Lama's conductor), the edge colour from the same inputs |
| `generalized_schlick_bsdf` | alone: a metal of `color0` and `color82`; on top of a layer: a dielectric of `color0`'s mean F0 |
| `sheen_bsdf`, `subsurface_bsdf`, `translucent_bsdf` | sheen; subsurface (translucent as subsurface with its colour) |
| `uniform_edf`, `conical_edf`, `measured_edf`, `generalized_schlick_edf` | emission colour (the cone and profile aren't read; Schlick's `color0` tints) |
| `mix`, `add`, `multiply` (BSDF, EDF) | weights and tints of the leaves under them; a connected mix weight or tint runs as a graph |
| `layer` | top over base, as above; a VDF base (interior absorption) isn't read |
| `roughness_anisotropy` feeding a roughness | `specular_roughness` and `specular_anisotropy`; other roughness graphs: the square root of the first component |

With a constant metal weight, the dielectric's specular, transmission and subsurface weights count within the rest (1 - metalness), as standard_surface's do. Where the graph has both diffuse and metal leaves, they share one base colour (their weighted sum) and `metalness` is the metal's weight: exact where the mix weight is 0 or 1 (masks), between the two colours elsewhere. Roughness, IORs and lobe colours come from the first leaf that sets them. `chiang_hair_bsdf` and unknown BSDF nodes are left out with a warning and counted in the render stats; a graph with no lobe Eldr maps, or a mix of surface shaders, renders the default grey.

## Nodes that block materials

What still blocks most of the rest: document-defined nodes the audit's static rule can't follow, `hextiledimage`, `flake2d/3d`, `blackbody`, hair BSDFs and matrix-valued graphs (`transformmatrix` with a connected matrix, `determinant`). Blocked = materials in the corpus using the node. Alone = materials it is the only blocker of. NG = defined in the MaterialX libraries as a nodegraph of other nodes.

| Node | Blocked | Alone | NG | Where |
|---|---|---|---|---|
| document-defined node (local nodedef) | 27 | 21 | yes | custom nodes, simple hair, versioned nodes (Eldr expands most; the static rule counts them as blocked) |
| chiang_hair_bsdf | 2 | 1 | | pbrlib hair tests |
| hextiledimage | 1-3 | | | stdlib |

The TestSuite keeps most BSDF tests inside nodegraphs, which USD's MaterialX reader drops (its "Shader not found ... NodeGraphs" warning), so neither renderer would see them: the harness moves an unused nodegraph with a BSDF, EDF or surface shader output to the document's top level first.

Most used supported nodes: standard_surface, convert, image, multiply, UsdPreviewSurface, texcoord, mix, constant, extract, normalmap, add, invert, clamp, UsdUVTexture, ifgreater, primvar readers, normal, position, remap, tiledimage, place2d, the noises.

## Conventions Eldr follows

Where Karma and Storm agree, Eldr follows them. The graph compiler (`hydra/material.cpp`) and the interpreter (`renderer/shaders/program.glsl`) implement each case below, and `tests/test_mtlx_graphs.py` (ctest `mtlx_graphs`) asserts each against small inline graphs.

| Case | Eldr |
|---|---|
| vector2 to vector3/color3 (`convert`, `image`/`tiledimage` vector2 outputs, vector2 ops with a float operand) | third component 0: `convert` is `kConvert` (from, to components), vector2 results are masked to two |
| `combine2/3/4` with vector inputs (VV, VF, CF) | inputs unpacked (the variant's suffix gives their sizes) |
| `dotproduct`, `normalize` on vector4 | all four components (vector2: xy) |
| `power_vector2`, `modulo_vector2` | per component (the mask clears the unused ones) |
| `ifgreater`, `ifgreatereq`, `ifequal` with boolean results | 1 or 0 from value1, value2 |
| float `tiledimage`/`image` read straight into an input | red, splat |
| `geompropvalue` vector2 of a missing primvar | its default; only `st`, `uv`, `st<n>`, `uv<n>` are uv sets (`UsdPrimvarReader_float2` keeps the first-uv-set rule) |
| `image` address mode `constant` | the image's `default` outside [0, 1] (`TextureRef::constantWrap`, tested in `Fetch`); the direct texture path hands such images to the graph compiler |
| `image` whose texcoord comes through any node but place2d/texcoord/primvar readers (a `multiply`, a `constant`, ...) | goes to the graph compiler, which reads that texcoord |
| `normalmap` output used as a colour | world-space normal (T, B, N from derivatives); only the surface's normal input keeps the tangent-space one. The tangent frame comes from derivatives, MaterialX's from a tangent stream |
| `surface` of BSDFs and EDFs only | one standard_surface of the graph's leaves (BSDF graphs, above); with no leaf Eldr maps, the default grey and a warning once (Karma shades them; black is further off) |

The TestSuite's `image_addressing` constant cases (`uborder_color`, `vborder_color`) show the `default` colour outside [0, 1], as the MaterialX spec says; Storm renders it black and Karma grey, so their dE against Eldr is 20+. The other addressing cases (clamp, mirror, decal) match Storm (dE 0.1-0.3); Karma is darker there.

Where only Karma differs, Eldr follows MaterialX's genglsl: `round`, `dodge` and `color4` splits match Storm; `splittb` follows the formula (Karma swaps top and bottom; Storm reads uv 0 there).

## Where Karma differs from MaterialX

Eldr matches Storm here.

| Case | Note |
|---|---|
| noise2d/3d, fractal2d/3d | Karma's own noise: other mean and pattern |
| smoothstep on vectors, `max_color4` | Karma returns 0 or wrong channels |
| color4 `image` outputs, HDR images, place2d | Karma darker or offset |
| OpenPBR `emission_luminance` | Karma scales nits down (10000 renders as about 20) |
| negative values | Karma keeps negative radiance; Eldr clamps at 0 (the harness clamps both) |

## How the compiler handles nodes

| Item | How it works |
|---|---|
| Partial fallback | An uncompilable node is bypassed instead of dropping the whole input: the nodedef output's `defaultinput`, else the first input of the output's type, else the output's default. |
| Fallback signal | Render stats `materialFallbackInputs` (inputs with a bypassed or dropped node, plus 1 per unknown surface) and `materialFallbackNodes` ("material: node (id)", first 12), shown under Materials in usdview's Eldr > Render Statistics and Houdini's viewport stats (gen_ui reads the same table). Unknown surface types warn once. |
| Nodegraph inliner | A node with a nodegraph implementation (HdMtlx's libraries, Houdini's `$HFS/houdini/materialx/libraries/houdini`, a document's own nodedef) is expanded into Hydra nodes inside the compiler, recursively; interface inputs take the node's connections, values, nodedef defaults or defaultgeomprops; filename defaults are resolved (tokens filled in). Nodes whose nodegraph needs a missing primitive are bypassed. |
| Registers | The compiler drops ops no output reads (bypassed partial compiles, unused nodegraph outputs) and shares equal constants; registers are reused from a value's last reader. Of the corpus' 842 programs 5 need more than 8 registers (Houdini's QuickSurfaceMaterial 14, two stdlib_procedural and two color_management graphs 12), so the interpreter has 16: at 1920x1080, full screen, the shading pass costs the same as with 8 (Ceramic 9.5-10.0 ms at 8, 9.2-9.6 at 16; CarPaintMetallic, 205 ops with noise, 47-48 vs 44 ms; graphs off: 6.1 and 5.9 ms). Constants: at most 114 per program after sharing, of 240 reachable. |
| bump + heighttonormal | In program.glsl. Materials Hydra hands over from Karma's render context (Houdini's) read bump heights in scene units, as Karma: height times scale along the normal, its surface gradient in the uv frame. Others follow MaterialX: heighttonormal is MaterialX's genglsl formula on screen derivatives of the height and uv (1/16 Sobel parity); bump is its nodegraph (heighttonormal into normalmap), so it lands on the normal like a normal map. Per vertex (displacement graphs) and at ray hits (rt_refraction) it is flat. The 95 Houdini materials with bump compile it, but their bump scales are tiny in uv units (-0.00025 to -0.004): Eldr and Storm (same formula) show almost no relief, Karma reads the height in scene units and shows it. |
| Standard ops | genglsl's formulas: fract, round, sign, tan, asin, acos, atan2, magnitude, crossproduct in the main interpreter; rotate2d/3d, splitlr/splittb (aastep), transformmatrix (constant matrix33/44, v * M, vector2M3 and vector3M4 as points), transformpoint/vector/normal (object/model and world), tangent/bitangent (uv frame from derivatives; per vertex any perpendicular), viewdirection, time/frame (the scene globals' current frame and time codes per second; 0 at the default time), artistic_ior in the procedural pipelines (no cost to other programs). switch picks its input at compile time when `which` is constant, else a chain of ifgreatereq; and/or/not/xor on 0/1 are min/max/1-x/|a-b|; geomcolor is displayColor. Karma's `dot` notes ray:direction and ray:hitN (kma_camerafalloff) read the view direction and world normal. |
| Compositing nodes | plus, minus, difference, screen, burn, dodge, inside, outside, over, premult and the rest of stdlib compositing, plus glTF PBR, Disney principled and `surface_unlit`, and Karma's `kma_rampconst`, `kma_color_hextiling` and `kma_voronoinoise3d`. Their cost on a full 1080p screen (GTX 1080 Ti, shading pass): `kma_rampconst` 1.9-2.1 ms, `kma_color_hextiling` 2.0 ms, `kma_voronoinoise3d` 3.3 ms, as a bump under a coat (Car Paint flakes) 4.8 ms. `eldr:proceduralTextures` off drops the Voronoi. A `transformmatrix` with a connected matrix is bypassed. |
