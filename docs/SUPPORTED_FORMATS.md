# IGI Game Converter — Supported Formats & Conversions

This document lists the supported file formats, their conversion targets, and what is currently missing or planned for future versions.

## Supported Formats

| Format | Description | Conversion / Operations | Status |
| :--- | :--- | :--- | :--- |
| **.res** | Resource Archive | List, extract, compile, pack, unpack, append, name-preserving repack | Supported |
| **.qvm** | Script Bytecode | Decompile to `.qsc`, disassemble, info | Supported |
| **.qsc** | Script Source Code | Validate, compile back to `.qvm`, list/edit game `Task_New` objects | Supported |
| **.tex** | Texture Format | Decode, info, convert to `.png` / `.tga`, resize | Supported |
| **.spr** | Sprite Format | Decode, info, convert to `.png` / `.tga`, resize | Supported |
| **.pic** | Image Format | Decode, info, convert to `.png` / `.tga`, resize | Supported |
| **.mef** | 3D Mesh Format | Info, dump, export to `.obj` + `.mtl` bundle, export to text MEF, compile text MEF to binary | Supported |
| **.mtp** | Model-Texture Package | Info, dump to JSON, compile/sync to `.dat` | Supported |
| **.dat** | Level Mapping / Asset List | Info, export to JSON, compile to `.mtp` / `.dat` | Supported |
| **.fnt** | Font Format | Info, export font texture atlas to `.png` | Supported |
| **.lmp** | Terrain Heightmap | Info, export to PGM (`.pgm`) | Supported |
| **.ctr** | Terrain Cube properties | Info, export to JSON | Supported |
| **graph*.dat** | AI Navigation Graph | Info, export to JSON | Supported |
| **.IFF** | Skeletal Animation | Info, decompile to text, convert to `.BEF`, create from `.BEF` / decompile text, rebuild round trip, export animated GIF | Supported |
| **.wav** (IGI) | ILSF audio container | Info, convert RAW / RAW_RESIDENT / ADPCM / ADPCM_RESIDENT to standard `.wav` (no external deps, single binary), batch convert-dir | Supported (all four methods) |

## MCP exposure

The MCP server exposes the registered game-affecting CLI operations in this matrix,
including read-only inspection needed to select or verify a game edit. The
inherently in-place CLI operations `lightmap.recalc`, `mtp.repair`, and
`mtp.sync` remain CLI-only until they have distinct-output modes; the
editor-only `iff.export-gif` preview is also not exposed. MCP does not expose
GUI settings, themes, cache paths, viewer/camera state, playback, layout, or
other editor-only behavior. See [mcp.md](mcp.md) for the complete registry and
tool schemas.

---

## Missing & Planned Formats (Future Support)

See [ISSUES.md](docs/ISSUES.md) for missing and planned formats.

---

## Missing Conversions (Read-Only Formats)

While several formats are supported for extraction or viewing, the write/compile operations for injecting them back into the game are currently unimplemented (read-only):

1.  **3D Models (OBJ → MEF)**: Currently, we can only export `.mef` to `.obj`. Compiling modified `.obj` files back into the game's native `.mef` mesh format is not yet supported.
2.  **Audio Encoding (WAV → IGI-ADPCM)**: Converting standard WAV files back into the game's custom compressed ADPCM sound files.
3.  **Fonts (PNG → FNT)**: Recompiling edited font texture sheets back into the `.fnt` format.
4.  **Terrain (PGM/JSON → LMP/CTR)**: Rebuilding terrain heightmaps and cube properties from standard formats.
5.  **AI Navigation (JSON → Graph)**: Recompiling JSON AI node graphs back into binary `graph*.dat` files.

