# MCP Game-Editing Support Design

## Goal

Add a standard Model Context Protocol (MCP) server to `igi1conv` so an MCP
client can inspect, transform, compile, pack, and verify Project IGI game
assets through the same supported operations as the converter. The exposed
surface is limited to data that changes or validates game behavior or game
files.

## Scope

### Included

- Existing headless converter operations that read, write, compile, repack, or
  validate game assets: textures, meshes, scripts, archives, model/texture
  metadata, terrain, navigation graphs, animations, audio, and lightmaps.
- Generic QSC task editing for game-facing object data. A caller can select a
  `Task_New` by stable task id, class, or object name and replace direct
  parameters. The common placement fields (position, gamma/rotation, model
  id, team, bone hierarchy, and stand animation) have named operations; an
  indexed parameter operation supports additional game task classes such as
  enemies, weapons, triggers, and AI containers without embedding a fixed set
  of user questions.
- MCP lifecycle, `ping`, tools, and a capabilities resource.
- Local `stdio` transport as the default and Streamable HTTP on an explicitly
  requested endpoint. HTTP binds to localhost by default, validates Origin,
  and requires an authentication token when binding to a non-loopback host.

### Excluded

- GUI preferences and state: settings, themes, cache paths, selected folders,
  window/viewer/camera transforms, animation playback controls, and editor
  layout.
- GUI-only actions that do not produce a game file or change game data.
- Shell execution, arbitrary executable paths, implicit current-user paths,
  and hidden global state changes.

Read-only inspection is exposed only when it is a game-data operation or is
needed to select, validate, or verify a game-changing edit.

## Architecture

`main.cpp` keeps one command dispatcher. The MCP server invokes that dispatcher
in-process with a validated argument vector, captures handler output, and
returns structured MCP content containing the exit code, stdout, stderr, and
detected output paths. It never invokes a shell or recursively invokes the
MCP command.

An operation registry is the source of truth for the MCP allowlist and the
capabilities resource. Each entry identifies a game-facing command family,
its supported operation syntax, whether it may write, and a concise schema
description. The generic `igi_game_command` tool accepts a registry operation
and ordinary CLI arguments, so new game operations can be added without
query-specific intent branches. A separate `igi_game_object_edit` tool exposes
typed QSC task edits and a generic indexed parameter fallback.

The protocol layer is transport-neutral:

```text
JSON-RPC request
      |
      v
MCP dispatcher -> capability/operation registry -> game command dispatcher
      |                                           |
      +---------------- structured result <--------+
```

`stdio` sends only newline-delimited JSON-RPC messages on stdout; diagnostics
go to stderr. HTTP serves one `/mcp` endpoint and uses JSON responses for
request/response calls. The server negotiates the requested protocol version
from the supported versions and rejects malformed or unsupported requests.

## Game-object editing contract

The QSC editor operates on direct arguments of balanced `Task_New(...)` calls
and preserves all untouched source text. Selection must identify exactly one
call; ambiguous or missing selectors fail before writing. Updates are written
to an explicit output file or to an explicitly requested in-place path, with
the original file left untouched until the transformed document is fully
validated.

Named fields use the known `HumanSoldier` layout:

| Field | Direct argument | Game effect |
| --- | ---: | --- |
| task id | 0 | stable placement selector |
| object class | 1 | task/object type |
| object name | 2 | placement identity |
| position X/Y/Z | 3/4/5 | object location |
| gamma | 6 | object yaw/rotation |
| model id | 7 | visual/physical model selection |
| team | 8 | allegiance/AI team |
| bone hierarchy | 9 | animation skeleton selection |
| stand animation | 10 | default animation selection |

For other task classes, `set-index` edits one direct argument using a typed
literal (number, string, or boolean) and leaves task-specific semantics to the
caller and the game's existing schema. No editor setting is accepted as a
target.

## Error and safety behavior

- JSON-RPC protocol errors use standard error codes; invalid tool arguments
  are returned as an MCP tool error with actionable text.
- Nonzero converter exit codes are preserved in the result and marked
  `isError: true`; partial output is never reported as success.
- File paths are passed as arguments, never interpolated into a shell command.
- HTTP rejects invalid Origin values and refuses non-loopback binding without
  an explicit token. The default HTTP bind is `127.0.0.1`.
- Writes require explicit output paths; in-place mutation is opt-in and
  backed by a temporary validated document before replacement.

## Verification

- Unit tests cover JSON-RPC parsing/dispatch, capability filtering, typed QSC
  selection and updates, ambiguity/malformed input rejection, and result
  shaping.
- Integration tests launch the built executable over stdio, complete
  `initialize`, list tools/resources, call a real game operation, and verify
  the output artifact. HTTP tests verify Origin/auth/binding behavior and a
  real MCP request/response round trip.
- The complete CTest suite, Release build, and a clean command-line smoke
  matrix are required before shipping.
- GUI visual state is intentionally out of scope; the existing GUI must still
  build and launch without MCP settings being persisted or changed.
