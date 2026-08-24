// test_igi1conv_roundtrip.cpp — conversion round-trips, version reporting, and
// error-handling behaviour.
#include "igi1conv_test_util.h"

using namespace igi1conv_test;

namespace {

void RemoveLastFunctionLine(const std::string& path, const std::string& prefix) {
    std::ifstream in(path);
    ASSERT_TRUE(in.is_open()) << "failed to open text MEF: " << path;

    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line))
        lines.push_back(line);
    in.close();

    auto it = std::find_if(lines.rbegin(), lines.rend(), [&prefix](const std::string& s) {
        return s.rfind(prefix, 0) == 0;
    });
    ASSERT_NE(it, lines.rend()) << "text MEF has no " << prefix << " lines: " << path;
    lines.erase(std::next(it).base());

    std::ofstream out(path, std::ios::trunc);
    ASSERT_TRUE(out.is_open()) << "failed to rewrite text MEF: " << path;
    for (const auto& s : lines)
        out << s << "\n";
}

} // namespace

// ─── round-trips ─────────────────────────────────────────────────────────────

// QVM -> QSC -> QVM: decompiled source must recompile cleanly.
TEST_F(IGI1ConvTest, RoundtripQvmQscQvm) {
    IGI1CONV_NEED(qvm, "\\.qvm$");
    TempDir tmp;
    std::string qsc  = tmp / "ammo.qsc";
    std::string qvm2 = tmp / "ammo_recompiled.qvm";

    ASSERT_EQ(RunIGI1Conv("qvm decompile " + Q(qvm) + " -o " + Q(qsc)), 0);
    ASSERT_TRUE(NonEmptyFile(qsc));
    EXPECT_EQ(RunIGI1Conv("qsc compile " + Q(qsc) + " -o " + Q(qvm2)), 0);
    EXPECT_TRUE(NonEmptyFile(qvm2));
}

// DAT -> MTP -> DAT: model/texture mappings survive both conversions.
TEST_F(IGI1ConvTest, RoundtripDatMtpDat) {
    IGI1CONV_NEED(dat, "^(?!.*graph).*\\.dat$");
    TempDir tmp;
    std::string mtp  = tmp / "common.mtp";
    std::string dat2 = tmp / "common_back.dat";

    ASSERT_EQ(RunIGI1Conv("dat to-mtp " + Q(dat) + " -o " + Q(mtp)), 0);
    ASSERT_TRUE(NonEmptyFile(mtp));
    EXPECT_EQ(RunIGI1Conv("mtp to-dat " + Q(mtp) + " -o " + Q(dat2)), 0);
    EXPECT_TRUE(NonEmptyFile(dat2));
}

// MTP -> DAT -> MTP: the inverse direction.
TEST_F(IGI1ConvTest, RoundtripMtpDatMtp) {
    IGI1CONV_NEED(mtp, "\\.mtp$");
    TempDir tmp;
    std::string dat  = tmp / "common.dat";
    std::string mtp2 = tmp / "common_back.mtp";

    ASSERT_EQ(RunIGI1Conv("mtp to-dat " + Q(mtp) + " -o " + Q(dat)), 0);
    ASSERT_TRUE(NonEmptyFile(dat));
    EXPECT_EQ(RunIGI1Conv("dat to-mtp " + Q(dat) + " -o " + Q(mtp2)), 0);
    EXPECT_TRUE(NonEmptyFile(mtp2));
}

// TEX -> TGA -> PNG: decode native texture, then re-encode via stb input path.
TEST_F(IGI1ConvTest, RoundtripTexTgaPng) {
    IGI1CONV_NEED(tex, "\\.tex$");
    TempDir tmp;
    std::string tga = tmp / "flare.tga";
    std::string png = tmp / "flare.png";

    ASSERT_EQ(RunIGI1Conv("tex to-tga " + Q(tex) + " -o " + Q(tga)), 0);
    ASSERT_TRUE(NonEmptyFile(tga));
    EXPECT_EQ(RunIGI1Conv("tex to-png " + Q(tga) + " -o " + Q(png)), 0);
    EXPECT_TRUE(NonEmptyFile(png));
}

// ─── MEF text/binary round-trips ─────────────────────────────────────────────

// Binary MEF -> Text MEF: to-text must produce a non-empty text file.
TEST_F(IGI1ConvTest, MefBinaryToText) {
    IGI1CONV_NEED(mef, "\\.mef$");
    TempDir tmp;
    std::string txt = tmp / "model.mef.txt";

    ASSERT_EQ(RunIGI1Conv("mef to-text " + Q(mef) + " -o " + Q(txt)), 0);
    ASSERT_TRUE(NonEmptyFile(txt));

    // Output must start with NewObject(
    std::ifstream f(txt);
    std::string firstLine;
    std::getline(f, firstLine);
    EXPECT_NE(firstLine.find("NewObject"), std::string::npos)
        << "Expected text MEF to start with NewObject, got: " << firstLine;
}

// Binary MEF -> Text MEF -> Binary MEF: compiled output must be parseable.
TEST_F(IGI1ConvTest, RoundtripMefBinaryTextBinary) {
    IGI1CONV_NEED(mef, "\\.mef$");
    TempDir tmp;
    std::string txt  = tmp / "model.mef.txt";
    std::string mef2 = tmp / "model_recompiled.mef";

    ASSERT_EQ(RunIGI1Conv("mef to-text " + Q(mef) + " -o " + Q(txt)), 0);
    ASSERT_TRUE(NonEmptyFile(txt));

    ASSERT_EQ(RunIGI1Conv("mef compile " + Q(txt) + " -o " + Q(mef2)), 0);
    ASSERT_TRUE(NonEmptyFile(mef2));

    // Compiled binary must have ILFF magic
    std::ifstream f(mef2, std::ios::binary);
    char magic[4] = {0};
    f.read(magic, 4);
    EXPECT_EQ(std::string(magic, 4), "ILFF")
        << "Compiled MEF must start with ILFF magic";

    // Compiled binary must be parseable (mef info returns 0)
    EXPECT_EQ(RunIGI1Conv("mef info " + Q(mef2)), 0);
}

// Sidecar compile preserves raw DNER/D3DR/PMTL topology. Vertex and UV edits are
// supported, but face topology edits must be rejected instead of producing a
// binary whose text-derived HSEM counts disagree with preserved DNER data.
TEST_F(IGI1ConvTest, MefSidecarCompileRejectsFaceTopologyEdits) {
    IGI1CONV_NEED(mef, "\\.mef$");
    TempDir tmp;
    std::string txt  = tmp / "model.mef.txt";
    std::string mef2 = tmp / "model_topology_changed.mef";

    ASSERT_EQ(RunIGI1Conv("mef to-text " + Q(mef) + " -o " + Q(txt)), 0);
    ASSERT_TRUE(NonEmptyFile(txt));
    std::string mex = txt.substr(0, txt.find_last_of(".")) + ".mex";
    ASSERT_TRUE(NonEmptyFile(mex));

    RemoveLastFunctionLine(txt, "Face(");

    std::string out;
    EXPECT_NE(RunIGI1Conv("mef compile " + Q(txt) + " -o " + Q(mef2), &out), 0)
        << "sidecar compile must reject Face() topology edits; output:\n" << out;
}

TEST_F(IGI1ConvTest, MefSidecarCompileRejectsVertexCountEdits) {
    IGI1CONV_NEED(mef, "\\.mef$");
    TempDir tmp;
    std::string txt  = tmp / "model.mef.txt";
    std::string mef2 = tmp / "model_vertex_count_changed.mef";

    ASSERT_EQ(RunIGI1Conv("mef to-text " + Q(mef) + " -o " + Q(txt)), 0);
    ASSERT_TRUE(NonEmptyFile(txt));
    std::string mex = txt.substr(0, txt.find_last_of(".")) + ".mex";
    ASSERT_TRUE(NonEmptyFile(mex));

    RemoveLastFunctionLine(txt, "Vertex(");

    std::string out;
    EXPECT_NE(RunIGI1Conv("mef compile " + Q(txt) + " -o " + Q(mef2), &out), 0)
        << "sidecar compile must reject Vertex() count edits; output:\n" << out;
}

// ─── version reporting (regression: used to print 3.0) ───────────────────────

TEST_F(IGI1ConvTest, VersionFlagReportsCurrentVersion) {
    std::string out;
    EXPECT_EQ(RunIGI1Conv("--version", &out), 0);
    EXPECT_NE(out.find("1.11.0"), std::string::npos) << "got: " << out;
}

TEST_F(IGI1ConvTest, HelpReportsCurrentVersion) {
    std::string out;
    EXPECT_EQ(RunIGI1Conv("--help", &out), 0);
    EXPECT_NE(out.find("v1.11.0"), std::string::npos) << "got: " << out;
}

// ─── error handling ──────────────────────────────────────────────────────────

TEST_F(IGI1ConvTest, MissingFileReturnsNonZero) {
    EXPECT_NE(RunIGI1Conv("tex info " + Q("C:\\nonexistent_file_xyz.TEX")), 0);
}
TEST_F(IGI1ConvTest, UnknownCommandReturnsNonZero) {
    EXPECT_NE(RunIGI1Conv("badcmd info somefile"), 0);
}
TEST_F(IGI1ConvTest, UnknownSubcommandReturnsNonZero) {
    EXPECT_NE(RunIGI1Conv("tex bogus somefile"), 0);
}
TEST_F(IGI1ConvTest, NoArgsReturnsNonZero) {
    // No arguments intentionally launches the GUI.  Bound this headless
    // probe so the test does not wait the helper's full process timeout for
    // an interactive window that cannot be exercised by the CLI suite.
    EXPECT_NE(RunIGI1Conv("", nullptr, 1000), 0);
}
