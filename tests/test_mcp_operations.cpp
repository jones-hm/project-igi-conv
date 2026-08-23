#include <gtest/gtest.h>

#include "mcp_operations.h"

#include <algorithm>
#include <string>
#include <vector>

TEST(McpOperations, RegistryIsDeterministicAndGameFacing) {
    const auto& operations = igi1conv::GameOperations();
    ASSERT_FALSE(operations.empty());

    std::vector<std::string> names;
    names.reserve(operations.size());
    for (const auto& operation : operations) {
        ASSERT_FALSE(operation.name.empty());
        ASSERT_FALSE(operation.description.empty());
        ASSERT_FALSE(operation.commandPrefix.empty());
        names.push_back(operation.name);
        EXPECT_EQ(operation.name.find("settings"), std::string::npos);
        EXPECT_EQ(operation.name.find("viewer"), std::string::npos);
        EXPECT_EQ(operation.name.find("camera"), std::string::npos);
    }

    EXPECT_TRUE(std::is_sorted(names.begin(), names.end()));
    EXPECT_NE(std::find(names.begin(), names.end(), "tex.info"), names.end());
    EXPECT_NE(std::find(names.begin(), names.end(), "qsc.compile"), names.end());
    EXPECT_NE(std::find(names.begin(), names.end(), "res.repack"), names.end());
    EXPECT_NE(std::find(names.begin(), names.end(), "lightmap.recalc"), names.end());
    EXPECT_NE(std::find(names.begin(), names.end(), "graph.md"), names.end());
    EXPECT_NE(std::find(names.begin(), names.end(), "iff.export-gif"), names.end());
    EXPECT_NE(std::find(names.begin(), names.end(), "mtp.sync"), names.end());
}

TEST(McpOperations, AllowsRegisteredGameCommandsAndRejectsEditorOrUnknownCommands) {
    std::string error;

    EXPECT_TRUE(igi1conv::IsAllowedGameCommand({"tex", "info", "level.tex"}, error))
        << error;
    EXPECT_TRUE(igi1conv::IsAllowedGameCommand({"qsc", "compile", "objects.qsc", "-o", "objects.qvm"}, error))
        << error;
    EXPECT_TRUE(igi1conv::IsAllowedGameCommand({"res", "repack", "source.res", "assets", "-o", "out.res"}, error))
        << error;
    EXPECT_TRUE(igi1conv::IsAllowedGameCommand({"lightmap", "recalc", "--model", "435_01_1"}, error))
        << error;
    EXPECT_TRUE(igi1conv::IsAllowedGameCommand({"graph", "md", "graph.bin"}, error))
        << error;
    EXPECT_TRUE(igi1conv::IsAllowedGameCommand({"iff", "export-gif", "walk.iff"}, error))
        << error;
    EXPECT_TRUE(igi1conv::IsAllowedGameCommand({"mtp", "sync", "level.mtp", "level.dat"}, error))
        << error;

    EXPECT_FALSE(igi1conv::IsAllowedGameCommand({}, error));
    EXPECT_FALSE(igi1conv::IsAllowedGameCommand({"mcp", "--transport", "stdio"}, error));
    EXPECT_FALSE(igi1conv::IsAllowedGameCommand({"test", "--game-path", "game"}, error));
    EXPECT_FALSE(igi1conv::IsAllowedGameCommand({"--gui"}, error));
    EXPECT_FALSE(igi1conv::IsAllowedGameCommand({"settings", "set", "theme", "dark"}, error));
    EXPECT_FALSE(igi1conv::IsAllowedGameCommand({"unknown", "operation"}, error));
    EXPECT_FALSE(error.empty());
}

TEST(McpOperations, FindsRegisteredOperationByStableName) {
    const auto* operation = igi1conv::FindGameOperation("qsc.edit-object");
    ASSERT_NE(operation, nullptr);
    EXPECT_EQ(operation->commandPrefix, (std::vector<std::string>{"qsc", "edit-object"}));
    EXPECT_TRUE(operation->writesGame);
    EXPECT_EQ(igi1conv::FindGameOperation("graph.md")->commandPrefix,
              (std::vector<std::string>{"graph", "md"}));
    EXPECT_EQ(igi1conv::FindGameOperation("iff.export-gif")->commandPrefix,
              (std::vector<std::string>{"iff", "export-gif"}));
    EXPECT_EQ(igi1conv::FindGameOperation("mtp.sync")->commandPrefix,
              (std::vector<std::string>{"mtp", "sync"}));
    EXPECT_EQ(igi1conv::FindGameOperation("settings.theme"), nullptr);
}
