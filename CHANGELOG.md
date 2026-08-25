# Changelog

All notable changes to this project will be documented in this file.

## [1.11.0] - 2026-06-25

### Added
- **Game-facing MCP server** — `igi1conv mcp` now exposes the registered
  converter operations through JSON-RPC tools and the
  `igi1conv://game-capabilities` resource.
- **Stdio and Streamable HTTP transports** — stdio is the default; HTTP binds
  to `127.0.0.1:8765/mcp` by default, validates `Origin`, uses per-client
  sessions, and refuses plaintext non-loopback binds.
- **QSC game-object editing** — list nested `Task_New` objects and write
  position, rotation/gamma, model id, team, bone hierarchy, stand animation,
  or validated indexed task arguments while preserving untouched source text.

### Scope and safety
- MCP exposes only game-data inspection, validation, conversion, packing, and
  edits that can affect files consumed by Project IGI.
- GUI preferences/state, themes, cache paths, viewer/camera transforms,
  playback controls, layout, shell execution, and arbitrary executables are
  intentionally unavailable.

### Verification
- Added focused registry, protocol, QSC editor, and transport tests.
- Added a real-process stdio/HTTP smoke harness covering a real QSC listing,
  position/rotation/model/team write-back, Origin rejection, and the remote
  authentication guard.

### Hardened
- Removed the editor-only `iff.export-gif` preview from the MCP allowlist.
- Reject unknown MCP argument fields instead of silently ignoring values that
  the advertised schemas mark with `additionalProperties: false`.
- Enforced the `HumanSoldier` layout for named placement fields; other task
  classes remain available through explicit indexed updates.
- Rejected malformed quoted QSC literals and added regression coverage.
- Documented that MCP QSC writes require a distinct output path.
- Bounded stdio frames before allocation and preserved framing after an
  oversized request.
- Made `lightmap.recalc` preflight and transactionally commit `.olm` files so
  parse/write failures do not intentionally leave a partial edit set.
- Scoped `working_directory` changes with restoration on failure and exception
  paths; made QSC object/lightmap scanning ignore block comments consistently.
- Added bounded HTTP `OPTIONS` CORS preflight handling with explicit allowed
  methods/headers and no CORS headers on rejected Origins.
- Rejected lightmap recalculation when the MEF render-block and resolved `.olm`
  counts differ; validated OLM payload sizes before allocation and PNG
  dimensions before the OLM `uint16` cast.
- Rejected unmatched directory files in `res repack`, checked QSC temporary-file
  flush/close failures, and made the corpus identity test restore every touched
  `.olm` file even when an assertion exits early.
- Kept corpus-generated IFF reports in test temporary directories so live
  integration tests do not require write access beside game assets.
- Capped HTTP header/body reads to the configured message budget, returned
  JSON-RPC `-32600` errors for malformed id-less objects, and sourced the MCP
  server version from the build version definition.
- Required MCP clients to complete `initialize` before any other method,
  preserved response-free lifecycle notifications, and returned JSON-RPC parse
  errors for malformed HTTP request bodies.
- Rejected in-place output paths for generic write commands, rejected empty
  direct QSC arguments without shifting indexes, and rejected non-finite or
  trailing numeric lightmap rotation arguments.
- Kept inherently in-place `lightmap.recalc`, `mtp.repair`, and `mtp.sync`
  available to the CLI while excluding them from the MCP registry.
- Added positional/default output collision checks, per-session HTTP lifecycle
  state, bounded command output, finite request deadlines, and colocated Qt
  runtime deployment for Windows builds.

### Added
- **OLM Writer** — `.olm` lightmaps can now be written, not just read:
  - `igi1conv olm from-png <in.png> -o <out.olm> [--template <ref.olm>]` — build an `.olm` from a PNG. Dimensions come from the PNG; `--template` copies the runtime header fields (uv scale, version, date, grid) from an existing `.olm` so a rebuilt file matches the original's metadata exactly. Verified by an `olm -> png -> olm -> png` round-trip that preserves the pixel payload byte-for-byte (R/B channel swap cancels out).
  - New `WriteOlm()` / `BuildOlmFromRGBA()` API in `cmd_olm.h` for in-process use by the recalc path.
- **`lightmap recalc` command** — re-light a placed object's baked `.olm` files after it has been rotated/moved, without a full radiosity re-bake:
  - `igi1conv lightmap recalc --model <id> --qsc <objects.qsc> --task-id <id> --mef <file.mef> --rot-orig X,Y,Z --rot-new X,Y,Z --sun-dir X,Y,Z [--sun-color R,G,B] [--ambient R,G,B]`
  - Each render block's lightmap is rescaled **per channel** by how much more/less its surface now faces the sun — `factor = L(N_new) / L(N_orig)` where `L(N) = ambient + sunColor·max(N·sunDir, 0)`. This preserves the original bake's shadow/ambient-occlusion detail while approximating the new orientation, instead of flattening it to pure direct light. Block normals are derived from triangle geometry (type-3 meshes store UV data in the per-vertex normal slot, so face normals are used). Factors are clamped (`<=4x`, denominator floored) so a previously-shadowed surface can't blow out to white.
  - An identity rotation (`rot-orig == rot-new`) is a guaranteed no-op (every factor is exactly 1.0), covered by a regression test.
  - The CLI overwrites the `.olm` files in `lightmaps_unpacked/` in place; this
    operation is intentionally not exposed through MCP. Repack
    `lightmaps.res` separately (`res repack`, below) for the game to pick them
    up.
- **`res repack` command** — `igi1conv res repack <orig.res> <dir> -o <out.res>` rebuilds a `.res` preserving each original entry's EXACT name, swapping in updated bytes for any entry whose basename matches a file in `<dir>` (all others kept verbatim). This is the safe way to write edited lightmaps back into `lightmaps.res`: plain `res pack`/`--prefix` derive entry names from the on-disk folder layout and cannot reproduce the game's nested `missions/location0/levelN/lightmaps/objNNN.olm` names from a flat `lightmaps_unpacked/` dir. An edit-free repack reproduces the source `.res` byte-for-byte (regression-tested).

### Tests
- `tests/test_olm_writer.cpp` — OLM writer round-trip (pixel-identical through PNG), from-png with/without template, missing-output usage error, and the recalc identity-rotation no-op invariant. All spawn the built `igi1conv.exe` against the corpus and skip cleanly when `IGI_GAME_PATH` is unset.

### Changed
- **Version bumped to 1.11.0** (minor: OLM writer + lightmap recalc).

## [1.10.0] - 2026-06-24

### Added
- **Lightmap Support for MEF Type 3 Models**
  - Parse lightmap UV coordinates (UV2 channel) from MEF Type 3 vertex data (XTRV bytes +32..+39)
  - Resolve bound `.olm` lightmap files from decompiled `objects.qsc` Task_New trees
  - Support reused model IDs with disambiguation: when a model is placed at multiple locations with different baked lightmaps, the GUI shows a picker
  - Apply Lightmap right-click menu on `.mef` files (Type 3 only) with OpenGL shader blending
  - **OLM Format** — new `olm` command for working with lightmap files:
    - `igi1conv olm info <file.olm>` — print dimensions and file size
    - `igi1conv olm convert <file.olm> -o <out.png|out.tga>` — export lightmap to image
  - **OLM Documentation** — comprehensive file format reference in `game_file_formats.md` with binding resolution, vertex layout, and directory structure
  - **`lightmap` CLI command** — model id alone is ambiguous (the same `.mef` can be placed at many locations, each with its own baked lightmap), so the binding parser now also captures each placement's Task_New id and X/Y/Z position:
    - `igi1conv lightmap list --model <id> --qsc <objects.qsc>` — list every placement of a model bound to a lightmap (task id, name, position, logical id)
    - `igi1conv lightmap resolve --model <id> --qsc <objects.qsc> [--task-id <id> | --pos X,Y,Z]` — resolve to one placement (exact task id match, or nearest by Euclidean distance to a given position) and print its `.olm` file paths. Exits 4 (ambiguous, lists candidates) if the model has multiple placements and no disambiguator is given.

### Fixed
- **Lightmap Binding Resolution** — parser now resolves bindings at the nearest enclosing Task_New tree, not the outermost Container wrapper. The decompiled QSC nests multiple sibling Building tasks under a shared `Task_New(-1, "Container", "Buildings", ...)` parent; the old scanner treated the entire Container as one binding scope, causing every building's model IDs to be (wrongly) attributed to the first LightmapInfo found anywhere in the container. Fixed with bottom-up resolution: a node only binds its own + non-resolving children's model IDs if IT has a direct LightmapInfo child; otherwise they bubble unresolved to the parent.
- **Play Animation Menu Visibility** — "Play Animation" context menu on `.mef` files is now restricted to AI model ID range (`000_00_0`–`030_00_0`), matching the existing Apply-Animation-on-Model filter. Building/prop meshes outside that range have no bone animation sets and showing the action was misleading.
- **GUI Settings Persistence** — `ObjectsQscPath` in the `.ini` file was pointing to the wrong level during testing; fixed to match the configured level.
- **OLM Documentation Accuracy** — corrected the OLM binary format description in `game_file_formats.md` to match the actual parser (`OlmMainHeader`/`OlmLayerDescriptor`, version-float identified rather than a FourCC magic, RGBA8 pixels with R/B channel swap on export), replacing an earlier inaccurate "MLOI magic + packed ARGB" description.

### Changed
- **Version bumped to 1.10.0** (major: lightmap feature complete).

## [1.9.7] - 2026-06-24

### Added
- **Apply Animation on Model** — right-click any `.iff`/`.bff` to select a MEF model from the current level (IDs `000_00_0`–`030_00_0`) and play the IFF animation skinned onto the chosen model. Scans all subdirectories under the configured `globalModelsDir`.
- **Play Animation right-click on `.mef`** files — context menu entry for `.mef`/`.mex` files (model IDs `000_00_0`–`030_00_0` only) that loads the model and starts the animation set from `objects.qsc`.

### Fixed
- **MEF V-coordinate explosion in collision geometry** (`mef_native.cpp:ParseCollisionGeometry`) — synthetic UVs (`x*0.1, z*0.1`) for Type‑3 collision models now wrapped into `[0,1)` via `std::fmod` so exported OBJ V values stay in range.
- **IFF Play Animation loading wrong file** — `playAnimationForFile()` now guards against signal re-triggering by blocking `viewModeCombo` signals during the mode switch.
- **Animation transport buttons missing** — `iffMediaBar->show()` added in `onAnimationPlayClicked()` so Play/Pause/Step‑Back/Step‑Forward appear when playing a QSC‑based animation.
- **`applyIffOnModel` MEF file not found** — replaced the broken path reconstruction (`searchDir + "/" + chosen + ext`) with a persistent `modelIdToPath` map built during the directory scan, so models in subdirectories are found correctly.
- **Test corpus picking text `.IFF` files** — `FindCorpusFileByRegex()` now skips files starting with `\r\n\\` (decompiled IFF text), preventing 6 IFF tests from failing on invalid binary input.
- **UV tests picking Type‑3 lightmap models** — `MefExportObjHasRealUvs` and `MefExportVFlipMatchesModelType` now use `FindCorpusMefOfModelType(1/0)` to avoid Type‑3 lightmap files whose UV range spans `-11..1`.

### Changed
- **Version bumped to 1.9.7**.
- **Model ID range filter** — `applyIffOnModel` limits the selectable model IDs to `000_00_0`–`030_00_0` to match the game's core HumanSoldier set.
- **IFF context menu** now always shows "Play Animation" at the top, followed by "Apply Animation on Model...", then the convert/decompile/export actions.

## [1.9.6] - 2026-06-23

### Changed
- **Audio cache directory now falls back to the "Cache Folder" setting**
  when no explicit `AudioCacheDir` is set.  Resolution order:
  `Settings > Audio Cache Folder...` (per-setting), then
  `Settings > Cache Folder...` (the existing temp dir shared with
  textures / models), then `<temp>/igi_audio_cache` (default).
  This means a user who only picked `Settings > Cache Folder` once
  no longer sees audio land in `C:\Users\...\AppData\Local\Temp\...`.
- **Right-click "Convert to .wav (Windows PCM)..."** now opens a
  standard `Save As` dialog.  The user picks the destination file
  (and folder), the file lands wherever they pointed, no
  cache.  `Play in default media player` and the Audio mode
  toolbar still use the SHA-256-keyed audio cache so re-plays are
  instant.

### Added
- **New `wav` CLI tests** (in addition to the existing 14):
  - `WavConvertDirHonoursNoRecursive` — `--no-recursive` flag works.
  - `WavConvertDirEmptyIsNotAnError` — empty source dir exits 0.
  - `WavConvertAutoDerivesWavInDir` — `-o <dir>` auto-derives
    `<src-stem>.wav` inside the directory.
  - `WavConvertRejectsMp3` — `.mp3` output is refused with rc=1
    and no file produced.
  - `WavConvertRejectsUnknownExtension` — `.flac`, `.ogg`,
    `.txt`, `.bin` all refused with rc=1.
- **Version bumped to 1.9.6** (minor; full audio flow change).

## [1.9.5] - 2026-06-23

### Added
- **WAV (IGI audio) decoder** — new `wav` command reads the proprietary
  InnerLoop `ILSF` `.wav` files shipped with the game and decodes the two
  RAW methods to standard PCM `.wav` that plays in any Windows media
  player. MP3 output is also available by routing the intermediate WAV
  through the LAME encoder (auto-detected on `PATH`, common install paths
  probed, or supplied via `--lame`).
  - `igi1conv wav info <file>` — prints signature, method, sample width,
    channels, frame rate, frame count, duration, and a per-method
    "supported: yes / no" line.
  - `igi1conv wav convert <file> -o <out.wav|out.mp3>` — single file;
    the output format is chosen by `-o`'s extension (`.wav` and `.mp3`
    are the only valid outputs). If `-o` is a directory (or has no
    extension), the output basename is derived from the input basename;
    `--mp3` forces `.mp3` in that case.
  - `igi1conv wav convert-dir <src_dir> -o <dst_dir> [--mp3] [--lame PATH]`
    — recursive walker (opt out with `--no-recursive`) that converts
    every `*.wav` it finds, preserving the directory tree under `dst_dir`.
    Per-file failures are reported inline; the batch exits 0 when at
    least one file is converted, 3 only when every input failed.
  - **All four methods now decode**: methods 0 (`RAW`) and 1
    (`RAW_RESIDENT`) copy the payload straight to PCM; methods 2
    (`ADPCM`) and 3 (`ADPCM_RESIDENT`) run the new 4-bit IMA/Intel
    ADPCM decoder (`igi1conv::wav::decode_ima_adpcm` in
    `igi1conv/wav_adpcm.h`) with initial state `predictor=0,
    step_index=0` - matching the `audioop.adpcm2lin(data, 2, None)`
    call from the Python dconv reference at
    `D:\IGI-Tools\GM_123\IGI MEF CONV\tools\dconv\format\wav.py`.
- **GUI integration for the `wav` command** — right-click on any `.wav`
  in the file tree now offers:
  - **Play in default media player** — converts the file to a sibling
    `<name>.playback.wav` and hands the URL to `QDesktopServices`, so
    the OS picks the user's preferred audio app (WMP / VLC / foobar).
  - **Convert to .wav (Windows PCM)** — saves a sibling
    `<name>.wav` (standard PCM, playable on any Windows PC, no extra
    deps). Overwrite prompt included.
  - **Convert to .mp3** — saves a sibling `<name>.mp3`. Errors from
    LAME (if not on `PATH`) are surfaced in a modal message box.
  - **Info** — runs `wav info` in the text viewer.
  Right-click on a *folder* that contains `.wav` files adds a
  **Batch Convert N .wav file(s) in Folder** submenu with separate
  entries for `.wav` and `.mp3` output.  The folder scan is
  **recursive** so right-clicking a parent folder that only contains
  wav files in subfolders still surfaces the option, and the
  destination folder is picked at click time.
- **`tex to-spr` CLI subcommand** — convert any PNG / TGA / BMP / JPG
  (or existing `.tex`/`.spr`/`.pic`) to a LOOP v11 `.spr` with mode 3
  (ARGB8888, full alpha).  Mirrors `to-png` / `to-tga`; supports
  `--resize W H` and the same `-o` defaulting.
- **Shared LOOP encoder** — `TEX_WriteLOOP` in `source/parsers/tex_parser.{h,cpp}`
  is the single source of truth for "pack RGBA into a `.tex`/`.spr`/`.pic`".
  Both `tex to-spr` and the GUI's `saveAsTex` (used by `Save` /
  `Convert to TEX` / `Convert to .spr`) call it, so the CLI and GUI
  produce bit-identical output.
- **Image-editor Flip H / Flip V buttons** — new `↔ Flip H` and
  `↕ Flip V` toolbar buttons in the texture viewer.  The flip is
  baked into `currentImage`, so the next `💾 Save` writes the flipped
  pixels (no separate "commit" step).  `⟳ Reset` still goes back to
  the pristine file as loaded from disk, so a flip can always be
  undone.
- **Bundled LAME 3.100 MP3 encoder** — the old "shell out to
  `lame.exe`" path is replaced with an in-process `lame_enc.dll`
  load (dynamically linked via `LoadLibrary`, no extra Qt module).
  The DLL is downloaded from
  <https://www.rarewares.org/mp3-lame-bundle.php> into
  `third_party/lame_enc.dll` and the CMake post-build step copies
  it next to `igi1conv.exe`.  No external binary, no PATH / quoting
  mess, no "LAME not found" install prompt.
- **Audio Cache Folder setting + SHA256-keyed cache** — new
  `Settings > Audio Cache Folder...` (defaults to
  `<temp>/igi_audio_cache`) plus `Settings > Clear Audio Cache`.
  Every right-click audio action (Play / Convert to .wav /
  Convert to .mp3) and the Audio mode "play this file" path now
  writes to this directory using a content-addressed filename
  (`<sha256>_<mtime>.<ext>`); re-opening the same source is
  instant.  The legacy "sibling <name>.wav" location is detected
  and a one-shot info dialog tells the user where the new cache
  lives.
- **ADPCM warmup skip** — the IMA ADPCM decoder now drops the
  first 16 decoded samples (~0.7 ms at 22 kHz) and replaces them
  with silence, eliminating the audible "click" the initial
  `0xF / 0xF / 0x7 / 0xF` nibble run produced with state
  `(0, 0)`.  The rest of the file decodes unchanged.
- **GUI: Audio mode (Mode 6)** — the Mode combo now has an `Audio`
  entry.  Clicking any `.wav` in the file tree auto-routes to Audio
  mode and runs `igi1conv wav convert` to a sibling
  `<name>.playback.wav` (standard Windows PCM), which is loaded into
  an in-process MCI music player.  The audio bar has the standard
  transport controls — `⏮` restart, `⏪` -5s, `▶ / ⏸` play-pause-resume,
  `⏹` stop, `⏩` +5s, `⏭` end — plus a clickable scrubber, a
  `m.mmm / m.mmm s` time readout, and a volume slider.  Already-standard
  `.wav` files (any `RIFF` header) are loaded directly without
  re-conversion.  No new Qt modules are required — playback is
  backed by `winmm` (MCI) which is always present on Windows.
- **"Convert to .spr" right-click option** — alongside the existing
  "Convert to TEX", PNG / TGA / BMP / JPG files in the file tree
  can now be converted to a sibling `.spr` (ARGB8888) with one click.

### Tests
- New `tests/test_igi1conv_wav.cpp` with 14 end-to-end cases: header
  parsing for all four methods, mono and stereo round trip, ADPCM
  refusal, bad-signature rejection, missing-file exit code, directory
  walker with mixed good/bad inputs, `-o <dir>` extension inference, and
  the MP3 path (auto-skipped when `lame.exe` is not on `PATH`).
- New `TexToSprFromTex` (corpus-dependent) and `TexToSprFromSynthesizedTga`
  (corpus-free) in `tests/test_igi1conv_commands.cpp`.  The synthesized
  TGA test creates a 4x4 red TGA, runs `tex to-spr`, and verifies the
  resulting `.spr` has the expected LOOP v11 header (mode 3, 4x4,
  depth 4) and a total size of 96 bytes (32-byte header + 64 bytes
  of ARGB8888 pixels).
- Updated `VersionFlagReportsOneNineFour` / `HelpReportsOneNineFour`
  to the new 1.9.5 sentinel.

### Fixed
- **`wav convert-dir` exit code on partial failure**: a batch with some
  per-file failures used to return 3 even when most files converted
  successfully.  It now exits 0 when at least one file was converted
  and 3 only when every input failed, matching the standard "partial
  success is not a fatal error" convention used elsewhere in the
  project.

### Changed
- **Version bumped to 1.9.5**.
- `docs/SUPPORTED_FORMATS.md` and `docs/COMMANDS.md` now document the
  `wav` command, the `tex to-spr` subcommand, and the new image-editor
  flip controls; `docs/ISSUES.md` is updated to mark the RAW decode as
  done and split the ADPCM encode/decode work into separate open items.

## [1.9.4] - 2026-06-22

### Added
- **Animation mode (Mode 6)**: New GUI mode that plays IFF bone animations on textured 3D MEF models. Gated by Settings > Animation toggle. Includes:
  - QSC object parser (`qsc_object_parser.*`) that reverse-engineers decompiled `objects.qsc` to extract HumanSoldier Task_New entries with model ID, bone hierarchy, and stand animation.
  - Auto-detection of `objects.qsc`, `common/ANIMS` folder, and `LEVEL/models` folder when a level is selected (Settings > Level).
  - Animation panel with Model dropdown, Animations listbox, Play button, Loop checkbox, and FPS input textbox (configurable 1-120 FPS).
  - Skeletal skinning that deforms the textured MEF mesh using IFF bone transforms each frame (30+ FPS).
  - Right-click "Play Animation" on binary .MEF/.MEX files.
  - `P` key to toggle rest-pose skinning for debugging; `B` key to toggle bone skeleton overlay.
  - Full 33-bone mapping verification logged to detect MEF/IFF skeleton mismatches.
- **`ComputeBoneWorldPositions()`** exposed in `mef_native.h` for GUI skinning.

### Fixed
- **Animation root-offset mismatch**: IFF places root at (0,0,0) but MEF mesh uses (0,0,3990.4). The skinning now adds the root translation back so the deformed model stays in the MEF viewer frame, preventing detached body parts.
- **Animation playback speed**: Timer increment was hardcoded at 16ms per tick regardless of actual timer interval (33ms). Now uses `1000.0f / fps` for correct speed at any FPS.
- **Bone overlay visibility**: Bone skeleton now renders on top of the 3D model (depth test disabled) with larger joint dots (radius 0.03 vs 0.012). B key now also calls `updateIffSkeleton()` so overlay appears immediately.

### Changed
- **Version bumped to 1.9.4**.
- **Animation FPS** now configurable via a QLineEdit in the Animation panel (default 30).

## [1.9.3] - 2026-06-22

### Changed
- **3D graph toolbar placement**: The graph toolbar (Node / -Node / Nodes / Links / Total Nodes / Total Links / Reset) was previously rendered BELOW the 3D viewer. It is now created BEFORE the modelViewer in the constructor and added to `rightLayout` between the view-mode row and the modelViewer, so the buttons sit on top of the 3D rendering surface - matching the layout pattern used by the IFF media bar and the image editor toolbar.

## [1.9.2] - 2026-06-22

### Added
- **3D graph toolbar**: New toolbar under the 3D viewer, shown only when a `graph*.dat` file is loaded. Mirrors the layout pattern used by the IFF media bar / image editor toolbar. Includes:
  - **+ Node / - Node** buttons to resize the red node cubes from the GUI (no keyboard shortcuts required).
  - **Reset** button to restore the default node scale.
  - **Nodes** and **Links** check-box toggles to show or hide the corresponding layer.
  - **Total Nodes: N** and **Total Links: N** read-only labels that update automatically every time a graph is loaded. Wired through a new `ModelViewer::onGraphLoaded` callback so the toolbar (in `MainWindow`) stays in sync with the `GraphFile` state (in `ModelViewer`).

### Changed
- **.DAT context menu** now detects `graph*.dat` (graph.dat, graph1.dat, graph2.dat, graph3.dat, and any filename starting with "graph") by base name and presents a dedicated context menu instead of the regular level-DAT (model/texture bindings) menu. The new graph menu offers: **View Graph in 3D**, **Export to JSON**, **Export to Table (.md)**, **Info**, **Dump**. All other `.dat` files (level1.dat, levelX.dat) keep the existing Convert / Export / Info options.
- **Export to Table (.md)** captures both `graph info` and `graph dump` into a structured Markdown report that lists every node, every link, and aggregate stats in fenced code blocks - a single human-readable summary of the entire AI navigation graph.
- **"Pack to .res archive"** renamed to **"Pack to Archive"** and now auto-saves to a sibling `<folder>.res` when one exists, without prompting the user with a save dialog. The folder's prefix is always `folderName/` (no more QInputDialog prompt), which matches how the IGI engine indexes texture resources.

### Fixed
- **Image-to-TEX conversion error** used to log the INPUT path on error (`Failed to convert image to TEX: <input>`), which was misleading - the conversion writes to a sibling `.tex` file. Now logs `input -> output` and shows a `QMessageBox::critical` with the actual error. `saveAsTex()` now also early-rejects images larger than 65535 px on either axis (the TEX v11 header stores width/height as `uint16`) and ensures the destination directory exists before opening the file.

## [1.9.1] - 2026-06-22

### Fixed
- **MEF texture orientation (the "upside down / sideways" bug)**: The `.mef → .obj` and `.mef → .mef` (via `mef build-rigid`) export paths now preserve V verbatim for every model type, matching the orientation the IGI engine and the GUI 3D viewer use. The 3D viewer (the live preview in `igi1conv.exe`) and the exported `.obj` / `.mef` now render identically for the same source `.mef`. Concretely, the original `015_01_1.mef` (Type 0 rigid), `001_01_1.mef` (Type 1 bone), and `404_01_1.mef` (Type 3 lightmap) all render right-side-up in the viewer **and** in the exported files. The 82% of MEFs that are Type 3 (lightmap) no longer come out flipped, and bone-model face textures are no longer upside down.

### Changed
- **Centralised V-flip decision** in `MefVToObjV(v, modelType)` (`source/parsers/mef_exporter.cpp`). The old per-call-site `1.0f - v.uv.y` / `1.0f - uv[]` literals have been removed and replaced with a single helper. The GUI 3D viewer reads `v.uv.y` directly with no V-flip helper. The rule is now the identity for every model type — no more special-casing of Type 1 (bone) vs Type 3 (lightmap) vs Type 0 (rigid).
- **Removed hardcoded `D:\IGI1` path** from the test suite. The corpus location is now fully user-controlled via the new `--game-path=PATH` / `--corpus=PATH` CLI flag or the `IGI_GAME_PATH` env var. When neither is set, `CorpusDir()` returns "" and tests `GTEST_SKIP()` instead of failing. `tests/igi1conv_test_util.h` no longer mentions a Windows-specific path.

### Added
- **Comprehensive MEF V-flip regression suite** in `tests/test_igi1conv_commands.cpp`:
  - `MefExportVFlip_Type0_Rigid_DoesNotFlipV`: rigid models preserve V.
  - `MefExportVFlip_Type1_Bone_DoesNotFlipV`: bone models preserve V.
  - `MefExportVFlip_Type3_Lightmap_DoesNotFlipV`: lightmap models preserve V.
  - `MefExportVFlip_AllTypesRespectRule`: sweeps up to 64 MEFs in the corpus, asserts every model obeys the per-type rule.
  - `MefExportVFlip_BinaryAndTextPathsAgree`: binary MEF → OBJ and text MEF → OBJ produce identical V for the same source MEF.
  - `MefExportVFlip_NoStrayOneMinusVLiterals`: structural test that greps `mef_exporter.cpp` for stray `1.0f - v.uv.y` / `1.0f - uv[` literals outside the helper, so the formula can't drift between call sites.
  - `MefViewerDoesNotFlipV`: structural test that the GUI 3D viewer never re-introduces a V-flip helper.
  - `MefExportObjHasRealUvs` and `MefExportBinaryAndTextObjUvsMatch`: lock in that the OBJ export has real per-vertex UVs and that the binary and text paths agree.
- **New test util helpers** in `tests/igi1conv_test_util.h`:
  - `FindCorpusMefOfModelType(int wantedType, const std::string& namePattern = "")` — finds a MEF with a specific `model_type` (0 = rigid, 1 = bone, 3 = lightmap) in the corpus.
  - `GetMefModelType(const std::string& mefPath)` — parses `model_type` from `mef info` output.
  - `FirstVtFromObj(const std::string& objPath)` — reads the first non-(0,0) `vt` line from an OBJ.
- **Test corpus** (`D:\IGI1\igi1conv_test_suite` — outside the repo, no hardcoded path committed): added `320_01_1.mef` (Type 0 rigid) and `404_01_1.mef` (Type 3 lightmap) so the per-type tests have files to exercise. The pre-existing `0_000_01_1.mef` (Type 1 bone) covers the third category.

## [1.9.0] - 2026-06-22

### Added
- **Native IFF Decompiler** (`iff decompile`): Splits a binary IFF skeleton-animation file into a human-readable `.IFF` text representation of the bone skeleton plus a per-animation `anims_<id>/anim_NNN.IFF` text file. Mirrors the layout produced by the old dconv `IGI1_iff.py` reference.
- **Native IFF Writer** (`iff create`): Reads a directory of BEF text files (output of `iff convert`) **or** the decompile text format (output of `iff decompile`) and assembles a single binary `.iff` file. The C++ round trip `iff convert` → `iff create` reproduces the original byte-for-byte for every IFF in the corpus.
- **`iff rebuild <src.iff> <out.iff>`**: One-shot IFF → BEF → IFF round trip via a temp directory.
- **`iff emit-qsc <dir> <out.qsc>`**: Generates the `Anims.qsc` for a folder of `.BEF` scripts (the file `IGI1_convert.py` produced in the legacy toolchain).
- **`iff export-gif <file.iff> <out.gif> [w] [h] [fps]`**: Headless software renderer that turns the IFF skeleton animation into a looping `GIF89a`. Uses an orthographic 3/4 projection, orange bones, cyan/white joint discs, and the `gif.h` NETSCAPE2.0 extension. Replaces the old OpenGL-based GUI export.
- **GUI IFF context menu**: New right-click actions on IFF files (`Decompile to text + per-anim IFFs`) and on folders of BEF scripts (`Create IFF from .BEF scripts`, `Generate Anims.qsc`). The `Export Animation as GIF...` GUI action now routes through the same `iff export-gif` headless renderer used by the CLI, so it works without the OpenGL viewport being visible.

### Changed
- **Removed `tools/` folder entirely**: the legacy `dconv/` (Python) and `gconv/` (3DS Max plugin) helpers that igi1conv used to shell out to have been deleted. `igi1conv.exe` is now a fully native, standalone C++ project with zero external runtime dependencies.
- **`igi1conv iff help`**: The help text now lists every IFF subcommand (`info`, `test`, `decompile`, `convert`, `create`, `rebuild`, `emit-qsc`, `export-gif`).
- **`iff_parser`**: The parser no longer aborts on the `>100MB` size sanity check when the chunk tag is `FORM`, because the IFF root/inner FORM sizes are intentionally "broken" (a small value that the engine walks past by tag). The 6-field `BOED` layout (2 int32 + 4 float32) is now decoded correctly with `bone_id` replacing the old misplaced `param` field.
- **BEF event field**: `BefEvent.param` is renamed to `BefEvent.bone_id`; `TriggerData(..., bone, px, py, pz)` writes/reads it correctly. The BEF writer's IFF writer now writes 24-byte events with all 6 fields.
- **BOSH object_id**: the IFF writer derives the BOSH object id from the leading digits of the first BEF filename (e.g. `003_anim_002.BEF` → object_id 3) so rebuilt IFFs report the same model id as the source.
- **`gif.h`**: every function in the header is now `static` (gated by `GIF_H_API`) so the header can be `#include`d from multiple translation units (`gui_main.cpp` + `iff_gif_exporter.cpp`) without ODR violations.
- **GUI**: dropped all references to the legacy `.bff` format and the "Batch Convert IFF/BFF" wording.

### Fixed
- **IFF writer byte order**: chunk sizes (FORM, BOSH, PLST, TLST, BOAH, BOEH, BOEH, BOED, BOTH, BOTD, BORH, BORD) are written big-endian, matching the IFF wire format. All multi-byte data values remain little-endian. The original 0-clips / wrong-object-id bugs are gone.
- **BEF parser tokenisation**: line comments (`//`) and the `,;()` separators are now stripped correctly so `AnimInit("003_anim_002",0,3741,0);` parses to the right four-arg form.

### Technical Details
- New files: `source/parsers/iff_bef.{h,cpp}`, `iff_decompiler.{h,cpp}`, `iff_writer.{h,cpp}`, `iff_gif_exporter.{h,cpp}`.
- 8 new gtest cases (`IffInfo`, `IffConvert`, `IffRebuild`, `IffDecompile`, `IffCreateFromBefs`, `IffExportGif`, `IffRoundTripSizeMatches`, `IffDecompileCreateRoundTrip`), all driven against `D:\IGI1\common\ANIMS` (000/001/002/003/005/006.IFF).
- End-to-end verified for all six IFFs: `iff convert` + `iff create` reproduces the original byte-for-byte (same `604088`/`1565672`/etc. file sizes); `iff decompile` + `iff create` round-trips successfully (size matches to within ~700 bytes due to the FORM-size convention); `iff export-gif` produces valid `GIF89a` files for all six.

---

## [1.8.0] - 2026-06-21

### Added
- **3D Graph Editor Viewport**: Implemented interactive 3D viewport rendering for graph files (`graph*.dat`), displaying graph nodes as 3D red cubes and visual connections as lines.
- **Dynamic Node Scaling**: Implemented automatic dynamic scaling for graph nodes based on the spatial span of the loaded map.
- **Interactive Node Scale Controls**: Added hotkeys to manually scale nodes: `Ctrl +` / `Ctrl -` to increase/decrease node size, `Ctrl + Mouse Wheel`, and `Middle Mouse + Mouse Wheel`.
- **View Toggles**: Added toggles (keys 'N' for Nodes, 'L' for Links) to hide/show nodes and links.
- **Improved View Defaults**: Configured default view rules where any `.dat` file with `graph` in its name defaults to '3D' view, and `.qvm` files default to 'Text' view.
- **View Mode Renaming**: Renamed '3D View' to '3D', and 'Image View' to 'Image' mode in the GUI.

### Fixed
- **Large Graphs Visibility**: Corrected sizing issue where nodes were too small to see in large graphs with 30-100+ nodes.

---

## [1.7.0] - 2026-06-13

### Added
- **Recursive ATTA Support in Exports**: Both "Export to OBJ" (`mef export-bundle`) and "Build Rigid Model" (`mef build-rigid`) now walk the full attachment hierarchy, automatically merging all sub-models and their textures into a single output
- **Texture Resolution from DAT Files**: `mef build-rigid` now accepts `--dat` and `--texdir` flags to resolve and convert `.tex` textures to `.tga` format with correct material slot offsets
- **Export Merged Geometry to Bundle**: New `ExportMergedToObjBundle()` exporter API for writing pre-merged geometries with resolved texture names
- **GUI Build Rigid Command Enhancement**: The GUI now automatically passes level DAT and texture directory to the build-rigid command when configured in Settings

### Fixed
- **Bone Model Rendering in GUI**: Character bone models (type1) now render at correct world positions in the 3D viewer, matching the OBJ exporter output. Previously used bone-local positions, causing visible distortion and incorrect placement
- **Texture Fallback Chain**: Improved texture discovery with priority: DAT+texDir (game assets) → local `mat_N.*` files (fallback for custom textures)

### Changed
- **MEF Export Bundle**: Now recursively merges ATTA children instead of exporting only the top-level model
- **Build Rigid Model**: Geometry from all ATTA children is inlined; no external ATTA references remain in the output

### Technical Details
- Fixed `MergeGeometryRecursive()` to use `v.pos * 40.96f` for bone models (world-positioned) instead of `v.rawPos` (bone-local), aligning with MEF native parser behavior
- Material slot offsets correctly propagated through merged hierarchy
- Portal and TAMC records properly offset and merged from all attachment levels

---

## [1.6.0] - 2026-05-15

### Added
- Full support for AI character bone models (Type 1 skeletal models)
- DNER/ECAF split face rendering for bone models
- Bone vertex weights and indices parsing
- Upside-down UV mapping correction for bone model textures

### Fixed
- ATTA parsing for recursive attachments
- MEX (text MEF) file support in export and compilation

---

## [1.5.0] - Earlier

See commit history for previous releases.
