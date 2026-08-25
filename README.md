# igi1conv — Project IGI 1 Game Converter
![igi1conv Logo](assets/igi1conv_logo.jpg)

This is a standalone command-line converter and inspector for *Project IGI 1* game files. Inspired by the original IGI1Conv shipped with IGI 2, it allows you to seamlessly read, convert, and inspect the engine's proprietary formats—textures, meshes, scripts, archives, terrain, fonts, and AI graphs—with **no OpenGL or game-editor dependency**.

It's the ultimate tool for modders and researchers looking to extract game assets, modify them using modern tools, and inject them back into the game.

## 🖥️ GUI Version (Graphical User Interface)

An interactive workspace designed for visual inspection, navigation, and quick asset modifications:
*   **File Tree Navigator**: Browse game folders with real-time directory searching.
*   **Interactive 3D Viewer**: Render native `.mef` and `.obj` models with rotation, zoom, and wireframe views.
*   **2D Image Viewer**: Instantly preview proprietary `.tex`, `.spr`, and `.pic` texture assets.
*   **Texture Paint & Editor**: Edit Game assets directly by drawing, painting, and modifying textures using pencil, eraser, and color pickers, and save modifications back to the native game forma[...]
*   **Script & Hex Inspectors**: Read decompiled `.qsc` script files and inspect raw binary data.
*   **Right-Click Context Menu**: Trigger conversion tasks directly from the graphical user interface.

> [!IMPORTANT]
> To use **Apply Textures** on 3D models in the GUI, you must first select the active level folder from the **Settings** menu to resolve the correct texture mappings.

> [!NOTE]
> **Latest: v1.11.0-rc.3 (August 2026) — Game-facing MCP release candidate**:
> - **Follow-up hardening**: every MCP output-producing operation rejects an input/output collision, including an input file contained by an output directory; HTTP sessions use bounded retention with expiry, and oversized `Content-Length` values are classified as `413 Payload Too Large` even when they exceed 32-bit integer range.
> - **Windows release bundle**: the downloadable x64 ZIP includes the Qt runtime and `vc_redist.x64.exe`; install the redistributable when the host does not already provide the MSVC runtime.
> - **Validation scope**: the deployed live matrix covers all 58 operations registered by `tools/list` (58/58, 100% operation coverage), plus four CLI-only cases; this is operation coverage, not source-code line coverage.
> - **Animation mode (Mode 6)**: New GUI mode that plays IFF bone animations on textured 3D MEF models. Toggle via **Settings > Animation**. Includes a Model dropdown, Animations listbox, Play button, Loop checkbox, and a configurable **FPS input textbox (1–120)**.
> - **Skeletal skinning**: The textured 3D MEF mesh is deformed each frame using the IFF bone transforms — you see the actual animated character, not skeleton dots. Press `P` to toggle rest-pose skinning for debugging; press `B` to toggle the bone skeleton overlay (now depth-test disabled so it renders on top of the model at the correct scale).
> - **Auto-setup**: Selecting a level from **Settings > Level** auto-detects `objects.qsc`, the `common/ANIMS` folder, and the level `models/` folder, so Animation mode is one click away.
> - **Root-offset fix**: The MEF↔IFF coordinate mismatch (root at `(0,0,3990.4)` vs `(0,0,0)`) is handled, so body parts no longer detach during animation.
> - **All hardcoded paths removed** from the source tree — GUI falls back to the user's home directory, and the corpus is resolved via the `IGI_GAME_PATH` env var or `--game-path` flag.

For the full version history see [**CHANGELOG.md**](./CHANGELOG.md).


### GUI Screenshots

**_1. Main View Interface_**  
![Main View](assets/01_main_view.png)  
*This screenshot showcases the primary **IGI Game Converter** graphical interface. It actively features the robust directory tree on the left panel, allowing seamless navigation and real-time visu[...]
**_2. Intelligent Model Search_**  
![Model Search](assets/02_model_search.png)  
*Demonstrates the powerful global model search functionality. By seamlessly filtering the massive directory tree, users can instantly locate specific internal game assets, drastically accelerating[...]

**_3. Texture Renderer (TEX)_**  
![Texture View 01](assets/03a_tex_000_01.png)  
*Presents the native 2D image renderer displaying standard proprietary `.tex` files. This highly optimized viewer decodes internal texture chunks flawlessly, offering direct visual inspection of w[...]

**_4. High-Res Texture Inspection_**  
![Texture View 04](assets/03b_tex_000_04.png)  
*Highlights the image viewer processing larger texture assets. It efficiently unpacks complex multi-layered game textures on-the-fly, providing critical diagnostic insights and verifying accurate [...]

**_5. Texture Context Menu_**  
![Texture Context Menu](assets/03c_tex_context_menu.png)  
*Reveals the interactive context menu available on `.tex` files. This vital popup menu grants immediate access to essential conversion commands, dynamically bridging the GUI with the underlying CL[...]

**_6. Texture Paint & Drawing Editor (Edit Game Assets)_**  
<p align="center">
  <img src="assets/05a_tex_paint.png" width="32%" alt="Texture Paint Tools" />
  <img src="assets/05aa_tex_paint.png" width="32%" alt="Texture Paint Canvas" />
  <img src="assets/05b_tex_paint.png" width="32%" alt="Texture Paint Brush" />
</p>
<p align="center">
  <img src="assets/05c_tex_paint.png" width="49%" alt="Editing Texture Asset" />
  <img src="assets/05d_tex_paint.png" width="49%" alt="Save Modified Asset" />
</p>
*Showcases the integrated advanced Image Editor. With this tool, we can edit Game assets directly; users can draw, paint, erase, change pen size/color, and save modified texture assets directly ba[...]

**_7. Raw 3D Mesh Viewer (MEF)_**  
![3D Raw Viewer](assets/04a_mef_3d_raw.png)  
*Shows the built-in interactive 3D renderer displaying a `.mef` model without applied textures. It provides smooth camera navigation, allowing modders to closely examine the structural geometry an[...]

**_8. 3D Model Zoom Inspection_**  
![3D Zoomed View](assets/04_mef_003_zoomed.png)  
*Displays the 3D viewer dynamically zoomed in on a specific geometric mesh component. This fine-tuned hardware-accelerated OpenGL view is essential for inspecting intricate polygonal details and d[...]

**_9. 3D Wireframe Rendering_**  
![3D Wireframe](assets/04c_3d_wireframe.png)  
*Features the 3D viewer explicitly toggled into wireframe rendering mode. This highly technical diagnostic view uncovers the underlying polygonal topography, exposing hidden mesh complexities crit[...]

**_10. Mesh Context Menu_**  
![MEF Context Menu](assets/04c_mef_context_menu.png)  
*Illustrates the interactive right-click menu tailored specifically for `.mef` 3D model files. It exposes critical workflow actions like automated texture application, batch bundle processing, and[...]

**_11. Integrated Text Editor_**  
![Text View](assets/07a_qvm_text_view.png)  
*Captures the built-in text editor actively displaying decompiled `.qsc` script files. It features an integrated file-searching utility and provides a lightweight, seamless environment for analyzi[...]

**_12. Hexadecimal Inspector_**  
![Hex View](assets/07a_qvm_hex_view.png)  
*Showcases the integrated Hex View inspector toggled for raw binary analysis. This specialized mode empowers developers to meticulously investigate unrecognized proprietary file structures byte-by[...]

**_13. UI Themes_**  
<p align="center">
  <img src="assets/08c_theme_dark.png" width="32%" alt="Dark Theme" />
  <img src="assets/08c_theme_military.png" width="32%" alt="Military Theme" />
  <img src="assets/08c_theme_solarized.png" width="32%" alt="Solarized Theme" />
</p>
*Customizable visual interfaces including Dark, Military, and Solarized styles.*

**_14. About & Documentation_**  
![About Dialog](assets/09b_about_dialog.png)  
*Features the comprehensive informational dialog box. It elegantly provides crucial versioning details, integrated clickable GitHub repository documentation links, and essential architectural over[...]

---

## 🐚 CLI Version (Command-Line Interface)

A lightweight, high-performance command-line utility optimized for scripting, automated builds, and batch tasks:

*   **Asset Extraction**: Unpack and pack `.res` archives.
*   **Mesh Conversion**: Export proprietary `.mef` models directly to standard `.obj` files.
*   **Texture Conversion**: Convert images to and from `.tex`, `.spr`, and `.pic` formats (with resizing).
*   **Script Decompilation**: Decompile `.qvm` bytecode to `.qsc` source, and compile `.qsc` back to `.qvm`.
*   **Metadata Editing**: Export `.dat` mappings to JSON and compile them back into binary `.dat`/`.mtp` packages.
*   **Batch Operations**: Recursively process entire folders of assets in one command.

### CLI Usage & Commands

Below are practical, real-world examples showing how to leverage the `igi1conv` CLI to mod Project IGI 1 assets:

#### 1. Working with Textures (`.tex`, `.spr`, `.pic`)
The game stores textures in a proprietary format. You can export them to `.png` to edit them, and then repack them.
```bash
# Get information about a texture (dimensions, mipmaps, mode)
igi1conv tex info textures/FLARE00.TEX

# Export a texture to PNG for editing
igi1conv tex to-png textures/FLARE00.TEX -o my_edits/FLARE00.png

# Resize and export simultaneously
igi1conv tex to-png textures/arrow1_1.spr -o out/arrow1_1.png --resize 32 32

# Convert your edited PNG back to the game's TEX format
igi1conv tex to-tga my_edits/FLARE00.png -o textures/FLARE00.TEX
```

## MCP server — game effects only

`igi1conv` includes a Model Context Protocol server for automation against the
same supported game-data operations as the CLI. It exposes conversions,
inspection, validation, packing, lightmap work, and QSC task edits that affect
files consumed by Project IGI. It does not expose GUI preferences, themes,
cache paths, viewer/camera state, animation playback controls, layout, or any
other editor-only state.

Start the default newline-delimited stdio transport:

```bash
igi1conv mcp --transport stdio
```

Or start Streamable HTTP on localhost:

```bash
igi1conv mcp --transport http --host 127.0.0.1 --port 8765
```

HTTP validates `Origin` and serves plain HTTP on loopback only. Non-loopback
binds are refused; use an HTTPS reverse proxy in front of a loopback listener
for remote access. Use `--origin` to provide an explicit Origin allowlist.
Browser clients can send an unauthenticated `OPTIONS` preflight; allowed
Origins receive only the documented POST/OPTIONS and MCP/auth/session header
permissions. The MCP endpoint is `/mcp` by default.

The server advertises two game-facing tools: `igi_game_command` (all entries
discovered from `tools/list`, such as `tex.info`, `mef.compile`, `res.repack`,
and `qsc.compile`) and `igi_game_object_edit`. The latter
can select a QSC `Task_New` by task id, class, or object name and update game
position, rotation/gamma, model id, team, bone hierarchy, stand animation, or
an arbitrary validated direct argument for task-specific enemy, weapon, AI,
and trigger data. Named placement fields are restricted to the known
`HumanSoldier` layout; other task classes use explicit indexed updates. MCP
file writes require distinct input/output paths. Stdio frames, HTTP request
deadlines, and command output capture are bounded; the MCP lifecycle is
session-scoped; and inherently in-place CLI operations are not exposed through
MCP. OLM creation rejects PNG dimensions that cannot be represented by the
game format, and `res repack` refuses unmatched directory files before writing
an archive. HTTP sessions expire and are retained in a bounded cache. Every
MCP output-producing path, including implicit/default exporter outputs, rejects
an output path that resolves to an input path.

### MCP examples

For stdio, send one JSON-RPC object per line. Initialize first, then discover
the tools and call a registered operation. The server prints only JSON-RPC
responses on stdout, so it can be connected directly to an MCP client:

```text
{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"igi-example","version":"1.0"}}}
{"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}}
{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"igi_game_command","arguments":{"command":"qsc.list-objects","args":["missions/location0/level1/objects.qsc","--json"],"working_directory":"D:/IGI1"}}}
```

The generic command tool uses the same operation names returned by
`tools/list`. For example, `tex.info` reads a texture without changing it:

```json
{
  "name": "igi_game_command",
  "arguments": {
    "command": "tex.info",
    "args": ["missions/location0/level1/textures/FLARE00.TEX"],
    "working_directory": "D:/IGI1"
  }
}
```

To edit a QSC placement, always use a different output path. Selectors can be
combined, and named placement fields use the known `HumanSoldier` layout:

```json
{
  "name": "igi_game_object_edit",
  "arguments": {
    "input_file": "missions/location0/level1/objects.qsc",
    "output_file": "D:/IGI1/tests_temp/objects-edited.qsc",
    "selector": {"class_name": "HumanSoldier", "object_name": "Guard01"},
    "position": [125.0, 42.0, 900.0],
    "rotation": 1.57,
    "model_id": "013_01_1",
    "team": 2
  }
}
```

For Streamable HTTP, retain the `Mcp-Session-Id` returned by `initialize` and
send it on every subsequent request. A minimal PowerShell session looks like:

```powershell
$body = '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"igi-http-example","version":"1.0"}}}'
$init = Invoke-WebRequest http://127.0.0.1:8765/mcp -Method Post `
  -Headers @{ Accept = 'application/json, text/event-stream' } `
  -ContentType 'application/json' -Body $body
$session = $init.Headers['Mcp-Session-Id']
$call = '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"igi_game_command","arguments":{"command":"tex.info","args":["missions/location0/level1/textures/FLARE00.TEX"]}}}'
Invoke-RestMethod http://127.0.0.1:8765/mcp -Method Post `
  -Headers @{ Accept = 'application/json, text/event-stream'; 'Mcp-Session-Id' = $session } `
  -ContentType 'application/json' -Body $call
```

For a protected loopback listener, set the token in the environment rather
than placing it in a shared command line or source file:

```powershell
$env:IGI1CONV_MCP_TOKEN = 'use-a-local-secret'
igi1conv mcp --transport http --host 127.0.0.1 --port 8765
```

Send `Authorization: Bearer use-a-local-secret` with the HTTP requests. HTTP
request bodies are bounded; an oversized body is rejected with `413` before
dispatch. Successful tool results preserve `exit_code`, `stdout`, `stderr`,
and `output_paths`; a nonzero converter exit code sets `isError`.

For the complete MCP contract, operation registry, JSON examples, result
format, and excluded editor-only surfaces, see [docs/mcp.md](docs/mcp.md).

To assemble the Windows prerelease bundle from a deployed Release directory,
run the tracked packaging script. It creates the binary-only ZIP, versioned
standalone executables, and a checksum manifest after the final archive is
written:

```powershell
pwsh -File ./tests/package_mcp_release.ps1 `
  -SourceRoot D:\IGI1\mcp-tests `
  -OutputRoot D:\IGI1\tests_temp\release-rc2 `
  -Version 1.11.0-rc.3
```

#### 2. Exporting 3D Meshes (`.mef`)
Extract weapons, characters, or level geometry into `.obj` format.
```bash
# Dump the structural data of a mesh to a text file for inspection
igi1conv mef dump models/model1.mef -o model1_struct.txt

# Export a single model to OBJ
igi1conv mef export models/model1.mef -o model1.obj

# Export all models in a folder iteratively (Batch Mode)
igi1conv mef export models/weapons/ -o output_objs/ --batch

# Bundle a MEF with its actual textures (requires the map's .dat file
