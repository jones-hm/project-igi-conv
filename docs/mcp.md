# Game-facing MCP support

`igi1conv` provides a Model Context Protocol server for Project IGI game-data
work. The boundary is deliberate: MCP exposes operations whose inputs or
outputs can affect files consumed by the game. It is not a remote-control API
for the editor UI.

## Start the server

The default transport is newline-delimited JSON-RPC over standard input and
output:

```text
igi1conv mcp --transport stdio
```

The server writes protocol responses only to stdout. Diagnostics are written
to stderr. A client sends one JSON-RPC object per line; notifications do not
receive a response.

Streamable HTTP is opt-in:

```text
igi1conv mcp --transport http --host 127.0.0.1 --port 8765 --endpoint /mcp
```

The endpoint accepts `POST` requests with `Content-Type: application/json`.
It returns `MCP-Protocol-Version: 2025-11-25` and JSON responses for request /
response calls. The default bind is loopback. Non-loopback binds require an
explicit token:

```text
igi1conv mcp --transport http --host 0.0.0.0 --port 8765 \
  --auth-token <secret> --origin https://trusted.example
```

Never place a real token in source control or a shared command log. HTTP
rejects an unrecognized `Origin`; clients without a browser Origin may omit
the header. The server does not execute a shell and does not accept an
arbitrary executable path.

## Protocol discovery

After `initialize`, call `tools/list` to obtain the two tools and the complete
operation enum. Call `resources/list` and then `resources/read` for:

```text
igi1conv://game-capabilities
```

That resource describes the game operation registry and the game effects of
the typed QSC fields. It contains no user GUI state.

## Tools

### `igi_game_command`

This is the generic, allowlisted adapter for existing converter operations.
`command` is a stable registry name, and `args` is an array of ordinary CLI
arguments. `working_directory` is optional and is restored before the request
finishes.

Example request arguments:

```json
{
  "command": "tex.info",
  "args": ["levels/level1/textures/FLARE00.TEX"]
}
```

Example for selecting game placements:

```json
{
  "command": "qsc.list-objects",
  "args": ["levels/level1/objects.qsc", "--json"]
}
```

The registry currently covers these game-data families:

- `dat`: `info`, `export`, `to-mtp`
- `fnt`: `info`, `export`
- `graph`: `info`, `dump`, `export`, `md`, `table`
- `iff`: `info`, `test`, `convert`, `create`, `decompile`, `rebuild`, `emit-qsc`, `export-gif`
- `lightmap`: `list`, `resolve`, `recalc`
- `mef`: `info`, `dump`, `export`, `to-text`, `compile`, `build-rigid`, `bundle`
- `mtp`: `info`, `dump`, `repair`, `sync`, `to-dat`
- `olm`: `info`, `to-png`, `to-tga`, `from-png`
- `qsc`: `validate`, `compile`, `list-objects`, `edit-object`
- `qvm`: `info`, `disasm`, `decompile`
- `res`: `list`, `extract`, `unpack`, `compile`, `pack`, `append`, `repack`
- `terrain`: `info`, `export-lmp`, `export-ctr`
- `tex`: `info`, `decode`, `to-png`, `to-tga`, `to-spr`
- `wav`: `info`, `convert`, `convert-dir`

For example, the object-edit operation is the registry name
`qsc.edit-object` and the corresponding list operation is
`qsc.list-objects`.

The MCP operation name uses a dot (`qsc.compile`) while the CLI command uses
a space (`qsc compile`). The server validates the registry name before
constructing the command vector.

### `igi_game_object_edit`

This tool writes a QSC source file through the same validated object editor as
the CLI. The required fields are `input_file`, `output_file`, and a selector:

```json
{
  "input_file": "levels/level1/objects.qsc",
  "output_file": "levels/level1/objects-edited.qsc",
  "selector": { "task_id": 701 },
  "position": [100.0, 200.0, 300.0],
  "rotation": 1.25,
  "model_id": "013_01_1",
  "team": 3,
  "bone_hierarchy": 8,
  "stand_animation": 9
}
```

Selectors can use `task_id`, `class_name`, and/or `object_name`. The combined
selector must match exactly one `Task_New` call. Missing and ambiguous
selectors fail before any output is written.

The common game placement fields map to direct arguments as follows:

| Field | `Task_New` index | Game effect |
| --- | ---: | --- |
| `task_id` | 0 | Stable placement identity / selector |
| `class_name` | 1 | Game task/object class |
| `object_name` | 2 | Placement name |
| `position` | 3, 4, 5 | Game-world X/Y/Z position |
| `rotation` | 6 | Placement gamma / rotation |
| `model_id` | 7 | Model selected by the game |
| `team` | 8 | Allegiance / AI team |
| `bone_hierarchy` | 9 | Game animation skeleton selection |
| `stand_animation` | 10 | Default game animation selection |

For task classes with additional semantics, use the generic `updates` array:

```json
{
  "input_file": "objects.qsc",
  "output_file": "objects-edited.qsc",
  "selector": { "class_name": "Weapon", "object_name": "Rifle" },
  "updates": [
    { "direct_index": 7, "literal": "\"rocket_launcher\"" },
    { "direct_index": 8, "literal": "12" }
  ]
}
```

Literals are one safe QSC token: a number, `TRUE`/`FALSE`, or a quoted
string. Commas, semicolons, newlines, malformed quotes, duplicate indexes,
and out-of-range indexes are rejected. Nested calls, comments, escaped
strings, and untouched source formatting are preserved.

The equivalent CLI commands are:

```text
igi1conv qsc list-objects objects.qsc --json
igi1conv qsc edit-object objects.qsc -o objects-edited.qsc \
  --id 701 --position 100 200 300 --rotation 1.25 \
  --model-id 013_01_1 --team 3
```

## Results and errors

Successful and failed game commands return an MCP tool result with:

- `structuredContent.exit_code`
- `structuredContent.stdout`
- `structuredContent.stderr`
- `structuredContent.output_paths`
- `isError`, set when the converter returns a nonzero exit code

The converter exit code is preserved. A failed or ambiguous edit is never
reported as a successful write.

## Explicitly excluded

The MCP surface intentionally does not expose:

- settings, themes, preferences, cache paths, or selected GUI folders;
- window/layout state, viewer or camera transforms, or animation playback;
- GUI-only previews, menus, or controls that do not produce game data;
- shell commands, arbitrary executable paths, or hidden global state changes.

If a value does not change or validate a game file or game behavior, it is out
of scope for this server.

## Verification

The repository includes focused GoogleTest coverage in
`test_mcp_operations.cpp`, `test_mcp_protocol.cpp`,
`test_mcp_transport.cpp`, and `test_qsc_object_editor.cpp`. The real-process
smoke harness is:

```powershell
./tests/mcp_smoke.ps1 -QtBin 'D:/Qt/6.5.3/msvc2019_64/bin'
```

It exercises stdio, a real QSC list/edit request, HTTP initialization,
invalid-Origin rejection, and the remote authentication guard.
