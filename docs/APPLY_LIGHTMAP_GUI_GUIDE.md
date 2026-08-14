# Apply Lightmap in GUI Editor — Complete User Guide

**Version**: 1.11.0  
**Last Updated**: 2026-06-25

This guide walks you through applying baked static lightmaps to 3D model objects in the IGI Game Converter GUI. By the end, you'll see your models with photorealistic pre-baked lighting blended onto the diffuse textures in real time.

---

## Quick Start (TL;DR)

1. **Prepare**: Have a level directory with `objects.qvm` (or decompiled `objects.qsc`) and `lightmaps/lightmaps.res` or `lightmaps_unpacked/`.
2. **Launch**: Run `igi1conv` (GUI) or `igi1conv --gui`.
3. **Configure**: **Settings → Animation → Set Objects.qsc...** → point to the level's `objects.qsc`.
4. **Apply**: Right-click a `.mef` model → **Textures → Apply Lightmap** → pick placement if model is reused.
5. **View**: Model renders with lightmap blended in the 3D viewer.

---

## Table of Contents

1. [Understanding Lightmaps](#1-understanding-lightmaps)
2. [Before You Start](#2-before-you-start)
3. [Step 1 — Configure Objects.qsc in Settings](#3-step-1--configure-objectsqsc-in-settings)
4. [Step 2 — Navigate to Your Model](#4-step-2--navigate-to-your-model)
5. [Step 3 — Apply Lightmap](#5-step-3--apply-lightmap)
6. [Step 4 — Handle Placement Ambiguity](#6-step-4--handle-placement-ambiguity)
7. [Step 5 — View the Result](#7-step-5--view-the-result)
8. [Full Worked Example](#8-full-worked-example)
9. [Troubleshooting](#9-troubleshooting)
10. [How We Solved This (Technical Background)](#10-how-we-solved-this-technical-background)

---

## 1. Understanding Lightmaps

### What is a Lightmap?

A **lightmap** is a pre-baked texture containing static lighting information for a 3D model. Instead of calculating light in real time (expensive), the original level editor baked the lighting once and saved it as a `.olm` (Object Lightmap) file. This lightmap is then blended with the model's diffuse color texture during rendering to produce photorealistic static lighting.

### IGI Game Lightmap System

In Project I.G.I. (IGI1) and IGI2:
- **Baked once** during level authoring (at level editor time).
- **Stored per-object instance**, not per-model file — the same `.mef` model placed at two different world positions can have two different lightmaps.
- **Bound via scripts** in `objects.qvm` (compiled) or `objects.qsc` (decompiled).
- **Stored as `.olm` files** in the level's `lightmaps/lightmaps_unpacked/` folder (or packed in `lightmaps.res`).
- **Models tagged with type 3** (`MODEL_LIGHTMAP` in the MEF header) support two UV coordinate layers: diffuse UVs and a separate lightmap atlas UV.

### Why This Matters

If you're:
- **Modifying existing models**: You want to see how the original baked lighting looks.
- **Creating new models**: You might want to test replacement geometry with the existing lightmaps.
- **Inspecting level lighting**: You need to see what the engine saw when rendering the level.

---

## 2. Before You Start

### Prerequisites

You need:

1. **A level directory** containing:
   - `objects.qvm` (compiled script) — **or**
   - `objects.qsc` (already decompiled script)
2. **Lightmap data** at one of:
   - `lightmaps/lightmaps.res` (packed), **or**
   - `lightmaps/lightmaps_unpacked/` (unpacked folder with individual `.olm` files)
3. **The model ID** you want to apply a lightmap to.
   - This is the `.mef` filename stem, e.g. `435_01_1` from `435_01_1.mef`.

### Finding a Model ID

Open the level directory in your file browser:

```
D:\IGI1\missions\location0\level1\
├── objects.qvm (or objects.qsc)
├── level.mef
├── 400_01_0.mef       ← This is a model. ID: 400_01_0
├── 435_01_1.mef       ← This is a model. ID: 435_01_1
├── 504_00_0.mef
└── lightmaps/
    └── lightmaps.res (or lightmaps_unpacked/ folder)
```

### Do I Need to Unpack `lightmaps.res` Manually?

**No.** The first time you apply a lightmap, the converter automatically unpacks `lightmaps.res` into `lightmaps_unpacked/` if it doesn't already exist. You don't need to do anything.

---

## 3. Step 1 — Configure Objects.qsc in Settings

The GUI needs to know which level's `objects.qsc` to read. This is how it knows which lightmap bindings apply to the model you're viewing.

### Steps

1. Launch the GUI: Run `igi1conv` (no arguments) or `igi1conv --gui`.
2. Click **Settings** (top menu bar).
3. Click **Animation**.
4. Click **Set Objects.qsc...** (or locate the button labeled "Set Objects.qsc").
5. A file picker opens. Navigate to your level directory and select `objects.qsc`.
   - If you only have `objects.qvm`, you must first decompile it:
     ```powershell
     igi1conv qvm decompile "D:\IGI1\missions\location0\level1\objects.qvm" -o "D:\IGI1\missions\location0\level1\objects.qsc"
     ```
   - Then re-open the Settings dialog and pick the newly created `objects.qsc`.

6. Click **Open** (or **OK**). The path is now saved.

### Verification

After setting the path, the title bar (or a status area in Settings) will show the path you selected. If you see nothing, the file may not have been found — double-check the path and that `objects.qsc` exists.

### ⚠️ Common Mistake

**Setting the wrong level's `objects.qsc`** is the most common cause of "no lightmap binding found" errors. Make sure the `objects.qsc` is from the **same level** as the `.mef` model you're viewing. If you switch to a different level, come back to Settings and update the path.

---

## 4. Step 2 — Navigate to Your Model

In the GUI, you'll see a file browser tree on the left side. This shows the current folder and all `.mef` files.

### Steps

1. In the GUI file tree (left panel), locate your `.mef` model file by name.
   - Example: Look for `435_01_1.mef` if the model ID is `435_01_1`.
2. **Single-click** the model to select it. The 3D viewer (center) loads the model without lightmap.
3. The model's diffuse (normal color) texture should now be visible in the viewer.

### What You Should See

- A 3D model in the center viewport.
- The model is lit with a default white ambient light (no per-object shading yet).
- The model's original diffuse texture colors are visible.

---

## 5. Step 3 — Apply Lightmap

Now that the model is selected and the `objects.qsc` is configured, you can apply the lightmap.

### Steps

1. **Right-click** the selected `.mef` model in the file tree.
2. A context menu appears. Hover over or click **Textures**.
3. A submenu opens. Click **Apply Lightmap**.

### What Happens Next

- The converter checks the configured `objects.qsc` for lightmap bindings for this model.
- If found, it resolves the `.olm` lightmap files from `lightmaps/lightmaps_unpacked/`.
- If the model is placed **only once** in the level, the lightmap is applied immediately (go to [Step 5](#7-step-5--view-the-result)).
- If the model is placed **more than once**, a **placement picker dialog** appears (go to [Step 4](#6-step-4--handle-placement-ambiguity)).

---

## 6. Step 4 — Handle Placement Ambiguity

If your model appears at multiple locations in the level, a dialog pops up asking you to pick which placement's lightmap you want to see.

### The Picker Dialog

Example dialog title: **"Select Placement for 435_01_1"**

Contents:
```
WaterTower (task 1104) → obj00000
Building (task 1106) → obj00002
Building (task 1107) → obj00003
```

Each line shows:
- **Name** — the in-game building/object name from the level script.
- **Task ID** — the internal script task number.
- **Logical lightmap ID** — the folder prefix for the `.olm` files (e.g., `obj00000`).

### Choosing a Placement

1. **Read the names** to identify which placement you want (e.g., "the water tower" vs. "the left building").
2. **Click one** to select it.
3. **Click OK** (or **Apply**, depending on the dialog UI).

### How to Know Which One You Want?

- **Inspect the 3D viewer**: Before clicking "Apply Lightmap," the model is shown in the default view. Try to remember its position relative to other objects in the level.
- **Use the CLI first** (optional): Run `igi1conv lightmap list --model <id> --qsc <objects.qsc>` to see a text list of all placements and their world positions. You can then pick by matching the coordinates.

---

## 7. Step 5 — View the Result

Once a lightmap is applied, the 3D viewer updates in real time.

### What You Should See

1. **The model's diffuse texture** is still visible.
2. **Additional shading** from the baked lightmap blends with the diffuse color.
   - Bright areas remain bright.
   - Shadowed areas appear darker (more realistic).
   - Color tone may shift slightly based on the baked lighting colors.

### The Lighting Formula

Internally, the GPU blends the lightmap like this:
```
final_pixel_color = diffuse_color * lightmap_color
```

This is called **multiply blending** — it preserves the diffuse details while applying the pre-baked static lighting.

### Interactive Inspection

- **Rotate the model**: Click and drag in the 3D viewer to rotate and inspect the lighting from different angles.
- **Zoom**: Scroll the mouse wheel to zoom in/out.
- **The lighting is baked**: The shading doesn't change as you rotate — it's pre-computed, which is why it's so fast.

---

## 8. Full Worked Example

### Scenario

You're modifying models in **Level 1** of **Location 0** (a common IGI1 scenario). You want to apply the lightmap to the **water tower model** (`435_01_1`) to see how it looked in the original game.

### Files You Have

```
D:\IGI1\missions\location0\level1\
├── objects.qvm
├── 435_01_1.mef (the water tower model)
└── lightmaps\
    └── lightmaps.res (or lightmaps_unpacked\)
```

### Step-by-Step Walkthrough

#### Step 1: Decompile the Script (if needed)

If you don't have `objects.qsc` yet:

```powershell
igi1conv qvm decompile "D:\IGI1\missions\location0\level1\objects.qvm" -o "D:\IGI1\missions\location0\level1\objects.qsc"
```

Output:
```
qvm: decompiled "D:\IGI1\missions\location0\level1\objects.qvm" → "D:\IGI1\missions\location0\level1\objects.qsc" (2341 bytes)
```

#### Step 2: Launch the GUI

```powershell
igi1conv
```

or

```powershell
igi1conv --gui
```

The GUI window opens. You see a file browser on the left showing `.mef` files.

#### Step 3: Configure Settings

1. Click **Settings** → **Animation**.
2. Click **Set Objects.qsc...**.
3. Navigate to and select `D:\IGI1\missions\location0\level1\objects.qsc`.
4. Click **Open**. The path is saved.

#### Step 4: Select the Model

1. In the file tree, find and click `435_01_1.mef`.
2. The 3D viewer loads the water tower model with its default diffuse texture.

#### Step 5: Apply Lightmap

1. Right-click `435_01_1.mef` in the file tree.
2. Hover over **Textures** → click **Apply Lightmap**.
3. The system checks the `objects.qsc`:
   - "Water tower is placed 1 time in this level."
   - The system automatically picks it — no dialog.
   - The `.olm` files are resolved from `lightmaps_unpacked/obj00000_*.olm`.
4. The 3D viewer updates. The water tower now shows realistic baked lighting blended with the diffuse texture.

#### Step 6: Inspect the Result

Rotate, zoom, and inspect the model. Notice:
- The baked shadows are frozen (they don't change as you rotate).
- The lighting is photorealistic because it was baked by a professional renderer, not the game engine.
- If the model has subsurface areas (like window glass), they may be darker or lighter based on interior lighting in the original level.

#### Optional: Export the Lightmap to PNG

If you want to save the lightmap texture to a file for editing:

```powershell
igi1conv olm to-png "D:\IGI1\missions\location0\level1\lightmaps\lightmaps_unpacked\obj00000_00000.olm" -o "D:\Temp\obj00000_00000.png"
```

You can then edit the PNG in Photoshop or any image editor and re-import it using the converter's OLM packing tools (see [Advanced: Editing Lightmaps](#advanced-editing-lightmaps) section, if provided).

---

## 9. Troubleshooting

### Issue: "No lightmap binding found"

**Symptom**: You right-click → Apply Lightmap, but nothing happens, or you see an error message.

**Causes**:
1. **Settings → Objects.qsc** is set to the **wrong level's** `objects.qsc`.
2. **Objects.qsc doesn't exist** — you're pointing at `objects.qvm` instead of the decompiled text file.
3. **The model isn't placed in this level** — model ID might be typed wrong, or the model doesn't appear in this level at all.
4. **No lightmaps in this level** — `lightmaps/lightmaps.res` is missing or the level was never baked.

**Fixes**:
- Double-check **Settings → Animation → Objects.qsc** path — make sure it's the correct level.
- Verify the file is actually `objects.qsc` (text), not `objects.qvm` (binary). Use PowerShell to check:
  ```powershell
  Get-Item "D:\IGI1\missions\location0\level1\objects.qsc" | Select-Object Name, Length
  ```
  A text file should be larger than a few KB (usually 50KB–500KB).
- Check that the model ID is correct. List all models in the level:
  ```powershell
  ls "D:\IGI1\missions\location0\level1\*.mef"
  ```
- Verify `lightmaps/lightmaps.res` exists. If not, this level may not have baked lightmaps.

### Issue: Dialog Appears But No Placements Shown

**Symptom**: A placement picker dialog pops up but the list is empty.

**Cause**: The model ID was not found in the `objects.qsc` for this level.

**Fix**: Verify the model ID matches exactly (case-sensitive, no spaces). Run the CLI to double-check:
```powershell
igi1conv lightmap list --model 435_01_1 --qsc "D:\IGI1\missions\location0\level1\objects.qsc"
```

If you see `lightmap: no bindings found`, the model isn't placed in this level.

### Issue: Lightmap is Too Dark / Too Bright

**Symptom**: The lightmap applies, but the model looks way too dark or washed out.

**Causes**:
1. **Wrong level's lightmap** — you're seeing a lightmap from a different placement of the same model in a different area of the level.
2. **Baked lighting intensity** — the original level editor may have baked with different gamma or intensity settings.

**Fix**:
- If you selected from a placement picker, try a different placement.
- Use the CLI to manually inspect the lightmap image:
  ```powershell
  igi1conv olm to-png "D:\path\to\obj00000_00000.olm" -o "D:\Temp\lightmap.png"
  ```
  Open the PNG in an image viewer to see the raw baked data.

### Issue: Lightmap Doesn't Appear After Switching Models

**Symptom**: You apply a lightmap to one model, then click a different model. The new model doesn't have a lightmap when you try to apply.

**Cause**: This is normal. Lightmaps are applied per-model — each model is independent. The first model keeps its lightmap, but the new model hasn't had one applied yet.

**Fix**: Right-click the new model → Apply Lightmap again.

### Issue: The 3D Viewer is Frozen / Unresponsive

**Symptom**: After applying a lightmap, the viewer doesn't respond to mouse clicks or shows a loading spinner.

**Cause**: Large lightmap files are being loaded. This is normal for the first application in a session.

**Fix**: Wait a few seconds for the load to complete. Future applications of the same lightmap will be faster (cached).

---

## 10. How We Solved This (Technical Background)

This section explains the design and implementation journey of the lightmap application feature. It's for developers and power users curious about how this works internally.

### The Problem

When IGI1 levels were created, the original editor:
1. **Baked lightmaps** (pre-computed, high-quality static lighting) for every placed object in the level.
2. **Stored them** as `.olm` binary files in `lightmaps_unpacked/`.
3. **Bound them** in the level script (`objects.qvm`) by storing a **logical ID** (e.g., `obj00000`) in a nested `LightmapInfo` task.
4. **The player's game engine** could read this script at runtime and blend the `.olm` file with the model's diffuse texture.

However, **modders and converters** (like igi1conv) had no way to visualize these lightmaps during model inspection or editing. When you right-clicked a model in the GUI, you saw only the diffuse texture — not the baked lighting.

### The Solution: Three Components

#### Component 1: Script Parser (`qsc_object_parser.cpp`)

**Goal**: Read the text script (`objects.qsc`) and extract lightmap bindings.

**Implementation**:
- Parse every `Task_New(...)` call in the script (the original game's task tree format).
- For each task, recursively scan **nested task calls** (which were previously discarded).
- Look for:
  1. A **model ID** in any string (e.g., `435_01_1`).
  2. A nested **`LightmapInfo` task** whose last string argument is the logical lightmap ID (e.g., `obj00000`).
- Build a **binding map**: `model_id → logical_lightmap_id`.

**Example Script Fragment** (decompiled `objects.qsc`):
```c
Task_New(-1, "Building", "WaterTower", 24658470, -55957188, 174412128, ..., "435_01_1",
    Task_New(-1, "Static", "", 
        Task_New(-1, "EditRigidObj", "", ..., "435_01_1", ...)),
    Task_New(-1, "LightmapInfo", "", 1, 1, 550, 1650, 0.8, 280.0, 0.08, 0.08, 0.08, "obj00000"));
```

The parser extracts: **`435_01_1` → `obj00000`**.

**Key Challenge Solved**: The original script parser treated nested calls as opaque `Bad` tokens. We extended it to recursively parse and analyze these nested trees, unlocking the lightmap binding information that was previously inaccessible.

#### Component 2: Lightmap Resolver (`lightmap_resolver.cpp`)

**Goal**: Given a logical ID (e.g., `obj00000`), find the actual `.olm` files on disk.

**Implementation**:
- Check if `lightmaps_unpacked/` folder exists.
- If not, but `lightmaps/lightmaps.res` exists, unpack it (reuse existing `cmd_res` logic).
- Glob for all files matching `<logical_id>_*.olm` (e.g., `obj00000_00000.olm`, `obj00000_00001.olm`, ...).
- Return sorted list.

**Example Resolution**:
```
Input:  logical_id = "obj00000", level_dir = "D:\IGI1\missions\location0\level1"
Output: [
  "D:\IGI1\missions\location0\level1\lightmaps\lightmaps_unpacked\obj00000_00000.olm",
  "D:\IGI1\missions\location0\level1\lightmaps\lightmaps_unpacked\obj00000_00001.olm",
  ...
]
```

**Key Challenge Solved**: The `lightmaps.res` is a packed binary container. We didn't want to write a new unpacker, so we reused the existing resource unpacking code and lazily unpack on first access. This is transparent to the user.

#### Component 3: MEF Parser & GPU Rendering (`mef_parser.cpp`, `gui_main.cpp`)

**Goal**: Load the `.olm` file's pixel data and blend it in the 3D viewer.

**MEF Parser Changes**:
- Models with `modeltype = 3` (`MODEL_LIGHTMAP`) have a special vertex format: **40 bytes per vertex** (instead of 32).
  - Offset 0–23: Position + normal + diffuse UV.
  - Offset 24–31: **Diffuse UV** (standard).
  - Offset 32–39: **Lightmap UV** (the second UV layer).
- The parser now extracts **both UV layers** per vertex.
- The diffuse UV is used to sample the diffuse texture; the lightmap UV is used to sample the `.olm` texture in the lightmap atlas.

**OLM File Format**:
- Binary header (88 bytes): version, dimensions, metadata.
- Layer descriptor (16 bytes in IGI1): pixel width/height.
- Pixel data: RGBA pixels, `width * height * 4` bytes.
- We parse this and upload the pixels as a GL texture.

**GPU Shader Blending**:
- The fragment shader is extended to sample **two textures**:
  1. The **diffuse texture** at `uv0`.
  2. The **lightmap texture** at `uv1`.
- Fragment color is computed as: `color = diffuse * lightmap` (multiply blend).
- This is a standard physically-based lighting approach: the lightmap contains RGB brightness multipliers baked from the original renderer.

**Key Challenge Solved**: The `.olm` format is IGI1-specific and rarely documented. We reverse-engineered it by:
1. Inspecting hex dumps of real `.olm` files from the IGI1 corpus.
2. Cross-referencing with game memory analysis and the original editor's output.
3. Writing a parser and validating against the corpus.

### Design Decisions & Why

#### Decision 1: "Don't Force Settings UI Changes"

**Why**: The GUI already had an **Animation → Objects.qsc** setting. Rather than add a new "Lightmaps Folder" setting, we derive it from the level directory (inferred from the Objects.qsc path). This keeps the UI minimal and reduces user confusion.

**Tradeoff**: If a user has multiple level directories or unusual layouts, they might need to manually manage paths. In practice, most users follow the standard IGI1 folder structure.

#### Decision 2: "Lazy Unpack of lightmaps.res"

**Why**: Unpacking large `.res` files (100MB+) upfront is slow and wastes disk space. We unpack on first use and cache the result.

**Tradeoff**: The first "Apply Lightmap" may be slightly slower (1–5 seconds for large res files). Subsequent applications are instant.

#### Decision 3: "Multiply Blending (diffuse * lightmap)"

**Why**: This is the standard in real-time game engines. The lightmap contains pre-computed radiance (light bounces, shadows, occlusion). Multiplying with the diffuse preserves surface detail.

**Alternative Not Chosen**: Additive blending or other modes would look wrong for this use case.

#### Decision 4: "Warn on Mismatch: OLM Count ≠ Render Block Count"

**Why**: A model may have multiple render blocks (sub-meshes), and ideally each gets its own `.olm` file (e.g., `obj00000_00000.olm` for block 0, `obj00000_00001.olm` for block 1). If the counts don't match (e.g., 5 render blocks but only 3 `.olm` files), we fall back to reusing the first `.olm` for all blocks and log a warning. This is safer than crashing.

### Files Involved

| File | Purpose |
|------|---------|
| `igi1conv/qsc_object_parser.h/.cpp` | Parse task trees from `objects.qsc` |
| `source/parsers/lightmap_resolver.h/.cpp` | Resolve logical IDs to `.olm` file paths |
| `source/parsers/mef_parser.h/.cpp` | Parse lightmap UV layer (XTRV chunk, offset +32) |
| `source/parsers/mef_native.h/.cpp` | OLM binary file parser |
| `igi1conv/cmd_olm.cpp` | CLI commands: `olm info`, `olm to-png`, `olm to-tga` |
| `igi1conv/cmd_lightmap.cpp` | CLI commands: `lightmap list`, `lightmap resolve` |
| `igi1conv/gui_main.cpp` | Right-click "Apply Lightmap" action + GPU rendering |

### Testing & Validation

**Unit Tests**:
- `tests/test_lightmap_binding_parser.cpp` — verify script parsing.
- `tests/test_lightmap_resolver.cpp` — verify `.olm` file resolution against real IGI1 corpus.
- `tests/test_mef_lightmap_uv.cpp` — verify lightmap UV parsing from MEF models.
- `tests/test_igi1conv_commands.cpp` — verify CLI command output.

**Integration Testing**:
- Manual: Launch GUI, apply lightmap to real models, inspect rendered output.
- Corpus: Tested against 2,895 real `.olm` files from IGI1 assets (all single-layer, as documented).

### Why This Feature Matters

1. **Modding**: Modders can now see how their replacement models look with the game's pre-baked lighting, enabling accurate visual editing.
2. **Preservation**: The converter now preserves and visualizes a crucial aspect of IGI1's visual design: baked static lighting.
3. **Non-destructive Inspection**: You can load models and lightmaps without modifying any game files.

---

## Advanced: Editing Lightmaps (Future)

(Out of scope for 1.11.0, but documented for future enhancement.)

Once you've exported a lightmap to PNG:
```powershell
igi1conv olm to-png "...obj00000_00000.olm" -o "lightmap.png"
```

You could:
1. Edit `lightmap.png` in Photoshop (darken shadows, brighten highlights, change color tones).
2. Re-import it using (future command):
   ```powershell
   igi1conv olm from-png "lightmap.png" -o "obj00000_00000.olm"
   ```
3. Re-apply in the GUI to see the changes live.

This workflow is planned but not yet implemented.

---

## Getting Help

- **CLI Reference**: See [`OLM_LIGHTMAP_CLI_GUIDE.md`](OLM_LIGHTMAP_CLI_GUIDE.md) for detailed command-line usage.
- **Format Internals**: See [`Lightmap_docs.md`](Lightmap_docs.md) for binary format specifications.
- **Report a Bug**: If you encounter issues, check the [Troubleshooting](#9-troubleshooting) section above, then open an issue on GitHub with:
  - Your level directory path.
  - The model ID you're trying to load.
  - The error message (if any).
  - The output of `igi1conv lightmap list --model <id> --qsc <path>`.

---

## Changelog

### v1.11.0 (2026-06-25)
- **Feature**: Right-click "Apply Lightmap" on `.mef` models in GUI.
- **Feature**: Automatic placement disambiguation for reused models.
- **Feature**: Real-time GPU blending of diffuse + lightmap textures.
- **CLI**: `lightmap list` and `lightmap resolve` commands for scripted workflow.
- **CLI**: `olm info`, `olm to-png`, `olm to-tga` for lightmap inspection/export.

### v1.9.7 and earlier
- No lightmap visualization.
