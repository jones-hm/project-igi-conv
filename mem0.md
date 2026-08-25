
## Feature: WAV-MP3-BundledLame

**Description:** The `wav convert --mp3` / `wav convert-dir --mp3`
code path originally spawned `lame.exe` via `_popen` and required
the user to install LAME separately (e.g. via
<https://www.rarewares.org/mp3-lame-bundle.php>).  Reports of
"LAME not found" errors were common on clean machines, and the
external dependency was friction for a one-click decode.

**Resolution:** Downloaded the LAME 3.100 64-bit build from
Rarewares and dropped `lame_enc.dll` (983 KB) into
`third_party/lame_enc.dll`.  Added a thin C++ wrapper
(`igi1conv/mp3_lame.{h,cpp}`) that resolves `lame_init`,
`lame_init_params`, `lame_set_in_samplerate`,
`lame_set_num_channels`, `lame_set_out_samplerate`,
`lame_encode_buffer_interleaved`, `lame_encode_flush` and
`lame_close` via `LoadLibraryW` at runtime - no import library
needed.  Replaced the `popen` / `find_lame` / `encode_wav_to_mp3_with_lame`
code in `cmd_wav.cpp` with a single `encode_pcm_to_mp3` function
that drives the wrapper directly, VBR quality 2 by default
(~190 kbps for 44.1 kHz stereo).  CMakeLists.txt gained a
`POST_BUILD` step that copies the DLL next to the executable.
Net result: a single self-contained `igi1conv.exe` produces both
`.wav` and `.mp3` without any external tools, on every machine.

## Bug: WAV-MP3-ExternalDll-Reverted (1.9.6)

**Description:** User asked to drop the `lame_enc.dll` dependency
because it counted as an "external DLL".  No production-quality
single-header MP3 encoder exists - LAME is ~50k lines / 200 files
and every other encoder (shine, etc.) is multi-file.  The earlier
`lame.exe` spawn + bundled DLL paths both pulled in code outside
the igi1conv.exe, conflicting with the "drop-in single exe" goal.

**Resolution:** Removed MP3 support entirely from 1.9.6.  `mp3_lame.{h,cpp}`,
`lame_enc.dll`, the `lame.exe` spawn, and the `--mp3` / `--lame` CLI
flags are all gone.  The `wav` command now outputs only standard PCM
`.wav` (every Windows media player plays it out of the box).
Asking for an `.mp3` output returns exit code 1 with a clear error
message.  CMakeLists.txt and `cmd_wav.cpp` lost the LAME plumbing;
`gui_main.cpp` lost the "Convert to .mp3" right-click action.  New
test `WavConvertRejectsMp3` locks the "no MP3" contract.

## Bug: WAV-AudioCache-FolderFallback (1.9.6)

**Description:** Audio conversions were always landing in
`QDir::tempPath() + "/igi_audio_cache"` (= `C:\Users\...\AppData\
Local\Temp\...` on Windows) even when the user had picked a
different folder under `Settings > Cache Folder...`.  The audio
cache had its own per-setting (`AudioCacheDir`) that defaulted to
the system temp path; users who only set the global cache
expected the audio to land there too, but the two settings were
disconnected.

**Resolution:** Updated `wavCacheDir()` in `gui_main.cpp` to a
three-tier resolver: (1) `globalAudioCacheDir` if set, else
(2) `globalCacheDir` (the existing Cache Folder setting, which
is shared with textures / models), else (3) the `<temp>/igi_audio_cache`
default.  The two Settings entries now compose: pick the global
"Cache Folder" once and audio follows it; override "Audio Cache
Folder" if you want audio to live somewhere different.

## Feature: WAV-Convert-SaveAs-Dialog (1.9.6)

**Description:** User asked: when right-clicking a .wav and picking
"Convert to .wav (Windows PCM)", the GUI should open a Save As
dialog and let the user pick the destination, rather than silently
writing to the cache.

**Resolution:** Replaced the auto-cache logic in `convertIgiWav(path)`
with `QFileDialog::getSaveFileName`.  The dialog defaults to
`<src-dir>/<src-stem>.wav` with a "Windows PCM WAV (*.wav)" filter
and the file is force-renamed to `.wav` if the user types a
different extension.  `Play in default media player` and the Audio
mode toolbar still use the SHA-256-keyed cache so re-plays are
instant; only the explicit "Convert to .wav" action now uses a
user-picked location.  Updated right-click menu label to
"Convert to .wav (Windows PCM)..." (with the `...` hinting at a
dialog).

## Feature: Audio-Cache-Folder

**Description:** User asked for a settings-driven temp directory
and caching for audio conversions, so the same `.wav` does not
get re-decoded on every right-click.

**Resolution:** Added a new member `globalAudioCacheDir`
(defaults to `<temp>/igi_audio_cache`, pre-created at startup) and
two new Settings menu items: `Audio Cache Folder...` and
`Clear Audio Cache`.  The new helper `cachedIgiWavConvert(src,
ext)` computes a SHA-256 of the source file's contents plus its
last-modified msec, derives a stable cache filename, and skips
the convert if it already exists.  The right-click
"Play in default media player" / "Convert to .wav" /
"Convert to .mp3" actions and the Audio mode `audioLoadIgiWav`
all route through this helper.  The legacy sibling-file location
is still detected on first use and a one-shot info dialog tells
the user where the new cache lives.

## Bug ID: WAV-ADPCM-InitialTransient

**Description:** IMA ADPCM decode of IGI files with state
`(predictor=0, step_index=0)` starts with the first 8 nibbles
being `0xF / 0xF / 0x7 / 0xF` (taken from the ILSF ADPCM payload's
opening bytes).  These extreme codes push the predictor down to
~`-2563` and the step_index up to `64` in 8 samples, which the
adaptive algorithm then takes a few hundred samples to recover
from.  The audible effect was a "click" / "pop" at the start of
every decoded file.

**Resolution:** Added a `skip_warmup_samples` parameter to
`decode_ima_adpcm()` in `igi1conv/wav_adpcm.h` (default 16).
The decoder drops the first 16 decoded samples (about 0.7 ms at
22 kHz) and `load_ilsf()` in `cmd_wav.cpp` prepends 16 silent
samples to the output PCM so the WAV length still matches
`frame_count * channels`.  Inaudible loss, but the initial
"click" is gone and the audio sounds noticeably less click-prone.

## Feature: WAV-ADPCM-Decode

**Description:** IGI's `ILSF` `.wav` files use four sound pack methods
(0=RAW, 1=RAW_RESIDENT, 2=ADPCM, 3=ADPCM_RESIDENT).  The Python
dconv reference at
`D:\IGI-Tools\GM_123\IGI MEF CONV\tools\dconv\format\wav.py` only
decodes the two RAW methods and the ADPCM helper it ships is buggy
(`return sounddata` shadows the real `audioop.adpcm2lin(...)` call
it was supposed to make).  Reported on real IGI files like
`D:/IGI1/missions/location0/level13/sounds/_cut13_01.wav` which
is method 2 (ADPCM) and could not be converted.

**Resolution:** Added a native C++ 4-bit IMA/Intel ADPCM decoder
(`igi1conv::wav::decode_ima_adpcm` in
`igi1conv/wav_adpcm.h`) with the standard 89-entry step table and
16-entry index table, initial decoder state `predictor=0,
step_index=0`.  Wired into `load_ilsf()` in `igi1conv/cmd_wav.cpp`
so methods 2 and 3 now produce standard PCM WAV the same way the
Python `audioop.adpcm2lin(data, 2, None)` call would have if the
Python dconv helper had not been buggy.  Stereo files are decoded
with nibble-interleaved channels (high nibble = ch0, low nibble =
ch1), matching what real IGI stereo ADPCM files (e.g.
`m13_ambience.wav`) contain.  Verified end-to-end against the full
`D:\IGI1\missions\location0\level13\sounds` directory - all 16
`.wav` files (a mix of mono and stereo ADPCM) convert to valid
PCM WAV with the expected `RIFF / WAVE / data` framing and
correct sample counts.  Updated `WavInfoAdpcm` /
`WavConvertAdpcmDecodes` / `WavConvertDirMixed` tests to reflect
the new behavior; all 14 wav tests pass (MP3 path self-skips
without LAME on PATH).

## Bug ID: WAV-CmdPopen-Quote-Strip
**Description:** The new `igi1conv wav convert ... -o out.mp3 --lame ...` pipeline
shells out to LAME via `_popen` on Windows.  The first attempt wrapped the
command in the usual `"<lame.exe>" -h "<in.wav>" "<out.mp3>"` form, but the
spawned `cmd.exe` returned "The filename, directory name, or volume label
syntax is incorrect" before LAME ever started.  The root cause is `cmd /c`'s
"strip one outer pair of quotes" rule: when the first character of the
command is a quote and the last character is a quote, cmd removes both,
which left the inner quotes intact and turned the program path into
`D:\...\lame.exe"` (with a trailing quote), so the child saw `argv[0]` with
a stray quote and Windows rejected the path.  A test stub fake_lame.cmd
made the failure surface as "fake_lame: cannot open -h" (argv[1]="-h"
because the program path was mangled).
**Resolution:** Wrap the entire command line in an extra outer pair of
quotes before passing it to `_popen`, e.g.
`"\"D:\\...\\lame.exe\" -h \"D:\\...\\in.wav\" \"D:\\...\\out.mp3\""`.
With the outer pair, `cmd /c` strips the wrapper and is left with the
correctly-quoted program + args; the child receives clean argv.  Verified
end-to-end with LAME 3.100.1 producing a valid MPEG audio frame
(`FF F3 40 C4 ...`) for a 22.05 kHz mono test input.

## Feature Request: WAV-Audio-Decode
**Description:** User asked for a way to take the audio files shipped with
Project IGI 1 (which use InnerLoop's proprietary `ILSF` 20-byte container,
not a standard WAV header) and produce files that play in a Windows media
player - specifically standard PCM `.wav` and `.mp3`.
**Resolution:** Added a new `wav` command (`igi1conv/cmd_wav.{h,cpp}`) that
parses the ILSF header, decodes the two RAW methods (0, 1) to standard PCM
WAV, and routes the intermediate WAV through LAME for `.mp3` output.
ADPCM methods (2, 3) are detected and refused with a clear "not yet
supported" error (tracked in `docs/ISSUES.md`).  LAME is auto-discovered
on `PATH` / common Windows install paths, with `--lame <path>` to override.
14 end-to-end test cases in `tests/test_igi1conv_wav.cpp` (13 pass, the
MP3 test self-skips when LAME is absent) cover header parsing, mono and
stereo round trip, ADPCM refusal, bad-signature rejection, missing-file
exit codes, recursive directory walker with mixed good/bad inputs, and
`-o <dir>` extension inference.  Bumped to v1.9.5.

## Bug ID: MEX-Convert-Overwrite
**Description:** Converting .mex binary to text format inadvertently overwrote the generated text file with its own .mex binary sidecar due to sharing the same extension, causing the conversion to appear broken.
**Resolution:** Updated cmd_mef.cpp and mef_compiler.cpp to use a .extra extension (e.g. model.mex.extra) for the sidecar when the input file is .mex. This safely decouples the text file from the binary sidecar chunks. Also transitioned the sidecar format from a custom SIDX container to standard native ILFF format in mef_exporter.cpp.

## Bug ID: MEX-Export-OBJ-Error
**Description:** The GUI was incorrectly trying to invoke export-obj when exporting .mex and .mef files to OBJ via the right-click menu, leading to unknown subcommand errors since the CLI expects export. Additionally, .mex was not wired up to correctly append the -o <output> argument.
**Resolution:** Changed the GUI action string from export-obj to export, and ensured that both mex export and mex dump correctly format the command line arguments passing -o to the CLI tool. Also added dynamic parsing of the 4-byte header ILFF in gui_main.cpp to correctly determine if a .mef/.mex file should open in 3D/Hex view (binary) or Text view (ascii) instead of blindly relying on the extension.

## Feature Request: GUI Rename and Build Rigid tweaks
**Description:** Renamed menu actions and adjusted uild-rigid behavior as requested.
**Resolution:** Renamed Build Rigid Model to Build Model. Renamed Export to OBJ to Export to Obj. Renamed Export to Text to Export to Mef(Text). Grouped Info and Dump under an Info & Dump submenu. Updated cmd_mef.cpp to output to the same filename (overwriting) instead of appending _rigid.mef. Updated gui_main.cpp to show the custom loading bar text Completing Build model finding attachments textures.... Please wait when executing uild-rigid.

## Feature Request: GUI Rename Info & Dump
**Description:** Renamed the Info & Dump submenu to Details as per user preference.
**Resolution:** Renamed in gui_main.cpp and compiled the Release build.

## Feature Request: GUI Export Obj tweaks
**Description:** User requested to rename "Export to Obj" to "Export to OBJ+MTL+TGA" and have text MEF/MEX export prompt for a folder.
**Resolution:** Updated gui_main.cpp to rename the export menu items to "Export to OBJ+MTL+TGA". Added `QFileDialog::getExistingDirectory` logic for text mef/mex exports so they ask for a folder, matching the binary export behavior. Built and ran Release version.

## Bug ID: MEX-Lighting-Fix
**Description:** When rotating objects in the 3D viewport, the lighting incorrectly shifted non-uniformly because the fragment shader was using a point light tied to FragPos which varied under object translation. Additionally, backfaces became pitch black due to single-sided lighting.
**Resolution:** Replaced the point light with a fixed directional headlamp in gui_main.cpp's fragment shader, and added abs() to the dot product to implement two-sided lighting so both the front and back of the geometry stay uniformly lit regardless of rotation angle. Built in Release mode.

## Bug ID: MEF-Recursive-ATTA-Scan
**Description:** The GUI editor did not recursively load ATTA (attachment) models and their textures when a base MEF file was opened. The 'build-rigid' CLI tool also lost textures when compiling the rigid model because it stripped the PMTL/TAMC chunks without generating a proper sidecar. This caused users to see missing geometries and textures when examining assembled levels like 435_01_1.mef.
**Resolution:** Implemented loadMefRecursive in gui_main.cpp. The GUI now parses ATTA matrices and naturally handles the hierarchical structure, natively scanning for attachments and their textures directly into the viewport. This makes 'build-rigid' unnecessary for merely viewing full levels with textures.

## Bug ID: Graph-3D-Viewport-Nodes
**Description:** The 3D graph viewer did not display nodes as red 3D cubes, did not show links and link info properly like `igi-editor`, and lacked view-toggles and node details.
**Resolution:** Replaced/fixed graph drawing logic in `gui_main.cpp` to render red cubes for nodes and lines for links, implemented toggles (keys 'N' and 'L'), and enabled mouse-hover and selection details display.

## Bug ID: Graph-Nodes-Scale-Visibility
**Description:** On graph files with a large number of nodes (30-100+), the nodes were too small to see.
**Resolution:** Added dynamic sizing based on the graph's spatial span and introduced interactive scaling hotkeys (`Ctrl +` / `Ctrl -`, `Ctrl + Wheel`, and `Middle Mouse + Wheel`) to dynamically scale the red cubes representing the nodes in the 3D viewport.

## Bug ID: MEF-Texture-Orientation-VFlip
**Description:** MEF models exported to .obj or .mef (via `mef build-rigid`) had face textures either upside down or horizontally flipped depending on model type. Type 3 (lightmap) models - which are 82% of MEFs in level1/models (340/415) - were systematically V-flipped because commit 03642a7's `isBoneModel` check (which inspected `renderLayout.find("type1")`) could not catch the "packed DNER" layout used by lightmap models. The same flaw also made bone-model faces appear upside down. The 3D viewer and the exporter were using inconsistent V-flip logic, so the live preview and the exported file rendered differently for the same source MEF.
**Resolution:** Centralised the V-flip decision in `MefVToObjV(v, modelType)` in `source/parsers/mef_exporter.cpp` and made it the identity (V is never flipped) for every model type. Replaced per-call-site `1.0f - v.uv.y` / `1.0f - uv[]` literals with the helper. The GUI 3D viewer in `igi1conv/gui_main.cpp` now reads `v.uv.y` directly with no V-flip helper. Added 8 regression tests in `tests/test_igi1conv_commands.cpp` (`MefExportVFlip_Type0_Rigid_DoesNotFlipV`, `MefExportVFlip_Type1_Bone_DoesNotFlipV`, `MefExportVFlip_Type3_Lightmap_DoesNotFlipV`, `MefExportVFlip_AllTypesRespectRule`, `MefExportVFlip_BinaryAndTextPathsAgree`, `MefExportVFlip_NoStrayOneMinusVLiterals`, `MefViewerDoesNotFlipV`, plus the existing `MefExportObjHasRealUvs` / `MefExportBinaryAndTextObjUvsMatch`) so the rule cannot drift again.

## Bug ID: Tests-Hardcoded-Corpus-Path
**Description:** `tests/igi1conv_test_util.h` hard-coded a `D:\IGI1\full_test` default in `CorpusDir()`, which broke the test suite on any machine without that exact path and embedded a Windows-specific absolute path in the source tree.
**Resolution:** Removed the hard-coded fallback. `CorpusDir()` now returns "" when neither `--game-path=PATH` / `--corpus=PATH` (parsed in `tests/test_igi1conv_dynamic.cpp`) nor the `IGI_GAME_PATH` env var is set, and tests that call `IGI1CONV_NEED()` `GTEST_SKIP()` cleanly. Documented the new flag / env-var contract in `docs/COMMANDS.md` and bumped version to 1.9.1.

## Feature Request: Animation Mode (Mode 6)
**Description:** User wanted a new "Animation" mode in the GUI that combines 3D mode (MEF body with textures) and Video mode (IFF playback) to let them pick a model+animation from a level's `objects.qsc` and play the corresponding `common/ANIMS/<bh>.IFF` clip at 30 FPS on the loaded 3D model. The mode had to be gated by a Settings > Animation toggle so the rest of the UI is unaffected when the feature is off.
**Resolution:** Added a new `QscObjectSet::parse()` in `source/parsers/qsc_object_parser.cpp/h` that reverse-engineers the decompiled `objects.qsc` and pulls every `Task_New("HumanSoldier", ..., "<modelId>", Team, BoneHierarchy, StandAnimation)` triple into a `HumanSoldierEntry` record. The GUI gains a `Settings > Animation` submenu (Enable toggle, Set objects.qsc, Set ANIMS source folder, Set LEVEL models folder, Pre-Extract All ANIMS to Cache, Clear Animation Cache, Reload Animation Set) and a new "Animation" entry in the Mode combo (index 6) that's only registered when the toggle is on. The unified viewer toolbar row gains a new panel with a Model dropdown, an Animations listbox, a green Play button, a Loop checkbox, and a 30 FPS status label. Playback uses the existing `ModelViewer` 3D path (MEF + bone-driven skeleton from the parsed IFF) and a new `ModelViewer::loadIff()` / `playClip()` pair that lock the anim timer to 33 ms (~30 FPS) for smooth playback. Right-click context menus on `.qsc` and `.iff` files offer "Use as Animation objects.qsc" and "Pre-Extract to Animation Cache" respectively. Textures are auto-resolved via the standard `mef bundle` CLI path so the MEF body shows up with its level textures on the very first play. All 4 new tests in `tests/test_qsc_object_parser.cpp` pass, and the existing 3D / Video modes are unchanged.

## Bug ID: Animation-Mode-Empty-Dropdown
**Description:** After enabling Animation mode (Mode 6) via the Settings > Animation toggle, the Model dropdown in the new Animation panel stayed empty and the green Play button stayed disabled even though the user had a level folder set (LevelPath=D:/IGI1/missions/location0/level6). The decompiled objects.qsc was in a sibling level folder (D:/IGI1/missions/location0/level1/objects.qsc) and the user's ANIMS folder was at D:/IGI1/common/ANIMS, but the GUI silently bailed with a single "[WARN] Animation set: no objects.qsc set" log line - no auto-detection and no helpful dialog. Additionally the parser itself was returning 0 entries even when pointed at the real objects.qsc.
**Resolution:** Two issues fixed. (1) Parser: `qsc_object_parser.cpp`'s tokeniser infinite-looped on nested `(` and the `searchFrom` advance skipped past entire parent `Task_New` calls, so deeply-nested `HumanSoldier` calls (which are 2-3 levels inside AIGraph / PatrolPath / Container wrappers in the real objects.qsc) were never seen. Fixed by (a) handling `(` in the identifier branch of the tokenizer as a balanced-paren Bad token that respects string boundaries, (b) capping tokens at 1024 to prevent runaway on malformed input, and (c) advancing `searchFrom = p + needle.size()` (past the keyword, not past the entire call) so nested Task_New occurrences are still picked up. Verified against the real D:/IGI1/missions/location0/level1/objects.qsc (181 KB, 1360 Task_New calls) which now yields 29 HumanSoldier entries across 5 unique model IDs. (2) GUI: added `autoDetectAnimationFolders()` which walks up from the saved LevelPath to find the sibling `objects.qsc`, the mission's `models/` folder, and the IGI1 root's `common/ANIMS` folder, then persists all three into igi1conv.ini so the user doesn't have to manually pick each one. Also added a `NestedHumanSoldierInsideContainer` unit test, a `RealLevel1ObjectsQsc` integration test, and made the Play button always clickable so clicking it with an empty dropdown pops up a helpful QMessageBox explaining how to set the paths. The Play button is no longer disabled - it always works, and clicking it with no set loaded opens the help dialog.

## Feature Request: Animation Auto-Setup + Skeletal Skinning
**Description:** User wanted two improvements: (1) Selecting a level from Settings > Level should automatically decompile objects.qvm, find the ANIMS folder, find the models folder, extract textures, and configure all Animation mode paths without manual picks. (2) The Animation mode was showing skeleton dots/lines instead of the full textured 3D MEF model. The user wanted the actual 3D character model (with textures) to be animated by the IFF bones - true skeletal skinning where the MEF mesh deforms in sync with the bone animation.
**Resolution:** (1) Auto-setup: the "Set Level..." handler in Settings > Level now automatically finds objects.qvm (in the level folder or sibling level folders), decompiles it to objects.qsc via `qvm decompile`, walks up the directory tree to find `common/ANIMS`, finds the `models/` folder (or unpacks the .res if needed), pre-extracts all IFF files to the cache, enables Animation mode, loads the animation set, and persists all paths to igi1conv.ini. The user sees a summary dialog listing all configured paths. (2) Skeletal skinning: added `RestVertex` storage to `ModelViewer` that preserves the MEF mesh's rest-pose vertices with bone indices and weights. When a MEF bone model (type1) is loaded via `loadMefRecursive`, each vertex's bone index is stored. When an IFF is loaded on top via `loadIff`, rest-pose bone world transforms are computed from the IFF skeleton. Each frame in `updateIffSkeleton`, the animated bone transforms are computed and the MEF mesh is deformed using rigid-transform skinning: `animatedPos = animBonePos + animBoneRot * (restVertPos - restBonePos)`. Normals are rotated the same way. UVs and submeshes (with textures) are preserved from the MEF load, so the textured character model is visible and animates in sync with the IFF. The old bone dots/lines view is kept as an optional overlay toggled with the 'B' key. When no MEF is loaded (just an IFF), the old skeleton-only view is shown as fallback.

## Bug ID: Animation-Skinning-Flying-Pieces
**Description:** The skeletal skinning in Animation mode produced flying/scattered mesh fragments instead of a coherent animated character. The root cause was that the skinning used the IFF skeleton's rest-pose bone positions as the reference, but the MEF parser bakes vertex positions using the MEF's OWN hardcoded bone hierarchy (from `hardcoded_bones.h`). These two bone hierarchies have different rest-pose translations, so the bone-local offset computed as `rv.pos - iffRestBonePos` was wrong, causing vertices to be displaced to incorrect positions.
**Resolution:** Fixed by storing the MEF's own bone world positions (computed via `ComputeBoneWorldPositions(geo.bones)`) when `loadMefRecursive` loads a bone model, and using THOSE as the rest-pose reference for skinning. The correct offset is now `rv.pos - mefBoneWorldPos[b] / IGI_SCALE`, which gives the true bone-local vertex position. The animated position is then `iffAnimBonePos + iffAnimBoneRot * offset`. Also moved `ComputeBoneWorldPositions` out of the anonymous namespace in `mef_native.cpp` and declared it in `mef_native.h` so the GUI can call it. Additionally: (a) reduced the Animation panel UI heights (buttons max 20-22px, labels max 16-18px, list max 60px, fonts 10-11px) so the toolbar is compact; (b) added a "Play Animation" right-click menu item on binary .MEF/.MEX files that auto-selects the model in Animation mode, loads it in the 3D viewer, and prompts the user to pick an animation from the list.

## Bug ID: Animation-Skinning-Root-Offset
**Description:** After fixing the flying-pieces bug, the animated character still looked distorted: body parts appeared to detach or the whole model sat in the wrong place. The root cause was that IFF animations place the skeleton root at `(0,0,0)` while the MEF mesh is authored with the root at `(0,0,3990.4)` game units (~97.4 viewer units). The skinning was applying IFF bone transforms in IFF space and then re-normalizing against the MEF rest bounding box, so the model was shifted away from its intended viewer position and rotations were effectively around the wrong origin.
**Resolution:** Switched the skinning to direct matrix-palette deformation (`QMatrix4x4::map(localOffset)` / `mapVector(normal)`) and added the MEF↔IFF root translation back after animation so the deformed mesh stays in the MEF viewer frame. At rest pose the result now exactly equals the original MEF mesh. Added a `showRestPose` toggle bound to the `P` key to sanity-check skinning, aligned the `B` bone overlay to the same root offset, and extended the bone-mapping debug log to compare all 33 MEF/IFF bones and warn if any differ by more than 0.5 units after accounting for the root offset.

## Bug ID: Bone-Overlay-DoubleNormalisation
**Description:** Pressing `B` in Animation mode showed all 33 bones collapsed into a single tiny dot instead of distributed across the body, and the bones never aligned with the 3D model. Root cause: `addBonesOverlay()` pre-normalised bone vertices with `(p.x - cx) / ext` to normalised space, but the normalisation loop in `updateIffSkeleton()` immediately re-normalised ALL vertices (including the just-added bone vertices) with `(v - modelCx) / modelScale`. This double normalisation squished the bones toward zero, collapsing them into a single point at the model centre.
**Resolution:** `addBonesOverlay()` now pushes bone vertices in raw MEF-space coordinates (no pre-normalisation). The existing normalisation loop in `updateIffSkeleton()` correctly converts both mesh and bone vertices together using the same `modelCx`/`modelScale`, so they stay perfectly aligned. Joint radius is now `0.025 * modelScale` so bones maintain a constant on-screen size regardless of the model's physical extent. The `B` key handler also calls `updateIffSkeleton()` so the overlay appears immediately.

## Bug ID: Hardcoded-Paths-Removed
**Description:** The codebase had several machine-specific hardcoded paths that would break on any install other than the original developer's: `D:/Software/IGI-Game` as the default folder for the file system model and the "Set Level" QFileDialog, `D:\Software\IGI-Game\COMMON\ANIMS` in the "Set ANIMS Source Folder" dialog title, and `D:/IGI1/missions/location0/level1/objects.qsc` / `C:/IGI1/...` hardcoded in the `RealLevel1ObjectsQsc` integration test.
**Resolution:** Replaced all hardcoded paths with portable, configurable fallbacks. The QFileSystemModel defaults to `QDir::homePath()` when no `LastFolder` is persisted. The "Set Level" and "Set ANIMS" dialogs default to `QDir::homePath()` (or the last-used directory). The `RealLevel1ObjectsQsc` test now resolves the corpus root via the `IGI_GAME_PATH` env var / `--game-path` flag through the existing `igi1conv_test::CorpusDir()` / `Corpus()` helpers and `GTEST_SKIP()`s cleanly when the corpus is absent. No machine-specific paths remain in the source tree.

## Bug ID: Audio-Mode-Mef-Misroute (1.9.6)
**Description:** Switching to the Audio tab while a `.mef` file was selected caused the GUI to run the IGI WAV converter on the model file, producing `wav convert failed: parse error: missing ILSF signature (not an IGI .wav?)`. Additionally, the audio file label showed the cached temp filename (e.g. `382ec3063589c2e3_19eef38ffe0.wav`) instead of the original source name.
**Resolution:** Added an extension guard in `audioLoadIgiWav()` and in the Audio mode `loadFile()` handler: only `.wav` files are decoded/loaded. Non-`.wav` selections now log a clear warning and show `(not audio)` in the file label instead of spawning a failing conversion. Added `audioSourceName` member to `MainWindow` and updated `mciOpen()` to display the original source filename while still playing the cached PCM `.wav` behind the scenes.

## Bug ID: Animation-Panel-Missing-Transport (1.9.6)
**Description:** The Animation panel only had a single "Play" button; there was no Pause/Resume, step-backward, or step-forward control in the panel itself.
**Resolution:** Added `Pause`/`Resume`, `Back`, and `Fwd` buttons to the Animation panel, wired to `modelViewer->iffTogglePlayPause()`, `iffStepBackward()`, and `iffStepForward()`. Button text and the IFF media-bar play button stay synchronised so pause/resume state is consistent across both toolbars.

## Bug ID: Texture-Default-Zoom (1.9.6)
**Description:** `.tex`/`.spr`/`.pic`/`.tga`/`.png` images opened in the image editor were displayed at 1:1 zoom by default, which often overflowed the window for high-resolution textures.
**Resolution:** Changed `ImageEditor::loadImage()` to compute an initial `zoomFactor` that fits the image inside the scroll area while never upscaling small images beyond 1:1. The existing `Fit` toolbar button still allows explicit refit after resize.

## Bug ID: Res-Pack-Renamed-Folder-Empty (1.9.6)
**Description:** Running `igi1conv res pack <dir> <out.res>` without an explicit `--prefix` produced an empty 20-byte `.res` file (only the ILFF header) when the input folder was renamed or when the folder name did not match the default empty prefix. `RES_Compile` resolves `LOCAL:prefix/file` relative to the QSC parent directory, so the prefix must equal the folder name on disk.
**Resolution:** In `cmd_res.cpp` `res pack`, default the `--prefix` to the input directory's filename plus `/` when the user does not supply one. This makes the CLI behaviour match the GUI's "Pack to Archive" context menu and ensures every file is found regardless of folder name.

## Bug ID: Tga-To-Spr-Fail-And-Label (1.9.6)
**Description:** Right-clicking a `.tga` and choosing "Convert to .spr" sometimes failed with `Could not load image` because `QImage` cannot load every TGA variant. The menu label also read "Convert to .spr" instead of the requested "Convert to SPR".
**Resolution:** Renamed the context-menu item to "Convert to SPR". Switched both "Convert to TEX" and "Convert to SPR" to use `loadImageSafe()`, which falls back to `stbi_load` for formats Qt cannot read, so TGA/PNG/BMP/JPEG conversions now work for all images the project supports.

## Bug ID: Open-File-Overwrites-Workspace (1.9.6)
**Description:** Using File > Open File changed the main file-tree root to the selected file's parent directory, overriding the user's chosen workspace folder.
**Resolution:** Removed the `fileModel->setRootPath()` and `treeView->setRootIndex()` calls from the "Open File" handler. Opening a file now loads it in the editor while leaving the main folder path unchanged. The separate "Open Folder..." action still changes the workspace root when explicitly requested.

## Bug ID: Video-Animation-Modes-Unified (1.9.6)
**Description:** `.iff` files opened in a dedicated "Video" mode while `.mef` files opened in "3D" mode, and right-click "Play Animation" only worked for `.mef`. The user wanted a single "Animation" mode that acts like a video player (play/pause/forward/backward/FPS/model selector), automatically handles both `.iff` (bones only) and `.mef` (model + textures), keeps the `B` bone toggle, and has a sorted mode list plus a working right-click "View As" menu.
**Resolution:** Replaced the mode list order with `Auto, Text, Hex, Image, Audio, Animation, 3D` and removed the old "Video" entry. `.iff` now auto-detects to Animation mode; `.mef` auto-detects to 3D mode but can be switched to Animation via right-click "Play Animation" or the mode combo. The Animation `loadFile()` handler loads the file in `ModelViewer` and shows both the IFF media bar (transport + clip selector) and the Animation panel (model/anim selector + FPS). The existing `B` key toggles the bone overlay for `.mef`; `.iff` continues to show bones by default. The right-click "View As" submenu now lists all seven modes and invokes `loadFile(path, mode)` for each.

## Bug ID: Mef-3D-Shows-Graph-Nodes (1.9.6)
**Description:** After a `graph*.dat` file had been opened, selecting a `.mef` (or `.iff`/`.obj`) in 3D/Animation mode initially displayed the model, but clicking or interacting with the 3D view suddenly switched it back to the graph node view. `currentGraph.valid` stayed true across model loads, so the graph mouse/keyboard handlers (especially node picking on `mousePressEvent`) regenerated graph geometry on top of the newly loaded model.
**Resolution:** `ModelViewer::loadModel()` now clears `currentGraph` and resets `selectedGraphNodeId` whenever a non-graph model is loaded, and notifies the graph toolbar callback with zero nodes/links so the previous graph state cannot interfere with MEF/IFF/OBJ interaction.

## Bug ID: Animation-Panel-Duplicate-Transport (1.9.6)
**Description:** The Animation panel gained Pause/Resume/Back/Fwd buttons, but the IFF media bar directly above it already provided the same transport controls, creating a duplicate GUI.
**Resolution:** Removed the redundant `Pause`, `Back`, and `Fwd` buttons from the Animation panel. Transport remains in the shared IFF media bar; the Animation panel keeps the `Play` button (for objects.qsc-driven loads), model/anim selectors, Loop checkbox, and FPS input.

## Bug ID: Image-Fit-Not-Matching-Fit-Button (1.9.6)
**Description:** The initial image zoom was capped at 1:1, so small images did not fill the viewport the same way as pressing the `Fit` toolbar button.
**Resolution:** Removed the 1.0 zoom cap in `ImageEditor::loadImage()` so the default zoom exactly matches the `Fit` button calculation (`min(scrollArea.width/img.width, scrollArea.height/img.height)`), including upscaling small images to fill the view.

## Bug ID: Multi-Select-Missing-Batch-Conversion (1.9.6)
**Description:** Selecting multiple files only offered Delete/Copy/Cut/Paste; there was no batch conversion option for any format.
**Resolution:** Added a "Batch Conversion" submenu to the multi-selection context menu. It detects the extensions of the selected files and offers per-format conversions: TEX/SPR/PIC → TGA/PNG/SPR, PNG/TGA/BMP/JPG → TEX/SPR, MEF/MEX → OBJ/Text, WAV → WAV, IFF/BFF → BEF. The user picks one output folder and the GUI runs `igi1conv` for each matching file, then reports success/failure counts.

## Bug ID: Iff-Play-Animation-Loads-Wrong-File (1.9.6)
**Description:** Right-clicking an `.iff` and choosing "Play Animation" sometimes displayed a textured MEF model instead of the IFF's bone-only skeleton, and could play the wrong animation (the one from a previously selected `.mef`). The cause was that `playAnimationForFile()` changed the mode combo without updating `currentFile` or blocking its signal, so the combo's `currentIndexChanged` handler reloaded the stale `currentFile`.
**Resolution:** `playAnimationForFile()` now sets `currentFile` and `currentExt` to the right-clicked path before switching to Animation mode, and blocks signals on `viewModeCombo` during the index change so no accidental reload of the previous file occurs. For `.iff` it loads only the IFF skeleton (bones) via `modelViewer->loadModel()`.

## Bug ID: Graph-Toolbar-Visible-In-Non-Graph-Modes (1.9.6)
**Description:** The graph toolbar (Node+, Node-, Reset, Nodes/Links checkboxes, Total Nodes/Links labels) was showing on top of the 3D view in Animation mode and other non-graph modes, cluttering the UI with irrelevant controls.
**Resolution:** The `onGraphLoaded` callback now only shows `graphToolbar` when `nodeCount > 0`. When `loadModel()` clears the graph state and calls `onGraphLoaded(0, 0)`, the toolbar is hidden instead of shown.

## Bug ID: Mef-Play-Animation-Empty-Panel (1.9.6)
**Description:** Right-clicking a `.mef` and choosing "Play Animation" showed an empty animation panel if `objects.qsc` was not loaded, leaving the user unable to pick an animation. The IFF media bar was also shown unnecessarily for `.mef` files.
**Resolution:** `playAnimationForFile()` now auto-loads the animation set via `loadAnimationSetFromQsc()` if it's empty when a `.mef` is selected. The status label shows "Set objects.qsc in Settings > Animation" if the set remains empty after loading. The IFF media bar is now only shown for `.iff` files, not `.mef`, reducing UI clutter.

## Bug ID: Mef-VCoord-Explosion-Collision-Geometry (1.9.7)
**Description:** Type 3 lightmap (collision) models exported synthetic UVs (`x*0.1, z*0.1`) that wrapped outside `[-1,2]` in OBJ output. The raw values passed through `std::fmod` without wrapping into `[0,1)`, causing UV coordinates to explode to large negative/positive values in the exported OBJ.

**Resolution:** Added `std::fmod` wrapping in `ParseCollisionGeometry()` (`mef_native.cpp`) to wrap the synthetic UVs into `[0,1)`. The UV range for Type 3 models in the corpus is now `0..0.9999` instead of `-11..1`.

## Bug ID: Apply-Animation-On-Model-Mef-Not-Found (1.9.7)
**Description:** The `applyIffOnModel()` function used a hardcoded path formula (`searchDir + "/" + chosen + ext`) to locate the selected MEF file, which failed when the model lived in a subdirectory of `globalModelsDir`.

**Resolution:** The function now builds a `QMap<QString, QString>` during directory scan that maps each model ID (`000_00_0`) to its full discovered path, and uses the map for lookup. Models in subdirectories are now found correctly.

## Bug ID: Animation-Transport-Buttons-Missing (1.9.7)
**Description:** After `playAnimationForFile()` loaded a `.mef` and started animation, the IFF media bar (play/pause/step-back/step-forward/scrubber/clip selector) was not visible, so there was no way to control playback.

**Resolution:** Added `iffMediaBar->show()` in `onAnimationPlayClicked()` after `modelViewer->playClip()` and after the `OnNotFoundAnimation` fallback, so transport controls appear whenever animation clips are actually playing.

## Bug ID: Test-Corpus-Text-IFF-Skip (1.9.7)
**Description:** `FindCorpusFileByRegex()` picked up decompiled text `.IFF` files (starting with `\r\n`) in the game corpus's `Decompiled` directory, causing 6 IFF tests (`IffConvert`, `IffRebuild`, `IffCreateFromBefs`, `IffExportGif`, `IffRoundTripSizeMatches`, `IffDecompileCreateRoundTrip`) to fail when trying to parse them as binary IFFs.

**Resolution:** Added a first-4-bytes check in `FindCorpusFileByRegex()` that skips files whose first bytes are `\r` or `\n` (text IFF signatures), ensuring only binary IFF files are returned.

## Bug ID: UV-Tests-Picking-Type3-Lightmap-Models (1.9.7)
**Description:** `MefExportObjHasRealUvs` and `MefExportVFlipMatchesModelType` used `IGI1CONV_NEED(f, "\\.mef$")` which could pick Type 3 lightmap models. Type 3 models have synthetic UVs spanning `-11..1` (before fix) and no real texture coordinates, causing the tests' expected UV range assertions to fail.

**Resolution:** Both tests now use `FindCorpusMefOfModelType(1)` (for Type 1 skinned) and `FindCorpusMefOfModelType(0)` (for Type 0 rigid) to guarantee they test only models with real texture coordinates. Type 3 models are excluded entirely from these assertions.

## Bug ID: MCP-QSC-Escaped-Closing-Quote (PR #19)
**Description:** The game-facing MCP QSC editor accepted a quoted literal whose final quote was escaped, allowing an edit request to persist malformed QSC source while reporting success.

**Resolution:** QSC literal validation now scans escaped characters and requires an unescaped closing quote at the end of the token. Added a regression test covering the malformed literal and preserving the original output on rejection.

## Bug ID: MCP-Stdio-Unbounded-Frame (PR #19)
**Description:** MCP stdio framing used `std::getline`, which could allocate an unbounded buffer before the configured 8 MiB message limit was checked.
**Resolution:** Added bounded byte-by-byte framing that caps input at 8 MiB, discards only the rest of the oversized frame, returns a JSON-RPC parse error, and continues with the next request. Added a regression test proving the next frame is still handled.

## Bug ID: MCP-Lightmap-Partial-Rewrite (PR #19)
**Description:** Lightmap recalculation could write earlier `.olm` files before a later parse or write failure, leaving a partially updated game asset set.
**Resolution:** Recalculation now parses and transforms every block first, writes temporary siblings, then commits through same-directory rename with backups and rollback. Rollback now verifies each restore, includes the currently staged file, and reports failure instead of claiming the original set was restored when a filesystem operation failed.

## Bug ID: MCP-WorkingDirectory-Leak (PR #19)
**Description:** A failed or exceptional game-facing MCP command that changed the process cwd could leave later commands executing from an unintended directory.
**Resolution:** Added a scoped current-directory guard that validates the target, restores the original cwd on every exit path, explicitly restores after failed changes, and reports restoration failure as a command failure.

## Bug ID: MCP-QSC-Block-Comment-Parsing (PR #19)
**Description:** QSC object and lightmap scanners did not consistently treat block comments as trivia, so commented `Task_New` text or comment punctuation could create false objects, break call nesting, or corrupt extracted positions and names.
**Resolution:** Centralized comment/string-aware scanning across tokenization, call-span detection, object discovery, lightmap binding, direct-argument extraction, and MCP QSC editor call discovery. Added regressions for fake commented calls, inline comments, comment-bearing literals, and comments between `Task_New` and its opening parenthesis.

## Bug ID: MCP-QSC-TaskNew-Trivia-Gap (PR #19)
**Description:** The HumanSoldier parser skipped only whitespace after `Task_New`, so a valid call written as `Task_New /* annotation */ (...)` was silently omitted even though the shared trivia scanner already understood block comments.
**Resolution:** Reused `skipTrivia()` before the opening-parenthesis check and added a regression test asserting that the commented call is parsed with its object name and model ID intact.

## Bug ID: MCP-Test-Live-Cwd (2026-08-25)
**Description:** Two structural MEF/GUI contract tests assumed the test process current directory was the repository root. Deploying the MCP test executable under `D:\IGI1\mcp-tests` therefore reported missing `mef_exporter.cpp` and `gui_main.cpp` even though the source contract was valid.

**Resolution:** Recorded the configured checkout root as `IGI1CONV_SOURCE_DIR`, added a shared `SourceTreeFile()` resolver, and changed both structural tests to use it. The deployed suite now runs from `D:\IGI1` with 156 passed, 2 expected skips, and 0 failures.

## Bug ID: MCP-Review-Coverage-Gates (2026-08-25)
**Description:** The MCP hardening review found that standalone Qt deployment was only warned about, the live matrix was not registered with CTest, the plan still described authenticated remote HTTP while the implementation intentionally rejects plaintext remote binds, and the smoke harness did not verify independent or unknown HTTP sessions.

**Resolution:** Windows configuration now requires `windeployqt` by default (with an explicit opt-out for PATH-managed Qt), CTest registers the live matrix when `IGI1CONV_GAME_PATH` is configured, the plan documents loopback-only plain HTTP plus HTTPS termination for remote access, and the smoke harness verifies distinct session IDs and rejects unknown sessions. The deployed matrix remains 100% with input integrity true.
