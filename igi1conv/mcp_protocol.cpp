#include "mcp_protocol.h"

#include "mcp_operations.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QString>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

namespace igi1conv {
namespace {

constexpr const char* kProtocolVersion = "2025-11-25";
constexpr const char* kCapabilitiesUri = "igi1conv://game-capabilities";

QString ToQString(const std::string& value) {
    return QString::fromUtf8(value.data(), static_cast<int>(value.size()));
}

std::string ToStdString(const QString& value) {
    const QByteArray utf8 = value.toUtf8();
    return std::string(utf8.constData(), static_cast<std::size_t>(utf8.size()));
}

QJsonObject ErrorResponse(const QJsonValue& id, int code, const QString& message) {
    QJsonObject error;
    error.insert("code", code);
    error.insert("message", message);

    QJsonObject response;
    response.insert("jsonrpc", "2.0");
    response.insert("id", id);
    response.insert("error", error);
    return response;
}

QJsonObject ResultResponse(const QJsonValue& id, const QJsonObject& result) {
    QJsonObject response;
    response.insert("jsonrpc", "2.0");
    response.insert("id", id);
    response.insert("result", result);
    return response;
}

bool IsSupportedProtocolVersion(const QString& version) {
    // These are the protocol revisions understood by the MCP clients in the
    // field.  Newer revisions must be explicitly added after compatibility
    // review rather than accepted silently.
    return version == QStringLiteral("2025-11-25")
        || version == QStringLiteral("2025-06-18")
        || version == QStringLiteral("2025-03-26")
        || version == QStringLiteral("2024-11-05");
}

QString NumberToString(const QJsonValue& value) {
    if (value.isDouble()) {
        const double number = value.toDouble();
        if (!std::isfinite(number)) return {};
        return QString::number(number, 'g', 15);
    }
    if (value.isString()) return value.toString();
    if (value.isBool()) return value.toBool() ? QStringLiteral("TRUE") : QStringLiteral("FALSE");
    return {};
}

bool ReadString(const QJsonObject& object, const char* key, std::string& value,
                QString& error, bool required = true) {
    const QJsonValue raw = object.value(key);
    if (raw.isUndefined() || raw.isNull()) {
        if (!required) {
            value.clear();
            return true;
        }
        error = QStringLiteral("missing required string argument: ") + key;
        return false;
    }
    if (!raw.isString()) {
        error = QStringLiteral("argument must be a string: ") + key;
        return false;
    }
    value = ToStdString(raw.toString());
    if (required && value.empty()) {
        error = QStringLiteral("argument must not be empty: ") + key;
        return false;
    }
    return true;
}

bool ReadInteger(const QJsonObject& object, const char* key, int& value,
                 QString& error, bool required = true) {
    const QJsonValue raw = object.value(key);
    if (raw.isUndefined() || raw.isNull()) {
        if (!required) return true;
        error = QStringLiteral("missing required integer argument: ") + key;
        return false;
    }
    if (!raw.isDouble()) {
        error = QStringLiteral("argument must be an integer: ") + key;
        return false;
    }
    const double number = raw.toDouble();
    if (!std::isfinite(number) || std::floor(number) != number
        || number < std::numeric_limits<int>::min()
        || number > std::numeric_limits<int>::max()) {
        error = QStringLiteral("argument must be an integer: ") + key;
        return false;
    }
    value = static_cast<int>(number);
    return true;
}

QString JsonString(const std::string& value) {
    return ToQString(value);
}

std::string BuildCapabilityText() {
    QJsonObject document;
    document.insert("scope", "game-facing");
    document.insert("description",
                    "Only operations that read, validate, transform, or write Project IGI game data are exposed.");

    QJsonArray operations;
    for (const auto& operation : GameOperations()) {
        QJsonObject entry;
        entry.insert("name", JsonString(operation.name));
        entry.insert("description", JsonString(operation.description));
        entry.insert("writes_game", operation.writesGame);
        QJsonArray prefix;
        for (const auto& part : operation.commandPrefix)
            prefix.append(JsonString(part));
        entry.insert("command_prefix", prefix);
        operations.append(entry);
    }
    document.insert("operations", operations);

    QJsonArray objectFields;
    const QPair<const char*, const char*> fields[] = {
        {"position", "Task_New direct arguments 3,4,5"},
        {"rotation", "Task_New direct argument 6"},
        {"model_id", "Task_New direct argument 7"},
        {"team", "Task_New direct argument 8"},
        {"bone_hierarchy", "Task_New direct argument 9"},
        {"stand_animation", "Task_New direct argument 10"},
    };
    for (const auto& field : fields) {
        QJsonObject entry;
        entry.insert("name", field.first);
        entry.insert("game_effect", field.second);
        objectFields.append(entry);
    }
    document.insert("game_object_fields", objectFields);

    // This is a scope statement, not an editor-settings API.  It makes the
    // intentional boundary discoverable without returning any GUI state or
    // allowing those values as tool arguments.
    QJsonArray excluded;
    excluded.append("GUI preferences and layout");
    excluded.append("viewer/camera transforms and animation playback");
    excluded.append("cache paths, themes, and selected folders");
    excluded.append("shell execution and arbitrary executables");
    document.insert("excluded_editor_only_surfaces", excluded);

    return ToStdString(QJsonDocument(document).toJson(QJsonDocument::Indented));
}

std::vector<std::string> OutputPaths(const std::vector<std::string>& command) {
    std::vector<std::string> result;
    for (std::size_t i = 0; i + 1 < command.size(); ++i) {
        if ((command[i] == "-o" || command[i] == "--output") && !command[i + 1].empty())
            result.push_back(command[i + 1]);
    }
    return result;
}

QJsonObject ExecutionToolResult(const McpExecutionResult& execution,
                                const std::vector<std::string>& command) {
    QJsonObject structured;
    structured.insert("exit_code", execution.exitCode);
    structured.insert("stdout", JsonString(execution.stdoutText));
    structured.insert("stderr", JsonString(execution.stderrText));
    QJsonArray outputPaths;
    for (const auto& path : OutputPaths(command)) outputPaths.append(JsonString(path));
    structured.insert("output_paths", outputPaths);

    QString summary = QStringLiteral("game command exit code %1").arg(execution.exitCode);
    if (!execution.stdoutText.empty()) summary += QStringLiteral("\n") + JsonString(execution.stdoutText);
    if (!execution.stderrText.empty()) summary += QStringLiteral("\n") + JsonString(execution.stderrText);

    QJsonObject result;
    QJsonArray content;
    content.append(QJsonObject{{"type", "text"}, {"text", summary}});
    result.insert("content", content);
    result.insert("structuredContent", structured);
    result.insert("isError", execution.exitCode != 0);
    return result;
}

QJsonObject ToolError(const QString& message) {
    QJsonObject structured;
    structured.insert("error", message);

    QJsonObject result;
    result.insert("content", QJsonArray{QJsonObject{{"type", "text"}, {"text", message}}});
    result.insert("structuredContent", structured);
    result.insert("isError", true);
    return result;
}

QJsonObject ToolSchemaForCommand() {
    QJsonArray enumValues;
    for (const auto& operation : GameOperations()) enumValues.append(JsonString(operation.name));

    QJsonObject argsItems;
    argsItems.insert("type", "string");
    QJsonObject properties;
    properties.insert("command", QJsonObject{{"type", "string"}, {"enum", enumValues}});
    properties.insert("args", QJsonObject{{"type", "array"}, {"items", argsItems}});
    properties.insert("working_directory", QJsonObject{{"type", "string"}});
    return QJsonObject{{"type", "object"}, {"properties", properties},
                       {"required", QJsonArray{"command", "args"}}, {"additionalProperties", false}};
}

QJsonObject ToolSchemaForObjectEdit() {
    QJsonObject selectorProperties;
    selectorProperties.insert("task_id", QJsonObject{{"type", "integer"}});
    selectorProperties.insert("class_name", QJsonObject{{"type", "string"}});
    selectorProperties.insert("object_name", QJsonObject{{"type", "string"}});
    QJsonObject selector = QJsonObject{{"type", "object"}, {"properties", selectorProperties},
                                       {"minProperties", 1}, {"additionalProperties", false}};

    QJsonObject updateItem = QJsonObject{
        {"type", "object"},
        {"properties", QJsonObject{{"direct_index", QJsonObject{{"type", "integer"}}},
                                    {"literal", QJsonObject{{"type", "string"}}}}},
        {"required", QJsonArray{"direct_index", "literal"}},
        {"additionalProperties", false},
    };
    QJsonObject properties{
        {"input_file", QJsonObject{{"type", "string"}}},
        {"output_file", QJsonObject{{"type", "string"}}},
        {"selector", selector},
        {"position", QJsonObject{{"type", "array"}, {"minItems", 3}, {"maxItems", 3},
                                  {"items", QJsonObject{{"type", "number"}}}}},
        {"rotation", QJsonObject{{"type", "number"}}},
        {"model_id", QJsonObject{{"type", "string"}}},
        {"team", QJsonObject{{"type", "integer"}}},
        {"bone_hierarchy", QJsonObject{{"type", "integer"}}},
        {"stand_animation", QJsonObject{{"type", "integer"}}},
        {"updates", QJsonObject{{"type", "array"}, {"items", updateItem}}},
    };
    return QJsonObject{{"type", "object"}, {"properties", properties},
                       {"required", QJsonArray{"input_file", "output_file", "selector"}},
                       {"additionalProperties", false}};
}

QJsonArray Tools() {
    QJsonObject command;
    command.insert("name", "igi_game_command");
    command.insert("description",
                   "Run one registered game-data converter operation.");
    command.insert("inputSchema", ToolSchemaForCommand());

    QJsonObject objectEdit;
    objectEdit.insert("name", "igi_game_object_edit");
    objectEdit.insert("description",
                      "List or edit game-facing QSC Task_New object data such as position, rotation, model, team, and task-specific parameters.");
    objectEdit.insert("inputSchema", ToolSchemaForObjectEdit());

    return QJsonArray{command, objectEdit};
}

std::optional<QJsonObject> HandleGameCommand(const QJsonObject& arguments,
                                             const QJsonValue& id,
                                             const McpCommandExecutor& executor) {
    QString error;
    std::string operationName;
    if (!ReadString(arguments, "command", operationName, error))
        return ResultResponse(id, ToolError(error));
    const GameOperation* operation = FindGameOperation(operationName);
    if (!operation)
        return ResultResponse(id, ToolError(QStringLiteral("operation is not registered as game-facing: ")
                                            + JsonString(operationName)));

    const QJsonValue rawArgs = arguments.value("args");
    if (!rawArgs.isArray())
        return ResultResponse(id, ToolError(QStringLiteral("args must be an array of strings")));

    std::vector<std::string> command = operation->commandPrefix;
    for (const auto& raw : rawArgs.toArray()) {
        if (!raw.isString())
            return ResultResponse(id, ToolError(QStringLiteral("args must be an array of strings")));
        command.push_back(ToStdString(raw.toString()));
    }

    std::string validationError;
    if (!IsAllowedGameCommand(command, validationError))
        return ResultResponse(id, ToolError(ToQString(validationError)));

    std::string workingDirectory;
    if (!ReadString(arguments, "working_directory", workingDirectory, error, false))
        return ResultResponse(id, ToolError(error));
    return ResultResponse(id, ExecutionToolResult(executor(command, workingDirectory), command));
}

bool AddValueArg(const QJsonObject& arguments, const char* key, const char* option,
                 std::vector<std::string>& command, QString& error) {
    const QJsonValue raw = arguments.value(key);
    if (raw.isUndefined() || raw.isNull()) return true;
    const QString value = NumberToString(raw);
    if (value.isEmpty()) {
        error = QStringLiteral("game object field must be a number, boolean, or string: ") + key;
        return false;
    }
    command.emplace_back(option);
    command.emplace_back(ToStdString(value));
    return true;
}

std::optional<QJsonObject> HandleGameObjectEdit(const QJsonObject& arguments,
                                                const QJsonValue& id,
                                                const McpCommandExecutor& executor) {
    QString error;
    std::string input;
    std::string output;
    if (!ReadString(arguments, "input_file", input, error)
        || !ReadString(arguments, "output_file", output, error))
        return ResultResponse(id, ToolError(error));

    const QJsonValue selectorValue = arguments.value("selector");
    if (!selectorValue.isObject())
        return ResultResponse(id, ToolError(QStringLiteral("selector must be an object")));
    const QJsonObject selector = selectorValue.toObject();
    std::vector<std::string> command{"qsc", "edit-object", input, "-o", output};
    bool hasSelector = false;
    if (!selector.value("task_id").isUndefined()) {
        int taskId = 0;
        if (!ReadInteger(selector, "task_id", taskId, error))
            return ResultResponse(id, ToolError(error));
        command.emplace_back("--id");
        command.emplace_back(std::to_string(taskId));
        hasSelector = true;
    }
    std::string selectorText;
    if (!ReadString(selector, "class_name", selectorText, error, false))
        return ResultResponse(id, ToolError(error));
    if (!selectorText.empty()) {
        command.emplace_back("--class");
        command.emplace_back(std::move(selectorText));
        hasSelector = true;
    }
    if (!ReadString(selector, "object_name", selectorText, error, false))
        return ResultResponse(id, ToolError(error));
    if (!selectorText.empty()) {
        command.emplace_back("--name");
        command.emplace_back(std::move(selectorText));
        hasSelector = true;
    }
    if (!hasSelector)
        return ResultResponse(id, ToolError(QStringLiteral("selector requires task_id, class_name, or object_name")));

    const QJsonValue positionValue = arguments.value("position");
    if (!positionValue.isUndefined() && !positionValue.isArray())
        return ResultResponse(id, ToolError(QStringLiteral("position must be an array of three numbers")));
    if (positionValue.isArray()) {
        const QJsonArray position = positionValue.toArray();
        if (position.size() != 3)
            return ResultResponse(id, ToolError(QStringLiteral("position must contain exactly three numbers")));
        command.emplace_back("--position");
        for (const auto& value : position) {
            const QString text = NumberToString(value);
            if (text.isEmpty() || !value.isDouble())
                return ResultResponse(id, ToolError(QStringLiteral("position values must be finite numbers")));
            command.emplace_back(ToStdString(text));
        }
    }

    if (!AddValueArg(arguments, "rotation", "--rotation", command, error))
        return ResultResponse(id, ToolError(error));

    std::string modelId;
    if (!ReadString(arguments, "model_id", modelId, error, false))
        return ResultResponse(id, ToolError(error));
    if (!modelId.empty()) {
        command.emplace_back("--model-id");
        command.emplace_back(std::move(modelId));
    }

    if (!AddValueArg(arguments, "team", "--team", command, error)
        || !AddValueArg(arguments, "bone_hierarchy", "--bone-hierarchy", command, error)
        || !AddValueArg(arguments, "stand_animation", "--stand-animation", command, error))
        return ResultResponse(id, ToolError(error));

    const QJsonValue updatesValue = arguments.value("updates");
    if (!updatesValue.isUndefined()) {
        if (!updatesValue.isArray())
            return ResultResponse(id, ToolError(QStringLiteral("updates must be an array")));
        for (const auto& rawUpdate : updatesValue.toArray()) {
            if (!rawUpdate.isObject())
                return ResultResponse(id, ToolError(QStringLiteral("each update must be an object")));
            const QJsonObject update = rawUpdate.toObject();
            int index = 0;
            std::string literal;
            if (!ReadInteger(update, "direct_index", index, error)
                || index < 0
                || !ReadString(update, "literal", literal, error)) {
                if (error.isEmpty()) error = QStringLiteral("direct_index must be non-negative");
                return ResultResponse(id, ToolError(error));
            }
            command.emplace_back("--set");
            command.emplace_back(std::to_string(index) + "=" + literal);
        }
    }

    std::string workingDirectory;
    if (!ReadString(arguments, "working_directory", workingDirectory, error, false))
        return ResultResponse(id, ToolError(error));
    return ResultResponse(id, ExecutionToolResult(executor(command, workingDirectory), command));
}

} // namespace

McpDispatcher::McpDispatcher(McpCommandExecutor executor)
    : executor_(executor ? std::move(executor) : [](const std::vector<std::string>&,
                                                   const std::string&) {
          return McpExecutionResult{1, {}, "MCP command executor is not configured"};
      }) {}

std::optional<QJsonObject> McpDispatcher::Handle(const QJsonObject& request) const {
    const bool hasId = request.contains("id");
    const QJsonValue id = hasId ? request.value("id") : QJsonValue(QJsonValue::Null);
    const auto finish = [hasId](QJsonObject response) -> std::optional<QJsonObject> {
        if (!hasId) return std::nullopt;
        return response;
    };

    if (request.value("jsonrpc").toString() != QStringLiteral("2.0"))
        return finish(ErrorResponse(id, -32600, QStringLiteral("invalid JSON-RPC request")));
    const QJsonValue methodValue = request.value("method");
    if (!methodValue.isString())
        return finish(ErrorResponse(id, -32600, QStringLiteral("method must be a string")));
    const QString method = methodValue.toString();

    if (method == QStringLiteral("notifications/initialized"))
        return std::nullopt;

    const QJsonObject params = request.value("params").isObject()
        ? request.value("params").toObject() : QJsonObject{};

    if (method == QStringLiteral("initialize")) {
        const QString requested = params.value("protocolVersion").isString()
            ? params.value("protocolVersion").toString() : QString::fromUtf8(kProtocolVersion);
        if (!IsSupportedProtocolVersion(requested))
            return finish(ErrorResponse(id, -32602, QStringLiteral("unsupported protocol version: ") + requested));

        QJsonObject capabilities;
        capabilities.insert("tools", QJsonObject{});
        capabilities.insert("resources", QJsonObject{});
        QJsonObject serverInfo;
        serverInfo.insert("name", "igi1conv");
        serverInfo.insert("version", "1.11.0");
        QJsonObject result;
        result.insert("protocolVersion", requested);
        result.insert("capabilities", capabilities);
        result.insert("serverInfo", serverInfo);
        result.insert("instructions", "Game-facing Project IGI asset editing only.");
        return finish(ResultResponse(id, result));
    }

    if (method == QStringLiteral("ping"))
        return finish(ResultResponse(id, QJsonObject{}));

    if (method == QStringLiteral("tools/list")) {
        QJsonObject result;
        result.insert("tools", Tools());
        return finish(ResultResponse(id, result));
    }

    if (method == QStringLiteral("resources/list")) {
        QJsonObject resource;
        resource.insert("uri", kCapabilitiesUri);
        resource.insert("name", "game-capabilities");
        resource.insert("description", "Game-facing converter operations and object-field effects");
        resource.insert("mimeType", "application/json");
        return finish(ResultResponse(id, QJsonObject{{"resources", QJsonArray{resource}}}));
    }

    if (method == QStringLiteral("resources/read")) {
        const QString uri = params.value("uri").toString();
        if (uri != QString::fromUtf8(kCapabilitiesUri))
            return finish(ErrorResponse(id, -32602, QStringLiteral("unknown resource URI")));
        QJsonObject content;
        content.insert("uri", kCapabilitiesUri);
        content.insert("mimeType", "application/json");
        content.insert("text", JsonString(BuildCapabilityText()));
        return finish(ResultResponse(id, QJsonObject{{"contents", QJsonArray{content}}}));
    }

    if (method == QStringLiteral("tools/call")) {
        const QString name = params.value("name").toString();
        const QJsonValue rawArguments = params.value("arguments");
        const QJsonObject arguments = rawArguments.isUndefined() || rawArguments.isNull()
            ? QJsonObject{} : rawArguments.toObject();
        if (name.isEmpty() || (!rawArguments.isUndefined() && !rawArguments.isNull()
                               && !rawArguments.isObject()))
            return finish(ResultResponse(id, ToolError(QStringLiteral("tools/call requires an object name and arguments"))));
        if (name == QStringLiteral("igi_game_command"))
            return finish(*HandleGameCommand(arguments, id, executor_));
        if (name == QStringLiteral("igi_game_object_edit"))
            return finish(*HandleGameObjectEdit(arguments, id, executor_));
        return finish(ErrorResponse(id, -32602, QStringLiteral("unknown tool: ") + name));
    }

    return finish(ErrorResponse(id, -32601, QStringLiteral("method not found: ") + method));
}

} // namespace igi1conv
