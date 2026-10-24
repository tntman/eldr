# Lärdomar från EEVEE för Eldr

Studie av Blenders EEVEE ("EEVEE Next", Blender 4.2+) på `main`, katalogen
`source/blender/draw/engines/eevee`. C++ ligger i `eevee_*.cc/hh`, shaders i
`shaders/*.bsl.hh` (Blenders nya shaderspråk, GLSL-likt). Blender är GPL, så
rapporten beskriver bara metoder och idéer. Ingen kod är kopierad.

Uppskattningarna gäller Claudes arbetstakt: S upp till 30 min, M 30–90 min,
L 1,5–4 h, XL över 4 h.

---

## 0. Översikt av EEVEE:s arkitektur

- **`Instance`** (`eevee_instance.cc`) äger alla moduler. En slutrendering är
  bara loopen `while (!sampling.finished()) render_sample();`. Varje sample är
  en hel bildruta med nya slumptal i *varje* stokastisk dimension, och
  `Film` ackumulerar resultatet.
- **`Sampling`** (`eevee_sampling.cc/hh`) delar ut ett lågdiskrepant tal per
  "dimension" och sample: pixelfilter, lins (DOF), tid (rörelseoskärpa),
  skuggor (ljusets form, PCF och strålar), raytrace, AO/GI, volym, SSS,
  transparens, ljusprober, g-buffer-dither och film-dither. Det är leapade
  Halton-följder med olika primtal. I shadern kombineras de med en
  blue-noise-textur per pixel genom `fract(blue_noise(pixel) + rng(dim))`
  (Cranley–Patterson-rotation).
- **Deferred-pipeline med closure-bins** (`eevee_pipeline.cc`,
  `eevee_gbuffer*.hh`, `eevee_deferred_*.bsl.hh`): upp till 3 closures per
  pixel. Varje closure får sin indirekta belysning från modulen
  **`RayTraceModule`** (`eevee_raytrace.cc`). Den spårar strålar för låg
  roughness och kör horisontskanning ("fast GI") för hög roughness, och
  ljusprober tar över där inget träffas.
- **Ljusprober**: sfärprober (reflektion, oktahedralt atlas,
  `eevee_lightprobe_sphere.cc`), volymprober (irradiance-galler som L1 SH med
  synlighet, bakade med surfels, `eevee_lightprobe_volume.cc`), planara prober
  samt världen, som alltid ligger sist som fallback.
- **Virtuella skuggkartor** (`eevee_shadow.cc`, `eevee_shadow_*.bsl.hh`):
  sidor på 256² i ett gemensamt atlas, tilemaps på 32×32, clipmaps för sol,
  LOD styrd av skärmens pixeltäthet, och ray-marching *i* skuggkartan för
  mjuka skuggor.
- **Volymer** (`eevee_volume.cc`, `eevee_volume.bsl.hh`): Frostbite-modellen
  med froxlar. Materialegenskaperna rastreras in i ett 3D-galler, sedan följer
  scatter (ljus, skuggor och prober), integration och resolve. Transparenta
  ytor samplar samma integrerade galler.
- **`Film`** (`eevee_film.cc`, `shaders/eevee_film.bsl.hh`): ett pixelfilter
  med vikter i ett separat buffertlager. Den växlar mellan TAA (när något rör
  sig) och ren viktad ackumulering (när bilden står still och i slutrenderingar).

---

## 1. Kvalitet i slutrenderingen: provackumulering

### Hur EEVEE gör

- **Två lägen i Film** (`eevee_film.hh`, kommentaren överst;
  `eevee_film.bsl.hh` `store_combined`):
  - *Interaktivt*: historiken reprojiceras med Catmull–Rom-sampling och
    klipps mot en varians-AABB i YCoCg i grannskapet. Blendningen viktas med
    luma (Karis 2014).
  - *Statiskt eller slutrendering*: **ingen reprojektion och ingen
    klippning**. Nytt värde = (gammalt·vikt_summa + nytt·filtervikt) /
    (vikt_summa + filtervikt). Vikterna ligger i en egen fp32-textur
    (`weight_tx_`, ett lager för ackumulerad vikt och ett för avstånd till
    närmaste sample).
  - Bytet sker efter `interactive_mode_threshold = 3` stillastående samples
    (`eevee_sampling.hh`). En stilla vy i viewporten konvergerar alltså också
    mot en väntevärdesriktig bild.
- **Pixelfilter i stället för box** (`eevee_film_shared.hh`
  `film_filter_weight`, `eevee_film.cc` rad ~811–910): en Gauss som är
  anpassad till Blackman–Harris. Standardradien 1,5 px (`scene.r.gauss`)
  kommer från användaren. Varje renderpixel splattas till upp till 16
  filmpixlar med förberäknade vikter. Jittret täcker hela pixeln, och vid
  radie under 1/√2 samplas en disk som täcker filtret.
- **Allt jittras per sample** (`eevee_sampling.cc` `step()`):
  kamerans subpixeloffset, linsposition (DOF), slutartid, punkt på ljusets
  form (skuggor), PCF-offset, strålriktningar, AO-slicevinkel, froxeljitter,
  SSS-rotation och val av ljusprob (stokastisk blend mellan prober).
- **DOF** (`eevee_depth_of_field.cc` rad 120–215, `eevee_sampling.cc`
  `dof_disk_sample_get`): linsen jittras i ett "web"-mönster med ringar. Den
  efterföljande post-DOF:en körs bara med en *rest*radie
  `(1/√N + overblur)·apertur`, som fyller hålen mellan linssamplen. Med
  många samples blir bokeh alltså fysiskt korrekt (rätt ocklusion och
  transparens).
- **Rörelseoskärpa** (`eevee_motion_blur.cc`): scenen evalueras om vid N
  tidssteg som fördelas enligt slutarkurvans CDF. Varje steg renderar
  samples/N samples, och vektoroskärpan fyller bara mellan stegen.
- **Kontroll av precisionsdrift**: `patch_float_for_16f_storage` adderar en
  slumpad avrundning under 1 ULP innan skrivning till fp16, eftersom
  hårdvaran trunkerar och upprepad ackumulering annars driver (#126947,
  #129533). NaN filtreras och negativa värden klampas.
- **Interaktiv slinga**: medan kameran rör sig loopas bara de första 8
  AA-samplen, 32 raytrace-samplen och 32 volymsamplen
  (`interactive_sample_*`). Brusmönstret blir då periodiskt och TAA kan
  stabilisera det.
- **Klamp av ljus** (`clamp_data_`): separata gränser för direkt och indirekt
  ljus på ytor och volymer. Det dödar fireflies utan att påverka resten.

### Hur Eldr skiljer sig

- Eldr använder samma TAA-pass (`taa.frag`, med varians-klippning och
  Reinhard-komprimering) även för en stilla kamera och för slutbilder. Vikten
  är 1/(n+1) upp till 16 frames. Klippningen skär bort legitim variation,
  till exempel brus i mjuka skuggor, SSGI och SSR, så bilden konvergerar mot
  ett *klippt* medelvärde. Den blir partisk, med "kladdig" penumbra och
  förlorade högdagrar.
- Jittret är 16 Halton-punkter (2,3), boxfiltrerat. Alla andra effekter
  använder IGN som flyttas per frame och är inte avkorrelerade från
  pixeljittret.
- Rörelseoskärpa gör bilden "final" efter ett enda sample. DOF är bara post.
  Skuggor jittras inte, så PCSS:ens approximation blir slutresultatet.

### Rekommendation

1. **Ackumuleringsläge i TAA-modulen**: lägg till en fp32-buffert med
   (rgb·w, w). När kameran stått still i ≥ 2–3 frames, eller för usdrecord,
   ska ackumuleringen vara ren, utan klipp och utan reprojektion, och
   fortsätta till N samples (användarstyrt, t.ex. 64–256). Behåll TAA när
   något rör sig. **L (90–150 min).**
2. **Gaussiskt pixelfilter** (radie ≈ 1,5 px) i ackumuleringen:
   splatta 3×3 med vikt efter jitteravståndet. Billigt och mycket skarpare än
   TAA-blur. Ingår i punkt 1 (+20 min).
3. **Avkorrelerade sampledimensioner**: ett litet `SampleSet` per frame
   (UBO) med ett Halton-tal per effekt (pixel, lins, tid, skugga, GI, SSR,
   fog, SSS), plus en blue-noise-textur (t.ex. 64² × 4 kanaler) i shadern med
   `fract(bn + rng[dim])`. **M (45–90 min).**
4. **Fp16-dither och NaN-filter** i ackumuleringen. **S (10–20 min).**

---

## 2. Global belysning och ray tracing

### Hur EEVEE gör

**Pipeline** (`eevee_raytrace.cc` `sync()`, `eevee_ray_*.bsl.hh`):

1. *Tile classify/compact*: indirekt dispatch, så bara tiles med closures som
   behöver strålar körs.
2. *Generate* (`eevee_ray_generate.bsl.hh`): en BSDF-samplad riktning per
   pixel och closure (GGX-VNDF) från blue noise plus sample-offset. 1/pdf
   sparas för denoisern. Spårningen kan köras i halv eller kvarts upplösning
   (`trace_pixel_scale`).
3. *Trace*: först planara prober, sedan skärmen (HiZ-spårning enligt
   McGuire–Mara med egna modifieringar, `eevee_ray_trace_screen_lib.bsl.hh`).
   Radiansen vid träffen hämtas från **förra bildrutans** resultat, och
   träffen reprojiceras med `history_persmat`. En träff som var utanför
   förra vyn räknas som miss. Vid miss används **probe-fallback per stråle**:
   sfärproben i strålens riktning med roughness-LOD och parallaxkorrektion
   plus volymprobens SH. Alla radianser klampas med `clamp.surface_indirect`.
4. *Denoise* (`eevee_ray_denoise.bsl.hh`, enligt "Stochastic All The
   Things", Stachowiak/EA SEED):
   - *Spatial*: ray reuse med ratio estimator. Grannarnas strålar
     återanvänds och viktas med BSDF/pdf. Filterkärnan orienteras och
     sträcks i reflektionsriktningen för låg roughness, och storleken skalas
     med roughness.
   - *Temporal*: reprojektion **både av ytan och av träffpunkten**
     (reflektionsparallax). Historiken klampas mot lokal statistik i YCoCg och
     blendas med 0,97 (lägre vid låg varians). Variansen bärs vidare i en egen
     buffert.
   - *Bilateral*: variansstyrd, med "tonemappade" vikter mot fireflies.
5. *Fast GI* (horisontskanning) för closures över `trace_max_roughness`.
   Övergången går mjukt via `roughness_mask_scale/bias`, se resolve i
   `eevee_fast_gi.bsl.hh`.

**Fast GI** (`eevee_fast_gi.bsl.hh`, efter "Screen Space Indirect Lighting
with Visibility Bitmask", Therrien m.fl.):

- *Setup*: förra bildrutans radians reprojiceras till aktuell vy och klampas.
  Mips byggs av radians och normal (4 nivåer i delat minne).
- *Scan*: slices × 2 sidor. Stegen är kvadratiskt fördelade
  (`1 + ((j+noise)/N)²·max`) och korsar alltid minst en pixel. HiZ-LOD väljs
  stokastiskt med `floor(noise + lod)`. Varje sample ger ett **vinkelintervall
  (front, back)** där back = front + estimerad tjocklek. Intervallet omvandlas
  till en **32-bitars synlighetsmask**, och bara sektorer som inte redan är
  ockluderade bidrar med ljus. Det ger korrekt ljus bakom tunna objekt i
  stället för GTAO:s "högsta horisont vinner".
- **Tjocklek** (`eevee_ray_types_lib.bsl.hh` `ScreenThicknessEstimator`,
  `eevee_raytrace_shared.hh` `ScreenThicknessParameters`): tjockleken
  uppskattas från djuplutningens kontinuitet mellan steg. Ett abrupt
  lutningsbyte betyder ett nytt objekt och kopplar bort. Minsta tjocklek
  motsvarar pixelns fotavtryck vid samplets djup. Det gör att ytor nästan
  parallella med vyn inte blir porösa. **Det är kärnan i problemet med
  strykande vinklar.**
- Emitterns normal tas med (`facing = dot(normal, -L)`), och baksidor skalas
  med `backface_hit_scale`. Bakgrund hoppas över så att den inte skuggar.
  Slicevinkeln jittras inom bitmaskens upplösning mot banding.
- Resultatet lagras som **SH L1 (4 texturer) med synlighet i w**. Resolve
  evaluerar SH:en i varje closures dominanta riktning, så samma skanning
  fungerar för diffus, rough spekulär och SSS. Synligheten fyller sedan i
  **avlägset ljus från volymproben**: `radiance += visibility ·
  probe_irradiance(L)`. Nära fält kommer från skärmen och fjärrfält från
  proberna, och de ersätter inte varandra.
- Denoisern är bara en 3×3 bilateral (plan- och normalvikt). Konvergensen
  kommer från Film-ackumuleringen, och i rörelse från TAA plus den
  interaktiva sampleslingan.

**Sfärprober** (`eevee_lightprobe_sphere.cc`,
`eevee_lightprobe_sphere_bake.bsl.hh`, `eevee_lightprobe_sphere.bsl.hh`):

- Kuber renderas till ett oktahedralt atlas med mips per roughness. Texeln
  centreras på oktaederns kanter mot sömmar, och vikterna följer exakt
  rymdvinkel per texel.
- **Normalisering**: reflektionen skalas med kvoten mellan lokal
  volymprob-irradiance (ambient-term) och probens egen SH-ambient
  (`normalization_factor`). Inomhus tar det bort himmelsreflektioner som
  läcker från en prob som fångats på annan plats.
- Proben väljs stokastiskt per pixel och sample, med mjuk övergång via brus.
  Cube och SH mixas efter roughness (`roughness_to_mix_fac`).
- **Solextraktion**: världens HDRI-energi över `sun_threshold` dras bort ur
  proben och blir en riktad sol (riktning = viktat medel, vinkel ur spridning)
  med riktiga skuggor.

**Volymprober** (`eevee_lightprobe_volume.cc`,
`eevee_lightprobe_volume_bake.bsl.hh`, `eevee_surfel_*.bsl.hh`):

- Bakningen sker offline. Scenen rastreras längs 3 axlar till *surfels*.
  Ljuset (med skuggor) appliceras på surfels, sedan körs "Global Ray Bundles":
  för slumpriktningar projiceras surfels till länkade listor som sorteras
  (radix), och ljus transporteras mellan grannar i listan under flera studsar.
- Gallerpunkter får **virtual offset**: de flyttas ut ur geometri med hjälp av
  närmaste surfels och deras facing. Varje punkt lagrar L1 SH irradiance plus
  **L1 SH synlighet** och en validitetsbit. Brickor används i ett atlas.
  Vid sampling används McGuires biaser (normal- och vyoffset) och manuellt
  viktad trilinjär interpolation, och världen ligger sist som fallback.
- Clamp för direkt och indirekt ljus per galler mot fireflies.

### Hur Eldr skiljer sig

- SSGI i Eldr är horisontbaserad (högsta horisont), utan tjockleksmodell och
  utan normaltest för träffen. Den har vyberoende radians och ackumuleras bara
  när kameran är still. Det förklarar artefakterna vid strykande vinklar
  (ingen minsta tjocklek, så "porösa" eller felaktiga ockluderare) och efter
  rörelse (ingen stabil interaktiv sampleslinga, och bara IGN).
- Att fylla synligheten med prob- eller domljus gör Eldr redan
  (commit 99dcd0d). Det är samma princip som EEVEE:s.
- SSR har ingen ratio estimator och ingen temporal reprojektion av
  träffpunkten. Eldrs spatiala 8-taps-filter på planet liknar bara EEVEE:s
  spatiala steg.
- Reflektionsproben är en kub utan normalisering, så den läcker inomhus.
- DDGI uppdateras med RT, vilket är dynamiskt och därmed bättre än EEVEE:s
  bakade galler. Däremot saknar Eldr virtual offset och fp32-ackumulering för
  slutbilder.

### Rekommendation

1. **Tjockleksestimator och minsta pixeltjocklek** i SSGI-skanningen och i
   SSR-marchen: härled tjockleken ur lutningskontinuiteten och sätt ett
   golv på ~pixelradien vid samplets djup. Det ger störst effekt mot artefakter
   vid strykande vinklar. **M (45–90 min).**
2. **Synlighetsbitmask (32 sektorer) i stället för max-horisont** i SSGI, med
   facingvikt från emitterns normal och slicejitter. Det ger ljus bakom tunna
   föremål och mindre banding. **L (90–180 min).**
3. **Stabil interaktiv slinga**: medan kameran rör sig, loopa de första 8–32
   samplen av brusoffseten så att TAA ser ett periodiskt mönster. Byt till
   ackumulering när kameran står still. **S (15–30 min)**, förutsatt avsnitt 1.3.
4. **SSR-denoiser enligt Stachowiak**: spara 1/pdf och träff-t. Spatial
   ratio estimator med kärna som sträcks i reflektionsriktningen, temporal
   reprojektion via träffpunkten med klamp mot lokal YCoCg-statistik, och
   varians i en egen buffert. **L (2–4 h).**
5. **Roughness-split**: strålar/SSR under en roughness-tröskel och SSGI-SH
   över, med en mjuk mixfaktor. Det kräver att SSGI sparar riktad information
   (SH L1, 4 × RGBA16F i halv upplösning). **L (90–180 min).**
6. **Normalisering av reflektionsproben** med kvoten lokal irradiance
   (DDGI/dome-SH) / probens SH-ambient. **S (20–40 min).**
7. **Solextraktion ur domen** till en riktad skuggande sol. Det ger skarpa
   HDRI-skuggor genom de befintliga kaskaderna. **M (60–120 min).**
8. **DDGI för slutbilder**: kör K probe-uppdateringar med vikt 1/(n+1)
   (ingen hysteres) innan första ackumulerade sample, och lägg till probe
   relocation (virtual offset) längs närmaste träff. **M (45–90 min).**

---

## 3. Skuggor

### Hur EEVEE gör

- **Virtuella skuggkartor** (`eevee_shadow.cc/hh`, `eevee_defines.hh`):
  sidor på 256² i ett poolatlas och tilemaps med 32×32 tiles. Det ger
  effektivt 8192² per kubsida eller clipmapnivå. Pass per frame:
  - `tag_usage` (från djupbuffert och transparenta ytor) markerar de sidor
    och den LOD som skärmens pixeltäthet behöver.
  - mask, free, defrag, allocate, clear och render, bara för sidor som saknas
    eller är smutsiga.
  - Statiska sidor cachas mellan frames.
  Solen använder clipmaps runt kameran. `shadow_resolution_scale` blir en
  global LOD-bias, och upplösningen kan begränsas per ljus.
- **Jitter av ljusets form per sample** (`eevee_light_shadow_setup.bsl.hh`
  ~rad 288–340, `eevee_light.cc` `shape_parameters_set`): solens riktning
  samplas i en kon med solens halva vinkel. Area-ljus samplas på rektangel
  eller disk och sfärljus i bollen. Skuggkartan renderas från den punkten.
  Det är på i slutrenderingar och valfritt i en stilla viewport
  (`SCE_EEVEE_SHADOW_JITTERED_VIEWPORT`). Kvarvarande filterbredd skalas med
  `shadow_jitter_overblur`, så mjukheten kommer från ackumuleringen och inte
  från filtret.
- **Ray tracing i skuggkartan** (`eevee_shadow_tracing.bsl.hh`): `ray_count`
  strålar med `step_count` steg går från en slumpad punkt på ljuset mot
  ytpunkten, alltså *omvänt*. Ockluderarens djup extrapoleras med lutningen
  mellan steg. Resultatet blir kontakthärdande penumbra från en enda karta,
  med få läckor.
- **Biaser**: normaloffset proportionell mot texelradien vid mottagaren
  (`shadow_normal_offset`, en kontinuerlig uppskattning utan hopp mellan LOD)
  samt en separat terminatoroffset mot shadow terminator-artefakter.
  Stokastisk PCF med `filter_radius`.
- Transparenta skuggkastare med jitter markerar sina sidor för omritning varje
  sample.

### Hur Eldr skiljer sig

Eldr har kaskader och ett atlas med PCSS (Poisson-sökning och filter roterat
per pixel och frame). PCSS är bara en approximation: fel penumbra vid
överlappande ockluderare och läckor i blocker-sökningen. Ljuset jittras aldrig,
så en slutbild konvergerar mot PCSS-approximationen och inte mot en riktig
area-ljusskugga.

### Rekommendation

1. **Jittrad ljusform i ackumuleringsläget**: rendera om sol-kaskaderna med
   riktningen samplad i solkonen per sample, och lokala ljus från en punkt på
   formen. Sänk PCSS-filtret till en liten restradie (~1/√N av penumbran).
   Kostnaden är skuggrendering varje sample, vilket är acceptabelt offline.
   **L (90–180 min).**
2. **Texelradie-baserad normaloffset och terminatoroffset**. **S (20–40 min).**
3. **Skugg-ray-marching i kartan** (omvänd, från ljuset) som alternativ till
   PCSS för interaktiv kvalitet. **L (2–4 h)**, lägre prioritet än punkt 1.
4. **Virtuella skuggkartor**: **XL (1–2 dagar).** Inte nu. RT-skuggorna
   täcker de högsta kraven.

---

## 4. Volymetri

### Hur EEVEE gör

- **Froxlar** (`eevee_volume.hh`, kommentaren överst): material
  (värld + objekt rastrerade in i ett 3D-galler, med occupancy-bitmasker för
  exakt in- och utträde i mesh-volymer), scatter, integration och resolve.
  Djupfördelningen är exponentiell och styrs av `sample_distribution`.
  Integrationen har ett snävt intervall runt volymobjekten för precisionens
  skull.
- **Scatter** (`eevee_volume.bsl.hh`): lokala ljus med skuggkartor plus
  **heterogena volymskuggor** genom att marcha extinktionsgallret mot ljuset
  (`shadow_steps`). För solen marchar den till frustumets kant. Indirekt ljus
  kommer från volymproberna, fasfunktionen beräknas som medel-anisotropi och
  `light_clamp` gäller.
- **Temporal reprojektion** bara interaktivt: en exponentiell historik med 16
  frames, 8 under navigering och 3 vid transformering eller uppspelning.
  **I slutrendering är reprojektion av** (`exponential_frame_count = 0`). I
  stället jittras froxlarna med subtexel-offset per sample
  (`SAMPLING_VOLUME_*`), och Film tar medelvärdet, vilket gör resultatet
  väntevärdesriktigt.
- **Komposition med transparens**: opaka ytor får resolve i ett
  fullskärmspass. Transparenta forward-ytor samplar *själva* det integrerade
  gallret vid sitt eget djup (`eevee_surf_forward.bsl.hh` ~rad 142) och
  applicerar transmittans och inscatter. Där tas också bort den del av
  scattern som ligger bakom ytan. Ordningen blir rätt per pixel, eftersom
  objektvolymerna ligger i *samma* froxelgaller som dimman.

### Hur Eldr skiljer sig

Froxeldimman i Eldr finns redan med reprojektion. NanoVDB-volymerna
raymarchas för sig *efter* transparensen, så glas eller WBOIT framför en
volym blir fel. Volymen hamnar ovanpå. Volymskuggning sker bara från sol och
kaskader, och lokala ljus är oskuggade.

### Rekommendation

1. **Froxeljitter per sample och ingen reprojektion i ackumuleringsläget**
   (ett enkelt medel). **S (20–40 min).**
2. **Volym under transparens**: låt WBOIT-ytorna (och SSR-refraktionen)
   applicera froxelgallrets transmittans och inscatter vid sitt djup, samma
   som EEVEE:s forward-väg. För NanoVDB: skriv transmittansen till en
   halvupplöst "volymdjup"-buffert (eller en 2-lagers front/back) som
   transparenspasset läser. Eller, enklast: raymarcha volymen *före*
   transparensen till en separat buffert (inscatter, transmittans) och
   komponera transparenta lager ovanpå med rätt djuptest. **L (2–3 h).**
3. **Skuggor från lokala ljus i dimma och volym** via skuggatlaset. Hämta en
   tap per steg och sample, och låt ackumuleringen reda ut bruset.
   **M (45–90 min).**

---

## 5. Övrigt som ger fotorealism billigt

- **Ljuströskel** (`eevee_light.cc` `attenuation_radius_get`):
  influensradien = √(effekt / tröskel) per ljus, separat för yta och volym,
  med mjuk falloff. Klustren blir mindre och utan synliga kanter. Eldr kan
  räkna radien från effekt och tröskel i stället för ett fast eller
  användarsatt värde. **S (20–40 min).**
- **Klamp av indirekt ljus** (`brightness_clamp_max` på SSR, SSGI, prober och
  reprojicerad radians): en enkel och tydlig vinst mot fireflies och
  "glittrande" SSGI. **S (15–30 min).**
- **Tjocklek** (`eevee_thickness_lib.bsl.hh`,
  `eevee_deferred_thickness_amend.bsl.hh`): materialets tjocklek, lagrad i
  g-bufferten med en sfär- eller slabmodell, används för refraktion genom två
  gränsytor (in och ut) och för SSS-transmission. Tjockleken *justeras* efter
  rastrering med skuggkartorna (avstånd genom objektet mot ljuset), vilket ger
  translucens på tunna och tjocka delar. Eldr har SSR-refraktion och separabel
  SSS: lägg till tjockleken från skuggatlaset i SSS-transmissionen
  ("back-lit"-öron och löv). **M (60–90 min).**
- **SSS** (`eevee_subsurface.bsl.hh`, Golubev 2018): stokastiska skivsamples
  per pixel med en profiltextur per kanal och radie, roterade per sample, och
  med avvisning av andra objekt (inget läckage mellan objekt). Med
  ackumulering konvergerar den till en mjuk profil. Eldrs separabla blur får
  korsartefakter. Att byta till slumpad skiva per sample är **M (45–90 min)**
  när ackumuleringen finns.
- **AO och bent normals**: EEVEE har ingen separat GTAO längre. AO kommer från
  samma bitmaskskanning ("fast GI AO only"), och AO-noden använder samma kod.
  Eldr kan på samma sätt låta SSGI-masken ge AO och specular occlusion.
- **Tonemapping**: AgX och Filmic ligger i Blenders OCIO-färghantering och
  inte i EEVEE. Eldr har redan ACES och AgX. Det som EEVEE gör och Eldr kan
  ta efter är att Film ackumulerar i *linjärt* scenljus och inte efter
  tonemappning, men med luma-viktning i det interaktiva TAA-läget.

---

## Prioriterad lista

| # | id | Namn | Ledtråd | min–max (min) |
|---|----|------|---------|---------------|
| 1 | `filmAccumulation` | Ren provackumulering för stilla vy och slutbild | fp32 (rgb·w, w), ingen klipp eller reprojektion efter 3 stilla frames, Gauss 1,5 px, fp16-dither och NaN-filter | 90–150 |
| 2 | `sampleDimensions` | Avkorrelerade sampledimensioner | UBO med ett Halton-tal per effekt och blue-noise-textur, `fract(bn + rng[dim])` i alla stokastiska shaders | 45–90 |
| 3 | `ssgiThickness` | Tjockleksestimator i SSGI och SSR | Lutningskontinuitet plus golv på pixelradien vid djup, korsa minst en pixel, kvadratisk stegfördelning | 45–90 |
| 4 | `shadowJitter` | Jittrad ljusform per sample | Solkon och punkt på area- eller sfärljus per sample, rendera om kartor, PCSS-restradie 1/√N | 90–180 |
| 5 | `interactiveSampleLoop` | Periodisk brusslinga i rörelse | Loopa de första 8–32 brusoffseten medan kameran rör sig, ackumulera fritt när den står still | 15–30 |
| 6 | `indirectClamp` | Klamp av indirekt radians | `brightness_clamp_max` på SSGI-input, SSR-träff och prober, separat gräns för direkt och indirekt | 15–30 |
| 7 | `ssgiBitmask` | Synlighetsbitmask i SSGI | 32 sektorer per slice, ljus bara från oockluderade sektorer, facingvikt från emitterns normal, slicejitter | 90–180 |
| 8 | `dofJitter` | Linsjitter per sample | Ring- eller web-mönster på aperturen per sample, post-DOF bara med restradie (1/√N + overblur) | 45–90 |
| 9 | `probeNormalize` | Normalisering av reflektionsproben | Skala kuben med lokal irradiance / probens SH-ambient mot läckor inomhus | 20–40 |
| 10 | `volumeFinalJitter` | Froxeljitter utan historik i slutbild | Subtexel-jitter per sample, reprojektion av vid ackumulering | 20–40 |
| 11 | `ssrDenoise` | SSR-denoiser (Stachowiak) | Spara 1/pdf och träff-t, ratio estimator, reprojektion via träffpunkt, variansbuffert och bilateral | 120–240 |
| 12 | `domeSunExtract` | Solextraktion ur domen | Energi över tröskeln blir en riktad sol med kaskadskuggor, tas bort ur IBL och SH | 60–120 |
| 13 | `ddgiPreconverge` | DDGI som konvergerar före slutbild | K uppdateringar med 1/(n+1) innan ackumulering, plus probe relocation | 45–90 |
| 14 | `volumeUnderTransparency` | Volymer under transparens | Volym före OIT till inscatter- och transmittansbuffert, transparenta lager komponeras ovanpå per djup | 120–180 |
| 15 | `motionBlurTimeSamples` | Rörelseoskärpa via tidssamples | Fördela samples över slutartiden (Hydra-tidssamples eller hastighet), vektorblur bara mellan stegen | 90–180 |
