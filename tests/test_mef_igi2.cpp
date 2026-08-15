#include "../source/parsers/mef_native.h"
#include "../source/parsers/res_parser.h"
#include <cctype>
#include "igi1conv_test_util.h"
#include <gtest/gtest.h>
#include <cstdlib>
#include <filesystem>

using namespace igi1conv_test;

static std::string Igi2Root() {
    for (const char* key : {"IGI2_ROOT", "IGI2_DIR", "IGI2_GAME_PATH"}) {
        if (const char* v = std::getenv(key)) {
            if (v[0] && std::filesystem::is_directory(v)) {
                return std::string(v);
            }
        }
    }
    return {};
}

static std::string FindIgi2ModelsRes() {
    const std::string root = Igi2Root();
    if (root.empty()) return {};
    const std::filesystem::path preferred =
        std::filesystem::path(root) / "MISSIONS" / "location1" / "level1" / "models" / "level1.res";
    if (std::filesystem::is_regular_file(preferred)) {
        return preferred.string();
    }
    const std::filesystem::path missions = std::filesystem::path(root) / "MISSIONS";
    if (!std::filesystem::is_directory(missions)) return {};
    for (const auto& entry : std::filesystem::recursive_directory_iterator(missions)) {
        if (!entry.is_regular_file()) continue;
        const auto path = entry.path();
        if (path.extension() != ".res") continue;
        if (path.parent_path().filename() != "models") continue;
        std::string norm = path.generic_string();
        if (norm.find("/common/") != std::string::npos) continue;
        return path.string();
    }
    return {};
}

static std::string BasenameStem(std::string name) {
    auto slash = name.find_last_of("\\/:");
    if (slash != std::string::npos) name = name.substr(slash + 1);
    for (char& c : name) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    if (name.size() > 4 && name.substr(name.size() - 4) == ".mef") {
        name = name.substr(0, name.size() - 4);
    }
    return name;
}

static std::vector<uint8_t> ExtractNamedMef(const std::string& resPath, const std::string& stem) {
    RESFile archive = RES_Parse(resPath);
    if (!archive.valid) return {};
    std::string want = stem;
    for (char& c : want) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    for (const auto& e : archive.entries) {
        if (BasenameStem(e.name) == want) return e.data;
    }
    return {};
}

TEST(MefIgi2, Type3BuildingUses28ByteXtrvAndEcaf) {
    const std::string res = FindIgi2ModelsRes();
    if (res.empty()) GTEST_SKIP() << "set IGI2_ROOT to an IGI 2 install";

    auto bytes = ExtractNamedMef(res, "400_10_1");
    if (bytes.empty()) GTEST_SKIP() << "400_10_1.mef not in " << res;

    ParsedGeometry geo = ParseMefBytes(bytes, "400_10_1.mef");
    EXPECT_TRUE(geo.isIgi2);
    EXPECT_FALSE(geo.isIgi1);
    EXPECT_EQ(geo.modelType, 3u);
    EXPECT_EQ(geo.xtrvStride, 28u);
    EXPECT_FALSE(geo.vertices.empty());
    EXPECT_FALSE(geo.triangles.empty());
    EXPECT_NE(geo.renderLayout.find("igi2 ECAF"), std::string::npos)
        << geo.renderLayout;
    EXPECT_TRUE(geo.fromRenderMesh);
}

TEST(MefIgi2, Type0UsesEcafNotCollisionFallback) {
    const std::string res = FindIgi2ModelsRes();
    if (res.empty()) GTEST_SKIP() << "set IGI2_ROOT to an IGI 2 install";

    auto bytes = ExtractNamedMef(res, "940_03_1");
    if (bytes.empty()) GTEST_SKIP() << "940_03_1.mef not in " << res;

    ParsedGeometry geo = ParseMefBytes(bytes, "940_03_1.mef");
    EXPECT_TRUE(geo.isIgi2);
    EXPECT_EQ(geo.modelType, 0u);
    EXPECT_FALSE(geo.triangles.empty());
    EXPECT_NE(geo.renderLayout.find("igi2 ECAF"), std::string::npos)
        << geo.renderLayout;
    EXPECT_TRUE(geo.fromRenderMesh);
}

TEST(MefIgi2, Type1StillHasRenderTriangles) {
    const std::string res = FindIgi2ModelsRes();
    if (res.empty()) GTEST_SKIP() << "set IGI2_ROOT to an IGI 2 install";

    auto bytes = ExtractNamedMef(res, "good_sc_1");
    if (bytes.empty()) GTEST_SKIP() << "good_sc_1.mef not in " << res;

    ParsedGeometry geo = ParseMefBytes(bytes, "good_sc_1.mef");
    EXPECT_TRUE(geo.isIgi2);
    EXPECT_EQ(geo.modelType, 1u);
    EXPECT_FALSE(geo.vertices.empty());
    EXPECT_FALSE(geo.triangles.empty());
    EXPECT_TRUE(geo.fromRenderMesh);
}
