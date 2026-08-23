// test_olm_writer.cpp — CLI tests for the OLM writer (olm from-png) and the
// lightmap recalc command. Spawns the freshly built igi1conv.exe against the
// real game corpus (set IGI_GAME_PATH / --game-path); skips cleanly when no
// corpus is available.
#include "igi1conv_test_util.h"
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <vector>
#include <cstdint>
#include <utility>
#include <cstring>

using namespace igi1conv_test;
namespace fs = std::filesystem;

namespace {

// Read a whole file into a byte vector (empty on failure).
std::vector<uint8_t> ReadAll(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// Corpus-backed recalc tests temporarily rewrite real .olm files. Keep the
// restoration independent of ASSERT_* control flow so an early assertion
// failure cannot leave the game corpus modified.
struct RestoreFiles {
    std::vector<std::pair<std::string, std::vector<uint8_t>>> files;

    ~RestoreFiles() {
        for (const auto& [path, bytes] : files) {
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            if (!output) continue;
            if (!bytes.empty()) {
                output.write(reinterpret_cast<const char*>(bytes.data()),
                             static_cast<std::streamsize>(bytes.size()));
            }
        }
    }
};

// Pull the "resolution: WxH" line out of `olm info` output.
bool OlmResolution(const std::string& olmPath, int& w, int& h) {
    std::string out;
    if (RunIGI1Conv("olm info " + Q(olmPath), &out) != 0) return false;
    auto pos = out.find("resolution:");
    if (pos == std::string::npos) return false;
    return std::sscanf(out.c_str() + pos, "resolution: %dx%d", &w, &h) == 2;
}

} // namespace

// from-png with a --template reproduces the original .olm pixel data exactly:
// olm -> png -> olm should round-trip the RGBA payload (R/B swap cancels out)
// and preserve the layer pixel dimensions from the template.
TEST(OlmWriter, RoundTripThroughPngPreservesPixels) {
    IGI1CONV_NEED(olm, "\\.olm$");
    TempDir tmp;
    std::string png = tmp / "rt.png";
    std::string rebuilt = tmp / "rt.olm";

    ASSERT_EQ(RunIGI1Conv("olm to-png " + Q(olm) + " -o " + Q(png)), 0);
    ASSERT_TRUE(NonEmptyFile(png));

    ASSERT_EQ(RunIGI1Conv("olm from-png " + Q(png) + " -o " + Q(rebuilt) + " --template " + Q(olm)), 0);
    ASSERT_TRUE(NonEmptyFile(rebuilt));

    // Dimensions must match the source.
    int ow = 0, oh = 0, rw = 0, rh = 0;
    ASSERT_TRUE(OlmResolution(olm, ow, oh));
    ASSERT_TRUE(OlmResolution(rebuilt, rw, rh));
    EXPECT_EQ(ow, rw);
    EXPECT_EQ(oh, rh);

    // Re-export both to PNG and compare those bytes: the pixel payload must be
    // byte-identical after a full olm->png->olm->png cycle.
    std::string png2 = tmp / "rt2.png";
    ASSERT_EQ(RunIGI1Conv("olm to-png " + Q(rebuilt) + " -o " + Q(png2)), 0);
    EXPECT_EQ(ReadAll(png), ReadAll(png2)) << "pixel data changed across olm->png->olm round-trip";
}

// from-png without a template still produces a parseable .olm (default header).
TEST(OlmWriter, FromPngNoTemplateIsValid) {
    IGI1CONV_NEED(olm, "\\.olm$");
    TempDir tmp;
    std::string png = tmp / "src.png";
    std::string out = tmp / "out.olm";

    ASSERT_EQ(RunIGI1Conv("olm to-png " + Q(olm) + " -o " + Q(png)), 0);
    ASSERT_EQ(RunIGI1Conv("olm from-png " + Q(png) + " -o " + Q(out)), 0);
    // The rebuilt file must parse and report the PNG's dimensions.
    int w = 0, h = 0;
    EXPECT_TRUE(OlmResolution(out, w, h));
    EXPECT_GT(w, 0);
    EXPECT_GT(h, 0);
}

TEST(OlmWriter, FromPngMissingOutputFails) {
    IGI1CONV_NEED(olm, "\\.olm$");
    TempDir tmp;
    std::string png = tmp / "src.png";
    ASSERT_EQ(RunIGI1Conv("olm to-png " + Q(olm) + " -o " + Q(png)), 0);
    // Missing -o is a usage error (exit 1).
    EXPECT_EQ(RunIGI1Conv("olm from-png " + Q(png)), 1);
}

TEST(OlmWriter, RejectsTruncatedOversizedPixelPayload) {
    TempDir tmp;
    std::string malformed = tmp / "truncated.olm";

    // The packed OLM header is 88 bytes and the layer descriptor is 16 bytes;
    // write only those structures while advertising a 65535x65535 payload.
    std::vector<uint8_t> bytes(88 + 16, 0);
    const float version = 0.12f;
    const uint16_t dimension = 65535;
    std::memcpy(bytes.data(), &version, sizeof(version));
    std::memcpy(bytes.data() + 88 + 12, &dimension, sizeof(dimension));
    std::memcpy(bytes.data() + 88 + 14, &dimension, sizeof(dimension));
    std::ofstream output(malformed, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(output.is_open());
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    output.close();

    std::string outputText;
    EXPECT_EQ(RunIGI1Conv("olm info " + Q(malformed), &outputText), 3);
    EXPECT_NE(outputText.find("pixel"), std::string::npos);
}

// ─── res repack (name-preserving write-back) ────────────────────────────────

// repack with an unpacked dir that contains every original entry (matched by
// basename) and no edits must reproduce the source .res byte-for-byte: same
// entry names, same bytes, same order. This is the invariant the editor's
// lightmap write-back relies on (only the recalc-modified .olm bytes change;
// every other entry and every entry NAME is preserved verbatim).
TEST(ResRepack, IdentityRoundTripIsByteIdentical) {
    IGI1CONV_NEED(res, "lightmaps\\.res$");
    TempDir tmp;
    std::string unpackDir = tmp / "unpack";
    std::string repacked  = tmp / "repacked.res";

    ASSERT_EQ(RunIGI1Conv("res unpack " + Q(res) + " " + Q(unpackDir)), 0);
    ASSERT_EQ(RunIGI1Conv("res repack " + Q(res) + " " + Q(unpackDir) + " -o " + Q(repacked)), 0);
    ASSERT_TRUE(NonEmptyFile(repacked));
    EXPECT_EQ(GetFileSHA256(res), GetFileSHA256(repacked))
        << "identity repack should reproduce the source .res byte-for-byte";
}

TEST(ResRepack, RejectsUnmatchedDirectoryFiles) {
    IGI1CONV_NEED(res, "lightmaps\\.res$");
    TempDir tmp;
    std::string inputDir = tmp / "unpack";
    std::string output = tmp / "rejected.res";
    fs::create_directories(inputDir);
    std::string extra = inputDir + "\\mcp-unmatched-file.bin";
    std::ofstream(extra, std::ios::binary).put('x');

    EXPECT_EQ(RunIGI1Conv("res repack " + Q(res) + " " + Q(inputDir) + " -o " + Q(output)), 3);
    EXPECT_FALSE(fs::exists(output));
}

// ─── lightmap recalc ────────────────────────────────────────────────────────

// Discover a level dir under the corpus that has BOTH a decompiled objects.qsc
// and a lightmaps_unpacked/ folder (the same setup the resolver needs). Returns
// the objects.qsc path, or "" if none is present.
static std::string FindLevelQscWithLightmaps() {
    if (!fs::exists(CorpusDir())) return "";
    for (const auto& entry : fs::recursive_directory_iterator(CorpusDir())) {
        if (!entry.is_directory() || entry.path().filename() != "lightmaps_unpacked") continue;
        fs::path levelDir = entry.path().parent_path().parent_path();
        fs::path qsc = levelDir / "objects.qsc";
        if (fs::exists(qsc)) return qsc.string();
    }
    return "";
}

// recalc with rot-orig == rot-new must be a no-op (factor 1.0 everywhere):
// the .olm bytes must be unchanged. This is the strongest invariant we can
// assert without a reference renderer — identity rotation can't change lighting.
TEST(LightmapRecalc, IdentityRotationLeavesOlmUnchanged) {
    std::string qsc = FindLevelQscWithLightmaps();
    if (qsc.empty())
        GTEST_SKIP() << "no corpus level dir has both objects.qsc and lightmaps_unpacked/ "
                        "(set IGI_GAME_PATH; decompile objects.qvm -> objects.qsc first)";

    // Find a type-3 model that has a lightmap binding in this qsc. Use `lightmap
    // list` output to pick a model+task with resolvable .olm files.
    // Simplest: scan the qsc text for a Building model id, then try recalc.
    std::ifstream f(qsc);
    std::string qscText((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    // Pull the first logical id "objNNNNN" to find its model via `lightmap list`
    // is indirect; instead drive recalc for a known-resolvable model by trying
    // each model id that appears next to a LightmapInfo. We grab a model id from
    // the corpus MEFs of type 3 and check it resolves.
    std::string mef = FindCorpusMefOfModelType(3);
    if (mef.empty()) GTEST_SKIP() << "no type-3 (lightmap) MEF in corpus";
    std::string modelId = fs::path(mef).stem().string();

    // Resolve to confirm this model has a binding + files; capture its task id.
    std::string listOut;
    if (RunIGI1Conv("lightmap list --model " + Q(modelId) + " --qsc " + Q(qsc), &listOut) != 0)
        GTEST_SKIP() << "model " << modelId << " has no lightmap binding in this level";
    // Parse the first "task <id>" from the list output.
    int taskId = -1;
    auto tpos = listOut.find("task ");
    if (tpos == std::string::npos || std::sscanf(listOut.c_str() + tpos, "task %d", &taskId) != 1)
        GTEST_SKIP() << "could not parse a task id for " << modelId;

    // Snapshot the resolved .olm files' bytes before recalc.
    std::string resolveOut;
    ASSERT_EQ(RunIGI1Conv("lightmap resolve --model " + Q(modelId) + " --qsc " + Q(qsc) +
                          " --task-id " + std::to_string(taskId), &resolveOut), 0)
        << resolveOut;
    std::vector<std::string> olmPaths;
    {
        std::istringstream iss(resolveOut);
        std::string line;
        bool inList = false;
        while (std::getline(iss, line)) {
            if (line.find(".olm file(s):") != std::string::npos) { inList = true; continue; }
            if (!inList) continue;
            size_t s = line.find_first_not_of(" \t");
            if (s == std::string::npos) continue;
            std::string p = line.substr(s);
            if (!p.empty() && p.back() == '\r') p.pop_back();
            if (fs::exists(p)) olmPaths.push_back(p);
        }
    }
    ASSERT_FALSE(olmPaths.empty());
    RestoreFiles restore;
    for (const auto& path : olmPaths) {
        restore.files.emplace_back(path, ReadAll(path));
    }
    TempDir tmp;
    // Snapshot each resolved .olm's PIXEL payload (via PNG export) before recalc.
    // Comparing PNGs isolates pixel equality from any header/trailing-byte
    // differences a rewrite might introduce.
    std::vector<std::vector<uint8_t>> beforePng;
    for (size_t i = 0; i < olmPaths.size(); ++i) {
        std::string png = tmp / ("before_" + std::to_string(i) + ".png");
        ASSERT_EQ(RunIGI1Conv("olm to-png " + Q(olmPaths[i]) + " -o " + Q(png)), 0);
        beforePng.push_back(ReadAll(png));
    }

    // Identity recalc: rot-orig == rot-new, so every factor is exactly 1.0.
    int rc = RunIGI1Conv("lightmap recalc --model " + Q(modelId) + " --qsc " + Q(qsc) +
                         " --task-id " + std::to_string(taskId) + " --mef " + Q(mef) +
                         " --rot-orig 0,0,0 --rot-new 0,0,0 --sun-dir 0,0,1"
                         " --sun-color 1,1,1 --ambient 0.3,0.3,0.3");
    ASSERT_EQ(rc, 0);

    // Every resolved file's pixels must be unchanged after an identity recalc.
    for (size_t i = 0; i < olmPaths.size(); ++i) {
        std::string png = tmp / ("after_" + std::to_string(i) + ".png");
        ASSERT_EQ(RunIGI1Conv("olm to-png " + Q(olmPaths[i]) + " -o " + Q(png)), 0);
        EXPECT_EQ(beforePng[i], ReadAll(png)) << "identity recalc changed pixels of " << olmPaths[i];
    }
}
