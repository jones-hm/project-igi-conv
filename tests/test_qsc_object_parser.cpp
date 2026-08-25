// test_qsc_object_parser.cpp - unit tests for the HumanSoldier Task_New
// parser.  Uses literal QSC text snippets so the suite does not need
// the IGI1 game corpus to run.

#include "igi1conv_test_util.h"
// Pull the parser in by translation unit so the test binary doesn't
// need to link the full parser source list (which would drag in
// mef_native/glm/etc. for no reason).
#include "../source/parsers/qsc_object_parser.h"
#include "../source/parsers/qsc_object_parser.cpp"

#include <gtest/gtest.h>
#include <string>

using igi1conv::HumanSoldierEntry;
using igi1conv::LightmapBindingSet;
using igi1conv::QscObjectSet;

// Real example straight from a decompiled objects.qsc.  The
// index-by-index mapping is:
//
//   [0]  Task ID      = 2001
//   [1]  Class        = "HumanSoldier"
//   [2]  Object name  = ""                    (left empty in this level)
//   [3]  X            = 126893824.0
//   [4]  Y            = -38029724.0
//   [5]  Z            = 175332848.0
//   [6]  Gamma        = 3.1400001049041748
//   [7]  Model        = "013_01_1"
//   [8]  Team         = 1
//   [9]  BoneHierachy = 6
//   [10] StandAnim    = 5
TEST(QscObjectParser, SingleHumanSoldier) {
    const std::string qsc = R"QSC(
Task_New(2001, "HumanSoldier", "", 126893824.0, -38029724.0, 175332848.0, 3.1400001049041748, "013_01_1", 1, 6, 5);
)QSC";
    std::string err;
    QscObjectSet set = QscObjectSet::parse(qsc, &err);
    EXPECT_TRUE(err.empty()) << err;
    ASSERT_EQ(set.entries.size(), 1u);
    const auto& e = set.entries[0];
    EXPECT_EQ(e.modelId, "013_01_1");
    EXPECT_EQ(e.boneHierarchy, 6);
    EXPECT_EQ(e.standAnimation, 5);
    EXPECT_NEAR(e.posX, 126893824.0, 1e-3);
    EXPECT_NEAR(e.gamma, 3.14, 1e-3);
}

TEST(QscObjectParser, MultipleHumanSoldiersAndGrouping) {
    const std::string qsc = R"QSC(
Task_New(407, "HumanSoldier", "Gunner 407", 24474926.0, 87795696.0, 174680656.0, 1.4660760164260864, "011_03_1", 1, 1, -1);
Task_New(2001, "HumanSoldier", "", 126893824.0, -38029724.0, 175332848.0, 3.1400001049041748, "013_01_1", 1, 6, 5);
Task_New(2002, "HumanSoldier", "Sniper", 0.0, 0.0, 0.0, 0.0, "013_01_1", 2, 6, 7);
)QSC";
    std::string err;
    QscObjectSet set = QscObjectSet::parse(qsc, &err);
    EXPECT_TRUE(err.empty()) << err;
    ASSERT_EQ(set.entries.size(), 3u);

    auto ids = set.modelIds();
    ASSERT_EQ(ids.size(), 2u);
    EXPECT_EQ(ids[0], "011_03_1");
    EXPECT_EQ(ids[1], "013_01_1");

    auto anims013 = set.animationsForModel("013_01_1");
    ASSERT_EQ(anims013.size(), 2u);
    EXPECT_EQ(anims013[0].boneHierarchy, 6);
    EXPECT_EQ(anims013[0].standAnimation, 5);
    EXPECT_EQ(anims013[1].standAnimation, 7);
}

TEST(QscObjectParser, IgnoresNonHumanSoldierCalls) {
    const std::string qsc = R"QSC(
Task_New(100, "Door", "", 0.0, 0.0, 0.0, 0.0, "door_01", 0, 0, 0);
Task_New(101, "Light", "lamp", 0.0, 0.0, 0.0, 0.0, "light_01", 0, 0, 0);
Task_New(407, "HumanSoldier", "G", 0.0, 0.0, 0.0, 0.0, "model_a", 1, 2, 3);
)QSC";
    std::string err;
    QscObjectSet set = QscObjectSet::parse(qsc, &err);
    EXPECT_TRUE(err.empty()) << err;
    EXPECT_EQ(set.entries.size(), 1u);
    EXPECT_EQ(set.entries[0].modelId, "model_a");
}

TEST(QscObjectParser, NewlineSpreadArguments) {
    // Real IGI1 QSC files often have Task_New calls split across
    // multiple lines, with comments between the args.  The parser
    // must accept this layout.
    const std::string qsc = R"QSC(
Task_New(
  407,
  "HumanSoldier",
  "Gunner 407",
  24474926.0,
  87795696.0,
  174680656.0,
  1.4660760164260864,
  "011_03_1",
  1,
  1,
  -1
);
)QSC";
    std::string err;
    QscObjectSet set = QscObjectSet::parse(qsc, &err);
    EXPECT_TRUE(err.empty()) << err;
    ASSERT_EQ(set.entries.size(), 1u);
    EXPECT_EQ(set.entries[0].modelId, "011_03_1");
    EXPECT_EQ(set.entries[0].boneHierarchy, 1);
    EXPECT_EQ(set.entries[0].standAnimation, -1);  // -1 means "no anim"
}

TEST(QscObjectParser, AcceptsBlockCommentsBetweenTaskNameAndOpeningParen) {
    const std::string qsc = R"QSC(
Task_New /* call annotation */ (701, "HumanSoldier", "Commented", 10.0, 20.0, 30.0, 0.5, "013_01_1", 3, 4, 5);
)QSC";
    std::string err;
    QscObjectSet set = QscObjectSet::parse(qsc, &err);
    EXPECT_TRUE(err.empty()) << err;
    ASSERT_EQ(set.entries.size(), 1u);
    EXPECT_EQ(set.entries[0].name, "Commented");
    EXPECT_EQ(set.entries[0].modelId, "013_01_1");
}

TEST(QscObjectParser, NestedHumanSoldierInsideContainer) {
    // Real IGI1 QSC has the layout:
    //   Task_New(<container-id>, "<ContainerClass>", ...,
    //            Task_New(1505, "HumanSoldier", <11 args>,
    //                     Task_New(1506, "HumanAI", <args>)))
    // The HumanSoldier call is nested inside a parent Task_New.  A
    // naive parser that consumes the entire parent arg list and
    // advances past its closing ')' will miss the HumanSoldier.
    const std::string qsc = R"QSC(
Task_New(1, "AIGraph", "", 52730.5, 197655.7, 61505.2, FALSE, 1, 100, 0, 1, 2.0, 3.0, 0.3, 1, FALSE, 0.05,
    Task_New(1504, "PatrolPath", "",
        Task_New(-1, "PatrolPathCommand", "End script", 6, -1),
        Task_New(1505, "HumanSoldier", "", 22683652.0, -60990340.0, 174936032.0, 0, "000_01_1", 1, 0, -1,
            Task_New(1506, "HumanAI", "", "AITYPE_PATROL_AK", 1))));
)QSC";
    std::string err;
    QscObjectSet set = QscObjectSet::parse(qsc, &err);
    EXPECT_TRUE(err.empty()) << err;
    ASSERT_EQ(set.entries.size(), 1u) << "nested HumanSoldier was missed";
    EXPECT_EQ(set.entries[0].modelId, "000_01_1");
    EXPECT_EQ(set.entries[0].boneHierarchy, 0);
    EXPECT_EQ(set.entries[0].standAnimation, -1);
}

TEST(QscObjectParser, BlockCommentsCannotCreateOrCorruptObjects) {
    const std::string qsc = R"QSC(
/* Task_New(999, "HumanSoldier", "fake", 1, 2, 3, 0, "fake", 1, 2, 3) */
Task_New(407, /* class comment */ "HumanSoldier", "real /* name text */",
         1.0 /* x */, 2.0, 3.0, 0.0, "model_real", 1, 2, 3);
)QSC";
    std::string err;
    QscObjectSet set = QscObjectSet::parse(qsc, &err);
    EXPECT_TRUE(err.empty()) << err;
    ASSERT_EQ(set.entries.size(), 1u);
    EXPECT_EQ(set.entries[0].name, "real /* name text */");
    EXPECT_EQ(set.entries[0].modelId, "model_real");
}

TEST(QscObjectParser, LightmapBindingsIgnoreBlockComments) {
    const std::string qsc = R"QSC(
/* Task_New(1, "Building", "fake", 0, 0, 0, "fake") */
Task_New(1104, "Building", "Tower", 1 /* x */, 2, 3,
         Task_New(-1, "LightmapInfo", "obj00042"), "tower_model");
)QSC";
    std::string err;
    LightmapBindingSet set = LightmapBindingSet::parse(qsc, &err);
    EXPECT_TRUE(err.empty()) << err;
    const auto* binding = set.bindingForModelAndTaskId("tower_model", 1104);
    ASSERT_NE(binding, nullptr);
    EXPECT_EQ(binding->logicalId, "obj00042");
    EXPECT_TRUE(binding->hasPos);
    EXPECT_DOUBLE_EQ(binding->posX, 1.0);
    EXPECT_FALSE(set.logicalIdForModel("fake").has_value());
}
