# MCP Game-Editing Support Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. All implementation steps below are complete.

**Goal:** Add a secure, transport-neutral MCP server to `igi1conv` that exposes game-affecting converter and QSC editing operations while excluding GUI-only settings and state.

**Architecture:** Keep one validated command dispatcher and route MCP tool calls to it in-process. A game-operation registry is the allowlist and capability description source; a QSC object editor provides typed placement edits plus a generic indexed task-parameter fallback. The protocol core is shared by newline-delimited stdio and localhost-first Streamable HTTP transports.

**Tech Stack:** C++20, Qt Core/Network already used by the executable, GoogleTest, CMake/CTest, JSON-RPC 2.0, MCP tools/resources.

**Spec:** `docs/superpowers/specs/2026-08-23-mcp-game-editing-design.md`

## Global Constraints

- Expose only game-affecting reads, writes, compiles, repacks, and validations; do not expose GUI settings, viewer state, cache paths, themes, or layout.
- Do not execute a shell or an arbitrary executable path from MCP input.
- Use explicit file paths and preserve non-target source text during QSC editing.
- stdio stdout must contain only valid newline-delimited JSON-RPC; diagnostics belong on stderr.
- HTTP defaults to `127.0.0.1`; validate Origin and require an auth token for non-loopback binds.
- Preserve converter exit codes and report nonzero commands as MCP tool errors.
- Follow TDD: each production behavior gets a failing test before implementation, then a green focused test and a clean refactor.

---

### Task 1: Establish the MCP operation registry and test seam

**Files:**
- Create: `igi1conv/mcp_operations.h`
- Create: `igi1conv/mcp_operations.cpp`
- Create: `tests/test_mcp_operations.cpp`
- Modify: `igi1conv/main.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- `struct GameOperation { std::string name; std::string description; bool writes; std::vector<std::string> commandPrefix; }`.
- `const std::vector<GameOperation>& GameOperations()` returns deterministic registry entries.
- `bool IsAllowedGameCommand(const std::vector<std::string>& argv, std::string& error)` accepts only registered game command prefixes and rejects `mcp`, `--gui`, unknown commands, and empty input.
- `int RunCommandVector(const std::vector<std::string>& argv)` becomes the single dispatcher seam used by `main` and later by MCP.

- [x] **Step 1: Write the failing registry and dispatcher tests**

Test deterministic operation names, acceptance of `tex info`, `qsc compile`, `res repack`, `lightmap recalc`, rejection of `mcp`/`--gui`, and preservation of the existing `--version` dispatcher behavior.

- [x] **Step 2: Run the focused tests to verify the expected failure**

Run `cmake --build build-mcp --config Release --target igi1conv_tests` after configuration. Expected failure: the new registry symbols are not defined.

- [x] **Step 3: Implement the registry and dispatcher seam**

Move the current command selection from `main` into `RunCommandVector`, retain GUI handling only in `main`, register only game-facing command families/subcommands, and keep aliases such as `mex` mapped to `mef`.

- [x] **Step 4: Run the focused tests and existing help/version smoke checks**

Run `ctest --test-dir build-mcp -C Release -R "McpOperations|IGI1Conv" --output-on-failure` and `bin/Release/igi1conv.exe --version`. Expected: PASS and version `1.11.0`.

- [x] **Step 5: Commit the seam**

```text
git add CMakeLists.txt igi1conv/main.cpp igi1conv/mcp_operations.* tests/test_mcp_operations.cpp
git commit -m "feat: add game operation registry for MCP"
```

### Task 2: Add generic game-facing QSC object editing

**Files:**
- Create: `source/parsers/qsc_object_editor.h`
- Create: `source/parsers/qsc_object_editor.cpp`
- Create: `tests/test_qsc_object_editor.cpp`
- Modify: `igi1conv/cmd_qsc.cpp`
- Modify: `igi1conv/cmd_qsc.h`
- Modify: `CMakeLists.txt`

**Interfaces:**
- `struct QscTaskSelector { std::optional<int32_t> taskId; std::string className; std::string objectName; }`.
- `struct QscFieldUpdate { size_t directIndex; std::string literal; }`.
- `struct QscEditResult { bool ok; size_t matchedCalls; size_t changedFields; std::string error; }`.
- `QscEditResult EditQscTasks(const std::string& source, const QscTaskSelector&, const std::vector<QscFieldUpdate>&, std::string& output)` preserves all bytes outside replaced direct-argument spans.
- CLI `qsc list-objects <input.qsc> [--json]` reports task id/class/name/direct arguments and known HumanSoldier fields.
- CLI `qsc edit-object <input.qsc> -o <output.qsc> [--id N|--class C|--name NAME] [--position X,Y,Z] [--rotation RADIANS] [--model-id ID] [--team N] [--bone-hierarchy N] [--stand-animation N] [--set N=LITERAL]...` requires exactly one selected task and an explicit output path.

- [x] **Step 1: Write failing tests for selection, movement, rotation, model ID, generic parameters, and safety**

Use synthetic QSC text containing two nested `Task_New` calls. Assert that position/gamma/model updates change only the selected call, indexed string/number literals support enemy/weapon/AI-style tasks, missing/ambiguous selectors fail, malformed parentheses fail, and output is unchanged on failure.

- [x] **Step 2: Run the focused editor tests and observe the expected failure**

Run `ctest --test-dir build-mcp -C Release -R QscObjectEditor --output-on-failure`. Expected: FAIL because the editor API does not exist.

- [x] **Step 3: Implement balanced Task_New scanning and span replacement**

Tokenize direct arguments while honoring quoted strings, escaped quotes, nested parentheses, and multiline whitespace. Match selectors exactly, validate numeric/vector/string literals, and replace spans in descending source-offset order.

- [x] **Step 4: Implement named CLI fields and JSON list output**

Map named HumanSoldier fields to direct indexes only after confirming class layout; use `--set` for other classes. Never infer a selector from a GUI setting or current working-directory state.

- [x] **Step 5: Run focused tests, CLI smoke tests, and compile validation**

Run `ctest --test-dir build-mcp -C Release -R "QscObjectEditor|Qsc" --output-on-failure`, then invoke `qsc list-objects` and `qsc edit-object` on the synthetic fixture and compile the edited QSC with `qsc compile`.

- [x] **Step 6: Commit QSC editing**

```text
git add CMakeLists.txt source/parsers/qsc_object_editor.* igi1conv/cmd_qsc.* tests/test_qsc_object_editor.cpp
git commit -m "feat: add game-facing QSC object editing"
```

### Task 3: Implement transport-neutral MCP JSON-RPC tools/resources

**Files:**
- Create: `igi1conv/mcp_protocol.h`
- Create: `igi1conv/mcp_protocol.cpp`
- Create: `tests/test_mcp_protocol.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- `class McpDispatcher { QJsonObject handle(const QJsonObject& request); }`.
- Supported methods: `initialize`, `ping`, `tools/list`, `tools/call`, `resources/list`, `resources/read`, and `notifications/initialized`.
- Tool `igi_game_command` accepts `{ "command": "<registered operation>", "args": ["..."], "working_directory": "..." }` and returns exit code/stdout/stderr plus changed output paths.
- Tool `igi_game_object_edit` accepts the typed selector and updates defined by Task 2.
- Resource `igi1conv://game-capabilities` returns the registry and the explicit GUI-only exclusion list.

- [x] **Step 1: Write failing protocol tests**

Test initialize negotiation, deterministic tools/list, capabilities resource content, valid command result shaping, invalid operation rejection, malformed tool arguments, unknown methods, notifications without responses, and nonzero command exit handling.

- [x] **Step 2: Run focused protocol tests to verify RED**

Run `ctest --test-dir build-mcp -C Release -R McpProtocol --output-on-failure`. Expected: FAIL with missing dispatcher behavior.

- [x] **Step 3: Implement JSON-RPC/MCP dispatcher**

Use Qt JSON types already available to the executable. Validate JSON-RPC version, request ids, method parameters, protocol versions, operation allowlist, and tool names. Capture `std::cout` and `std::cerr` around the in-process dispatcher and restore both streams with RAII.

- [x] **Step 4: Implement game command execution and object-edit tool adapters**

Convert only validated MCP arguments into `argv` vectors. Resolve an optional working directory temporarily and restore it even when a command throws. Return successful and failed tool results without leaking exception text or unrelated GUI state.

- [x] **Step 5: Run focused and regression tests**

Run `ctest --test-dir build-mcp -C Release -R "McpProtocol|McpOperations|QscObjectEditor" --output-on-failure` and the existing unit suite.

- [x] **Step 6: Commit the protocol core**

```text
git add CMakeLists.txt igi1conv/mcp_protocol.* tests/test_mcp_protocol.cpp
git commit -m "feat: expose game operations through MCP tools"
```

### Task 4: Add stdio and Streamable HTTP server entry points

**Files:**
- Create: `igi1conv/mcp_transport.h`
- Create: `igi1conv/mcp_transport.cpp`
- Create: `tests/test_mcp_transport.cpp`
- Modify: `igi1conv/main.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- `int RunMcpStdio(const McpDispatcher&)` reads one JSON-RPC message per line and writes exactly one response line for requests.
- `int RunMcpHttp(const McpDispatcher&, const HttpOptions&)` serves `/mcp` using Qt Network.
- `HttpOptions` defaults to host `127.0.0.1`, port `8765`, endpoint `/mcp`, and no remote bind without `--auth-token`.
- `igi1conv mcp --transport stdio|http [--host HOST] [--port PORT] [--auth-token TOKEN]` is the only server entry point.

- [x] **Step 1: Write failing transport tests**

Test stdio framing with initialize and tools/list, stdout purity, malformed-line errors, HTTP POST round trip, wrong endpoint rejection, invalid Origin rejection, missing/invalid auth rejection, loopback default, and graceful shutdown on stdin EOF.

- [x] **Step 2: Run focused transport tests to verify RED**

Run `ctest --test-dir build-mcp -C Release -R McpTransport --output-on-failure`. Expected: FAIL because no transport implementation exists.

- [x] **Step 3: Implement stdio transport**

Read bounded lines, parse JSON, dispatch, serialize compact JSON, flush after each response, and send diagnostics to stderr only. Do not construct a GUI application in stdio mode.

- [x] **Step 4: Implement HTTP transport**

Use `QTcpServer`/`QTcpSocket` on one endpoint. Parse bounded HTTP headers/body, enforce POST content type, validate Origin against loopback/configured origins, enforce the auth token when configured or required for non-loopback binds, and return JSON-RPC responses with MCP content types.

- [x] **Step 5: Run protocol-level smoke tests against a real process**

Launch `bin/Release/igi1conv.exe mcp --transport stdio`, send `initialize`, `notifications/initialized`, `tools/list`, and a safe real `tex info` or synthetic `qsc list-objects` call. Launch HTTP on a test port and repeat through `/mcp` with the required headers.

- [x] **Step 6: Commit the transports**

```text
git add CMakeLists.txt igi1conv/main.cpp igi1conv/mcp_transport.* tests/test_mcp_transport.cpp
git commit -m "feat: add stdio and HTTP MCP transports"
```

### Task 5: Documentation, changelog, and release checks

**Files:**
- Modify: `README.md`
- Modify: `CHANGELOG.md`
- Modify: `docs/SUPPORTED_FORMATS.md`
- Create: `docs/mcp.md`
- Create: `tests/mcp_stdio_smoke.ps1`

- [x] **Step 1: Write documentation acceptance checks**

Add a documentation test/script that verifies README and `docs/mcp.md` contain the executable command, both transports, the game-only scope, QSC edit examples for position/rotation/model ID, HTTP safety defaults, and the GUI-only exclusion list.

- [x] **Step 2: Document the complete capability registry**

Provide copyable stdio and HTTP client configuration, tool input examples, operation discovery through `tools/list` and `igi1conv://game-capabilities`, QSC selection/ambiguity rules, output-file safety, and the list of intentionally excluded settings.

- [x] **Step 3: Update changelog and supported-format matrix**

Record the MCP feature, QSC game-object editing, transports, and test evidence without claiming GUI settings are exposed.

- [x] **Step 4: Run the full verification matrix**

Run a clean Release configure/build, `ctest --test-dir build-mcp -C Release --output-on-failure`, the stdio/HTTP process smoke script, existing CLI help/version checks, and a GUI launch/offscreen smoke that verifies the original settings surface is unchanged.

- [x] **Step 5: Review the final diff and commit documentation**

```text
git diff --check
git status --short
git add README.md CHANGELOG.md docs/SUPPORTED_FORMATS.md docs/mcp.md tests/mcp_stdio_smoke.ps1
git commit -m "docs: document game-facing MCP support"
```

- [x] **Step 6: Final audit and ship**

Verify all commits, test output, generated artifacts, branch state, and worktree state. Push the feature branch if a configured remote is available; otherwise report the exact local branch and the remaining external merge/push gate.
