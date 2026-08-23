#include <gtest/gtest.h>

#include "igi1conv_test_util.h"
#include "qsc_object_editor.h"

#include <string>
#include <vector>

using namespace igi1conv_test;

namespace {

const char* kObjects =
    "// game placements\n"
    "Task_New(401, \"HumanSoldier\", \"Alpha\", 10, 20, 30, 0.5, \"000_01_1\", 1, 2, 3);\n"
    "Task_New(402, \"HumanSoldier\", \"Bravo\", -10, -20, -30, -0.5, \"000_02_1\", 2, 4, 5);\n"
    "Task_New(500, \"Container\", \"Weapons\",\n"
    "  Task_New(501, \"Weapon\", \"Rifle\", 1, 2, 3, 0, \"rifle_a\", 7));\n";

} // namespace

TEST(QscObjectEditor, ListsNestedGameTasksAndTheirDirectArguments) {
    std::vector<igi1conv::QscTaskSummary> tasks;
    std::string error;

    ASSERT_TRUE(igi1conv::ListQscTasks(kObjects, tasks, error)) << error;
    ASSERT_EQ(tasks.size(), 4u);
    EXPECT_EQ(tasks[0].taskId, 401);
    EXPECT_EQ(tasks[0].className, "HumanSoldier");
    EXPECT_EQ(tasks[0].objectName, "Alpha");
    EXPECT_EQ(tasks[0].directArguments[3], "10");
    EXPECT_EQ(tasks[2].className, "Container");
    EXPECT_EQ(tasks[3].taskId, 501);
    EXPECT_EQ(tasks[3].className, "Weapon");
    EXPECT_EQ(tasks[3].objectName, "Rifle");
}

TEST(QscObjectEditor, UpdatesOnlyTheUniquelySelectedPlacement) {
    igi1conv::QscTaskSelector selector;
    selector.taskId = 401;
    std::vector<igi1conv::QscFieldUpdate> updates = {
        {3, "100.25"},
        {4, "200.5"},
        {5, "-300"},
        {6, "1.25"},
        {7, "\"013_01_1\""},
        {8, "3"},
    };
    std::string output;

    const auto result = igi1conv::EditQscTasks(kObjects, selector, updates, output);

    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(result.matchedCalls, 1u);
    EXPECT_EQ(result.changedFields, 6u);
    EXPECT_NE(output.find("401, \"HumanSoldier\", \"Alpha\", 100.25, 200.5, -300, 1.25, \"013_01_1\", 3, 2, 3"), std::string::npos);
    EXPECT_NE(output.find("402, \"HumanSoldier\", \"Bravo\", -10, -20, -30, -0.5, \"000_02_1\", 2, 4, 5"), std::string::npos);
    EXPECT_NE(output.find("// game placements\n"), std::string::npos);
}

TEST(QscObjectEditor, SupportsGenericIndexedParametersForOtherGameTaskClasses) {
    igi1conv::QscTaskSelector selector;
    selector.className = "Weapon";
    selector.objectName = "Rifle";
    std::string output;

    const auto result = igi1conv::EditQscTasks(
        kObjects, selector, {{7, "\"rocket_launcher\""}, {8, "12"}}, output);

    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_NE(output.find("501, \"Weapon\", \"Rifle\", 1, 2, 3, 0, \"rocket_launcher\", 12"), std::string::npos);
    EXPECT_NE(output.find("401, \"HumanSoldier\", \"Alpha\", 10, 20, 30, 0.5, \"000_01_1\", 1, 2, 3"), std::string::npos);
}

TEST(QscObjectEditor, RejectsAmbiguousOrMalformedWritesWithoutChangingOutput) {
    igi1conv::QscTaskSelector ambiguous;
    ambiguous.className = "HumanSoldier";
    std::string output = "sentinel";
    auto result = igi1conv::EditQscTasks(kObjects, ambiguous, {{3, "99"}}, output);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.matchedCalls, 2u);
    EXPECT_EQ(output, kObjects);

    igi1conv::QscTaskSelector unique;
    unique.taskId = 401;
    output = "sentinel";
    const std::string malformed = "Task_New(401, \"HumanSoldier\", \"broken\", 1, 2, 3;";
    result = igi1conv::EditQscTasks(malformed, unique, {{3, "99"}}, output);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(output, malformed);
    EXPECT_NE(result.error.find("parenthesis"), std::string::npos);
}

TEST(QscObjectEditor, RejectsUnsafeMultiTokenLiterals) {
    igi1conv::QscTaskSelector selector;
    selector.taskId = 401;
    std::string output;

    const auto result = igi1conv::EditQscTasks(kObjects, selector, {{3, "1, 2"}}, output);

    EXPECT_FALSE(result.ok);
    EXPECT_EQ(output, kObjects);
    EXPECT_NE(result.error.find("literal"), std::string::npos);
}

TEST(QscObjectEditor, RejectsNonFiniteNumericLiterals) {
    igi1conv::QscTaskSelector selector;
    selector.taskId = 401;
    std::string output;

    const auto result = igi1conv::EditQscTasks(kObjects, selector, {{3, "nan"}}, output);

    EXPECT_FALSE(result.ok);
    EXPECT_EQ(output, kObjects);
    EXPECT_NE(result.error.find("literal"), std::string::npos);
}

TEST(QscObjectEditor, RejectsQuotedLiteralsWithEscapedClosingQuote) {
    igi1conv::QscTaskSelector selector;
    selector.taskId = 401;
    std::string output;
    const std::string malformedLiteral = "\"unterminated\\\"";

    const auto result = igi1conv::EditQscTasks(
        kObjects, selector, {{3, malformedLiteral}}, output);

    EXPECT_FALSE(result.ok);
    EXPECT_EQ(output, kObjects);
    EXPECT_NE(result.error.find("literal"), std::string::npos);
}

TEST(QscObjectEditor, PreservesCommentsAfterEditedArgumentLiterals) {
    const std::string source =
        "Task_New(701, \"HumanSoldier\", \"Commented\", 10 /* keep x */, "
        "20 // keep y\n, 30, 0.5, \"model\", 1, 2, 3);\n";
    igi1conv::QscTaskSelector selector;
    selector.taskId = 701;
    std::string output;

    const auto result = igi1conv::EditQscTasks(source, selector,
                                                {{3, "100"}, {4, "200"}}, output);

    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_NE(output.find("100 /* keep x */"), std::string::npos);
    EXPECT_NE(output.find("200 // keep y"), std::string::npos);
}

TEST(QscObjectEditor, ExposesGamePlacementEditingThroughQscCli) {
    TempDir temp;
    const std::string input = temp / "objects.qsc";
    const std::string output = temp / "edited.qsc";
    {
        std::ofstream file(input, std::ios::binary);
        ASSERT_TRUE(file.is_open());
        file << kObjects;
    }

    std::string listing;
    ASSERT_EQ(RunIGI1Conv("qsc list-objects " + Q(input) + " --json", &listing), 0);
    EXPECT_NE(listing.find("\"task_id\":401"), std::string::npos);
    EXPECT_NE(listing.find("\"class\":\"Weapon\""), std::string::npos);

    std::string editOutput;
    ASSERT_EQ(RunIGI1Conv(
        "qsc edit-object " + Q(input) + " -o " + Q(output)
            + " --id 401 --position 100.25 200.5 -300 --model-id 013_01_1 --team 3",
        &editOutput), 0) << editOutput;

    std::ifstream edited(output, std::ios::binary);
    ASSERT_TRUE(edited.is_open());
    const std::string source((std::istreambuf_iterator<char>(edited)),
                             std::istreambuf_iterator<char>());
    EXPECT_NE(source.find("401, \"HumanSoldier\", \"Alpha\", 100.25, 200.5, -300"),
              std::string::npos);
    EXPECT_NE(source.find("\"013_01_1\", 3, 2, 3"), std::string::npos);
    EXPECT_NE(source.find("402, \"HumanSoldier\", \"Bravo\", -10, -20, -30"),
              std::string::npos);
}

TEST(QscObjectEditor, ExposesAnimationSelectionFieldsThroughQscCli) {
    TempDir temp;
    const std::string input = temp / "animated.qsc";
    const std::string output = temp / "animated-edited.qsc";
    {
        std::ofstream file(input, std::ios::binary);
        ASSERT_TRUE(file.is_open());
        file << "Task_New(601, \"HumanSoldier\", \"Animated\", 1, 2, 3, 0, "
                   "\"soldier\", 1, 2, 3);\n";
    }

    std::string commandOutput;
    ASSERT_EQ(RunIGI1Conv(
        "qsc edit-object " + Q(input) + " -o " + Q(output)
            + " --id 601 --bone-hierarchy 8 --stand-animation 9",
        &commandOutput), 0) << commandOutput;

    std::ifstream edited(output, std::ios::binary);
    ASSERT_TRUE(edited.is_open());
    const std::string source((std::istreambuf_iterator<char>(edited)),
                             std::istreambuf_iterator<char>());
    EXPECT_NE(source.find("\"soldier\", 1, 8, 9"), std::string::npos);
}

TEST(QscObjectEditor, RejectsInPlaceOutputWithoutChangingInput) {
    TempDir temp;
    const std::string input = temp / "objects.qsc";
    {
        std::ofstream file(input, std::ios::binary);
        ASSERT_TRUE(file.is_open());
        file << kObjects;
    }

    std::string commandOutput;
    EXPECT_NE(RunIGI1Conv(
        "qsc edit-object " + Q(input) + " -o " + Q(input)
            + " --id 401 --set 3=99",
        &commandOutput), 0);

    std::ifstream unchanged(input, std::ios::binary);
    ASSERT_TRUE(unchanged.is_open());
    const std::string source((std::istreambuf_iterator<char>(unchanged)),
                             std::istreambuf_iterator<char>());
    EXPECT_EQ(source, kObjects);
}
