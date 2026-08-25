#include <gtest/gtest.h>

#include "mcp_protocol.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

#include <filesystem>
#include <string>
#include <vector>

namespace {

QJsonObject Request(int id, const char* method, const QJsonObject& params = {}) {
    QJsonObject request;
    request.insert("jsonrpc", "2.0");
    request.insert("id", id);
    request.insert("method", method);
    if (!params.isEmpty()) request.insert("params", params);
    return request;
}

QJsonObject ToolCall(int id, const char* name, const QJsonObject& arguments) {
    QJsonObject params;
    params.insert("name", name);
    params.insert("arguments", arguments);
    return Request(id, "tools/call", params);
}

bool Initialize(igi1conv::McpDispatcher& dispatcher, int id = 100) {
    const auto response = dispatcher.Handle(Request(id, "initialize", [] {
        return QJsonObject{
            {"protocolVersion", "2025-11-25"},
            {"capabilities", QJsonObject{}},
            {"clientInfo", QJsonObject{{"name", "test"}, {"version", "1"}}},
        };
    }()));
    return response.has_value() && response->value("error").isUndefined();
}

} // namespace

TEST(McpProtocol, InitializesListsOnlyGameFacingToolsAndExposesCapabilities) {
    igi1conv::McpDispatcher dispatcher;

    const auto initialize = dispatcher.Handle(Request(1, "initialize", [] {
        QJsonObject params;
        params.insert("protocolVersion", "2025-11-25");
        params.insert("capabilities", QJsonObject{});
        params.insert("clientInfo", QJsonObject{{"name", "test"}, {"version", "1"}});
        return params;
    }()));
    ASSERT_TRUE(initialize.has_value());
    ASSERT_TRUE(initialize->value("result").isObject());
    EXPECT_EQ(initialize->value("result").toObject().value("protocolVersion").toString(),
              "2025-11-25");

    const auto tools = dispatcher.Handle(Request(2, "tools/list"));
    ASSERT_TRUE(tools.has_value());
    const auto toolArray = tools->value("result").toObject().value("tools").toArray();
    ASSERT_EQ(toolArray.size(), 2);
    const auto toolListing = QString::fromUtf8(QJsonDocument(*tools).toJson(QJsonDocument::Compact));
    EXPECT_TRUE(toolListing.contains("igi_game_command"));
    EXPECT_TRUE(toolListing.contains("igi_game_object_edit"));
    EXPECT_FALSE(toolListing.contains("settings"));
    EXPECT_FALSE(toolListing.contains("viewer"));
    EXPECT_FALSE(toolListing.contains("camera"));

    QJsonObject objectTool;
    for (const auto& tool : toolArray) {
        if (tool.toObject().value("name").toString() == "igi_game_object_edit")
            objectTool = tool.toObject();
    }
    ASSERT_FALSE(objectTool.isEmpty());
    EXPECT_TRUE(objectTool.value("inputSchema").toObject().value("properties")
                    .toObject().contains("working_directory"));

    const auto resource = dispatcher.Handle(Request(3, "resources/read", [] {
        QJsonObject params;
        params.insert("uri", "igi1conv://game-capabilities");
        return params;
    }()));
    ASSERT_TRUE(resource.has_value());
    const auto contents = resource->value("result").toObject().value("contents").toArray();
    ASSERT_EQ(contents.size(), 1);
    const auto capabilityText = contents.at(0).toObject().value("text").toString();
    EXPECT_TRUE(capabilityText.contains("qsc.edit-object"));
    EXPECT_TRUE(capabilityText.contains("game-facing"));
}

TEST(McpProtocol, ExecutesRegisteredGameCommandAndShapesStructuredResult) {
    std::vector<std::string> actualCommand;
    std::string actualWorkingDirectory;
    igi1conv::McpDispatcher dispatcher(
        [&](const std::vector<std::string>& command, const std::string& workingDirectory) {
            actualCommand = command;
            actualWorkingDirectory = workingDirectory;
            return igi1conv::McpExecutionResult{0, "texture information", ""};
        });
    ASSERT_TRUE(Initialize(dispatcher));

    QJsonObject arguments;
    arguments.insert("command", "tex.info");
    arguments.insert("args", QJsonArray{"level.tex"});
    arguments.insert("working_directory", "D:/game");
    const auto response = dispatcher.Handle(ToolCall(4, "igi_game_command", arguments));

    ASSERT_TRUE(response.has_value());
    EXPECT_FALSE(response->value("error").isObject());
    const auto result = response->value("result").toObject();
    EXPECT_FALSE(result.value("isError").toBool());
    EXPECT_EQ(result.value("structuredContent").toObject().value("exit_code").toInt(), 0);
    EXPECT_EQ(actualCommand, (std::vector<std::string>{"tex", "info", "level.tex"}));
    EXPECT_EQ(actualWorkingDirectory, "D:/game");

    QJsonObject graphArguments;
    graphArguments.insert("command", "graph.export");
    graphArguments.insert("args", QJsonArray{"level.dat", "--out", "graph.json"});
    const auto graphResponse = dispatcher.Handle(ToolCall(41, "igi_game_command", graphArguments));
    ASSERT_TRUE(graphResponse.has_value());
    const auto graphOutputPaths = graphResponse->value("result").toObject()
                                      .value("structuredContent").toObject()
                                      .value("output_paths").toArray();
    ASSERT_EQ(graphOutputPaths.size(), 1);
    EXPECT_EQ(graphOutputPaths.at(0).toString(), "graph.json");
}

TEST(McpProtocol, RejectsEditorOnlyCommandsAndMalformedToolArguments) {
    igi1conv::McpDispatcher dispatcher(
        [](const std::vector<std::string>&, const std::string&) {
            return igi1conv::McpExecutionResult{0, "unexpected", ""};
        });
    ASSERT_TRUE(Initialize(dispatcher));

    QJsonObject invalidCommand;
    invalidCommand.insert("command", "settings.theme");
    invalidCommand.insert("args", QJsonArray{});
    const auto rejected = dispatcher.Handle(ToolCall(5, "igi_game_command", invalidCommand));
    ASSERT_TRUE(rejected.has_value());
    EXPECT_TRUE(rejected->value("result").toObject().value("isError").toBool());
    EXPECT_TRUE(rejected->value("result").toObject().value("structuredContent")
                    .toObject().value("error").toString().contains("not registered"));

    QJsonObject malformed;
    malformed.insert("command", "tex.info");
    malformed.insert("args", QJsonArray{42});
    const auto badArgs = dispatcher.Handle(ToolCall(6, "igi_game_command", malformed));
    ASSERT_TRUE(badArgs.has_value());
    EXPECT_TRUE(badArgs->value("result").toObject().value("isError").toBool());
    EXPECT_TRUE(badArgs->value("result").toObject().value("structuredContent")
                    .toObject().value("error").toString().contains("args"));
}

TEST(McpProtocol, ReportsNonzeroCommandExitAndSupportsTypedGameObjectEditing) {
    std::vector<std::string> actualCommand;
    igi1conv::McpDispatcher dispatcher(
        [&](const std::vector<std::string>& command, const std::string&) {
            actualCommand = command;
            if (command.size() > 1 && command[1] == "edit-object")
                return igi1conv::McpExecutionResult{0, "edited", ""};
            return igi1conv::McpExecutionResult{3, "", "parse failed"};
        });
    ASSERT_TRUE(Initialize(dispatcher));

    QJsonObject failedArgs;
    failedArgs.insert("command", "qsc.validate");
    failedArgs.insert("args", QJsonArray{"broken.qsc"});
    const auto failed = dispatcher.Handle(ToolCall(7, "igi_game_command", failedArgs));
    ASSERT_TRUE(failed.has_value());
    EXPECT_TRUE(failed->value("result").toObject().value("isError").toBool());
    EXPECT_EQ(failed->value("result").toObject().value("structuredContent")
                  .toObject().value("exit_code").toInt(), 3);

    QJsonObject objectArgs;
    objectArgs.insert("input_file", "objects.qsc");
    objectArgs.insert("output_file", "edited.qsc");
    QJsonObject selector;
    selector.insert("task_id", 401);
    objectArgs.insert("selector", selector);
    QJsonArray position;
    position << 10.0 << 20.0 << 30.0;
    objectArgs.insert("position", position);
    objectArgs.insert("rotation", 1.5);
    objectArgs.insert("model_id", "soldier_model");
    objectArgs.insert("team", 2);
    QJsonArray updates;
    updates.append(QJsonObject{{"direct_index", 11}, {"literal", "TRUE"}});
    objectArgs.insert("updates", updates);

    const auto edited = dispatcher.Handle(ToolCall(8, "igi_game_object_edit", objectArgs));
    ASSERT_TRUE(edited.has_value());
    EXPECT_FALSE(edited->value("result").toObject().value("isError").toBool());
    EXPECT_EQ(actualCommand,
              (std::vector<std::string>{"qsc", "edit-object", "objects.qsc", "-o", "edited.qsc",
                                        "--id", "401", "--position", "10", "20", "30",
                                        "--rotation", "1.5", "--model-id", "soldier_model",
                                        "--team", "2", "--set", "11=TRUE"}));
}

TEST(McpProtocol, HandlesNotificationsAndUnknownMethodsAccordingToJsonRpc) {
    igi1conv::McpDispatcher dispatcher;
    ASSERT_TRUE(Initialize(dispatcher));

    QJsonObject notification;
    notification.insert("jsonrpc", "2.0");
    notification.insert("method", "notifications/initialized");
    EXPECT_FALSE(dispatcher.Handle(notification).has_value());

    const auto unknown = dispatcher.Handle(Request(9, "not-a-method"));
    ASSERT_TRUE(unknown.has_value());
    EXPECT_EQ(unknown->value("error").toObject().value("code").toInt(), -32601);

    const auto malformed = dispatcher.Handle(QJsonObject{});
    ASSERT_TRUE(malformed.has_value());
    EXPECT_EQ(malformed->value("error").toObject().value("code").toInt(), -32600);
    EXPECT_TRUE(malformed->value("id").isNull());
}

TEST(McpProtocol, RejectsWrongTypesForTypedGameObjectFields) {
    bool executed = false;
    igi1conv::McpDispatcher dispatcher(
        [&](const std::vector<std::string>&, const std::string&) {
            executed = true;
            return igi1conv::McpExecutionResult{0, "unexpected", ""};
        });
    ASSERT_TRUE(Initialize(dispatcher));

    QJsonObject arguments;
    arguments.insert("input_file", "objects.qsc");
    arguments.insert("output_file", "edited.qsc");
    arguments.insert("selector", QJsonObject{{"task_id", 401}});
    arguments.insert("rotation", "nan");
    const auto response = dispatcher.Handle(ToolCall(10, "igi_game_object_edit", arguments));

    ASSERT_TRUE(response.has_value());
    EXPECT_TRUE(response->value("result").toObject().value("isError").toBool());
    EXPECT_FALSE(executed);
}

TEST(McpProtocol, RejectsUnknownFieldsDeclaredInvalidBySchemas) {
    bool executed = false;
    igi1conv::McpDispatcher dispatcher(
        [&](const std::vector<std::string>&, const std::string&) {
            executed = true;
            return igi1conv::McpExecutionResult{0, "unexpected", ""};
        });
    ASSERT_TRUE(Initialize(dispatcher));

    QJsonObject commandArgs{{"command", "tex.info"}, {"args", QJsonArray{}},
                            {"camera", "editor-only"}};
    const auto commandResponse = dispatcher.Handle(ToolCall(12, "igi_game_command", commandArgs));
    ASSERT_TRUE(commandResponse.has_value());
    EXPECT_TRUE(commandResponse->value("result").toObject().value("isError").toBool());

    QJsonObject objectArgs{{"input_file", "objects.qsc"}, {"output_file", "edited.qsc"},
                           {"selector", QJsonObject{{"task_id", 401}, {"camera", true}}}};
    const auto selectorResponse = dispatcher.Handle(ToolCall(13, "igi_game_object_edit", objectArgs));
    ASSERT_TRUE(selectorResponse.has_value());
    EXPECT_TRUE(selectorResponse->value("result").toObject().value("isError").toBool());

    objectArgs.insert("selector", QJsonObject{{"task_id", 401}});
    objectArgs.insert("updates", QJsonArray{QJsonObject{{"direct_index", 11},
                                                         {"literal", "TRUE"},
                                                         {"camera", false}}});
    const auto updateResponse = dispatcher.Handle(ToolCall(14, "igi_game_object_edit", objectArgs));
    ASSERT_TRUE(updateResponse.has_value());
    EXPECT_TRUE(updateResponse->value("result").toObject().value("isError").toBool());
    EXPECT_FALSE(executed);
}

TEST(McpProtocol, RejectsNamedPlacementFieldsForNonHumanSoldierSelector) {
    igi1conv::McpDispatcher dispatcher(
        [](const std::vector<std::string>&, const std::string&) {
            return igi1conv::McpExecutionResult{0, "unexpected", ""};
        });
    ASSERT_TRUE(Initialize(dispatcher));
    QJsonObject arguments{{"input_file", "objects.qsc"}, {"output_file", "edited.qsc"},
                          {"selector", QJsonObject{{"class_name", "Weapon"}}},
                          {"model_id", "rifle"}};
    const auto response = dispatcher.Handle(ToolCall(15, "igi_game_object_edit", arguments));
    ASSERT_TRUE(response.has_value());
    EXPECT_TRUE(response->value("result").toObject().value("isError").toBool());
    EXPECT_TRUE(response->value("result").toObject().value("structuredContent")
                    .toObject().value("error").toString().contains("HumanSoldier"));
}

TEST(McpProtocol, NegotiatesToSupportedVersionForNewerClient) {
    igi1conv::McpDispatcher dispatcher;
    QJsonObject params{
        {"protocolVersion", "2099-01-01"},
        {"capabilities", QJsonObject{}},
        {"clientInfo", QJsonObject{{"name", "future-client"}, {"version", "1"}}},
    };
    const auto response = dispatcher.Handle(Request(11, "initialize", params));
    ASSERT_TRUE(response.has_value());
    EXPECT_EQ(response->value("result").toObject().value("protocolVersion").toString(),
              "2025-11-25");
}

TEST(McpProtocol, RequiresInitializeBeforeTools) {
    bool executed = false;
    igi1conv::McpDispatcher dispatcher(
        [&](const std::vector<std::string>&, const std::string&) {
            executed = true;
            return igi1conv::McpExecutionResult{0, "unexpected", ""};
        });

    const auto beforeInitialize = dispatcher.Handle(Request(20, "tools/list"));
    ASSERT_TRUE(beforeInitialize.has_value());
    EXPECT_EQ(beforeInitialize->value("error").toObject().value("code").toInt(), -32002);
    EXPECT_FALSE(executed);
    ASSERT_TRUE(Initialize(dispatcher, 21));
    const auto afterInitialize = dispatcher.Handle(Request(22, "tools/list"));
    ASSERT_TRUE(afterInitialize.has_value());
    EXPECT_TRUE(afterInitialize->value("result").toObject().value("tools").isArray());
}

TEST(McpProtocol, PreInitializeLifecycleNotificationRemainsSilent) {
    igi1conv::McpDispatcher dispatcher;
    const QJsonObject notification{{"jsonrpc", "2.0"},
                                   {"method", "notifications/initialized"}};
    EXPECT_FALSE(dispatcher.Handle(notification).has_value());

    const auto beforeInitialize = dispatcher.Handle(Request(24, "tools/list"));
    ASSERT_TRUE(beforeInitialize.has_value());
    EXPECT_EQ(beforeInitialize->value("error").toObject().value("code").toInt(), -32002);
}

TEST(McpProtocol, RejectsInPlaceGameCommandOutputBeforeExecution) {
    bool executed = false;
    igi1conv::McpDispatcher dispatcher(
        [&](const std::vector<std::string>&, const std::string&) {
            executed = true;
            return igi1conv::McpExecutionResult{0, "unexpected", ""};
        });
    ASSERT_TRUE(Initialize(dispatcher));

    const QJsonObject arguments{
        {"command", "qsc.compile"},
        {"args", QJsonArray{"objects.qsc", "-o", "objects.qsc"}},
    };
    const auto response = dispatcher.Handle(ToolCall(23, "igi_game_command", arguments));
    ASSERT_TRUE(response.has_value());
    EXPECT_TRUE(response->value("result").toObject().value("isError").toBool());
    EXPECT_TRUE(response->value("result").toObject().value("structuredContent")
                    .toObject().value("error").toString().contains("differ"));
    EXPECT_FALSE(executed);
}

TEST(McpProtocol, RejectsPositionalAndDefaultInPlaceGameOutputs) {
    int executions = 0;
    igi1conv::McpDispatcher dispatcher(
        [&](const std::vector<std::string>&, const std::string&) {
            ++executions;
            return igi1conv::McpExecutionResult{0, "unexpected", ""};
        });
    ASSERT_TRUE(Initialize(dispatcher));

    const QJsonObject positional{
        {"command", "iff.rebuild"},
        {"args", QJsonArray{"walk.iff", "walk.iff"}},
    };
    const auto positionalResponse = dispatcher.Handle(ToolCall(25, "igi_game_command", positional));
    ASSERT_TRUE(positionalResponse.has_value());
    EXPECT_TRUE(positionalResponse->value("result").toObject().value("isError").toBool());

    const QJsonObject implicitOutput{
        {"command", "mef.build-rigid"},
        {"args", QJsonArray{"model.mef"}},
    };
    const auto implicitResponse = dispatcher.Handle(ToolCall(26, "igi_game_command", implicitOutput));
    ASSERT_TRUE(implicitResponse.has_value());
    EXPECT_TRUE(implicitResponse->value("result").toObject().value("isError").toBool());
    EXPECT_EQ(executions, 0);
}

TEST(McpProtocol, RejectsDirectoryAndSecondaryInputOutputCollisions) {
    bool executed = false;
    igi1conv::McpDispatcher dispatcher(
        [&](const std::vector<std::string>&, const std::string&) {
            executed = true;
            return igi1conv::McpExecutionResult{0, "unexpected", ""};
        });
    ASSERT_TRUE(Initialize(dispatcher));

    const auto root = std::filesystem::temp_directory_path() / "igi1conv-mcp-collision-test";
    std::error_code cleanupError;
    std::filesystem::remove_all(root, cleanupError);
    std::filesystem::create_directories(root / "source");

    const QJsonObject pack{
        {"command", "res.pack"},
        {"args", QJsonArray{"source", "source/output.res"}},
        {"working_directory", QString::fromStdString(root.string())},
    };
    const auto packResponse = dispatcher.Handle(ToolCall(29, "igi_game_command", pack));
    ASSERT_TRUE(packResponse.has_value());
    EXPECT_TRUE(packResponse->value("result").toObject().value("isError").toBool());

    const QJsonObject templateCollision{
        {"command", "olm.from-png"},
        {"args", QJsonArray{"input.png", "--template", "ref.olm", "-o", "ref.olm"}},
    };
    const auto templateResponse = dispatcher.Handle(
        ToolCall(30, "igi_game_command", templateCollision));
    ASSERT_TRUE(templateResponse.has_value());
    EXPECT_TRUE(templateResponse->value("result").toObject().value("isError").toBool());
    EXPECT_FALSE(executed);
    std::filesystem::remove_all(root, cleanupError);
}

TEST(McpProtocol, RejectsExplicitlyEmptyModelId) {
    bool executed = false;
    igi1conv::McpDispatcher dispatcher(
        [&](const std::vector<std::string>&, const std::string&) {
            executed = true;
            return igi1conv::McpExecutionResult{0, "unexpected", ""};
        });
    ASSERT_TRUE(Initialize(dispatcher));
    const QJsonObject arguments{
        {"input_file", "objects.qsc"}, {"output_file", "edited.qsc"},
        {"selector", QJsonObject{{"task_id", 701}}}, {"model_id", ""},
    };
    const auto response = dispatcher.Handle(ToolCall(31, "igi_game_object_edit", arguments));
    ASSERT_TRUE(response.has_value());
    EXPECT_TRUE(response->value("result").toObject().value("isError").toBool());
    EXPECT_FALSE(executed);
}

TEST(McpProtocol, ResolvesInPlaceChecksRelativeToWorkingDirectory) {
    bool executed = false;
    igi1conv::McpDispatcher dispatcher(
        [&](const std::vector<std::string>&, const std::string&) {
            executed = true;
            return igi1conv::McpExecutionResult{0, "unexpected", ""};
        });
    ASSERT_TRUE(Initialize(dispatcher));

    const QJsonObject arguments{
        {"command", "qsc.compile"},
        {"args", QJsonArray{"objects.qsc", "-o", "./objects.qsc"}},
        {"working_directory", "D:/IGI1"},
    };
    const auto response = dispatcher.Handle(ToolCall(27, "igi_game_command", arguments));
    ASSERT_TRUE(response.has_value());
    EXPECT_TRUE(response->value("result").toObject().value("isError").toBool());
    EXPECT_FALSE(executed);
}

TEST(McpProtocol, RejectsInvalidJsonRpcIdTypes) {
    igi1conv::McpDispatcher dispatcher;
    const QJsonObject request{
        {"jsonrpc", "2.0"},
        {"id", true},
        {"method", "initialize"},
        {"params", QJsonObject{
            {"protocolVersion", "2025-11-25"},
            {"capabilities", QJsonObject{}},
            {"clientInfo", QJsonObject{{"name", "test"}, {"version", "1"}}},
        }},
    };
    const auto response = dispatcher.Handle(request);
    ASSERT_TRUE(response.has_value());
    EXPECT_EQ(response->value("error").toObject().value("code").toInt(), -32600);
    EXPECT_TRUE(response->value("id").isNull());
}

TEST(McpProtocol, NotificationInitializeDoesNotCompleteLifecycle) {
    igi1conv::McpDispatcher dispatcher;
    const QJsonObject notification{
        {"jsonrpc", "2.0"},
        {"method", "initialize"},
        {"params", QJsonObject{
            {"protocolVersion", "2025-11-25"},
            {"capabilities", QJsonObject{}},
            {"clientInfo", QJsonObject{{"name", "test"}, {"version", "1"}}},
        }},
    };
    EXPECT_FALSE(dispatcher.Handle(notification).has_value());
    const auto response = dispatcher.Handle(Request(28, "tools/list"));
    ASSERT_TRUE(response.has_value());
    EXPECT_EQ(response->value("error").toObject().value("code").toInt(), -32002);
}
