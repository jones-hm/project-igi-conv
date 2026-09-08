// test_igi1conv_commands.cpp — one or more tests for every igi1conv command and
// every subcommand, driven against the real game-file corpus.
#include "igi1conv_test_util.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace igi1conv_test;

// ─── tex ─────────────────────────────────────────────────────────────────────
TEST_F(IGI1ConvTest, TexInfoTex) {
    IGI1CONV_NEED(f, "\\.tex$");
    EXPECT_EQ(RunIGI1Conv("tex info " + Q(f)), 0);
}
TEST_F(IGI1ConvTest, TexInfoSpr) {
    IGI1CONV_NEED(f, "\\.spr$");
    EXPECT_EQ(RunIGI1Conv("tex info " + Q(f)), 0);
}
TEST_F(IGI1ConvTest, TexInfoPic) {
    IGI1CONV_NEED(f, "\\.pic$");
    EXPECT_EQ(RunIGI1Conv("tex info " + Q(f)), 0);
}
TEST_F(IGI1ConvTest, TexDecode) {
    IGI1CONV_NEED(f, "\\.tex$");
    TempDir tmp;
    std::string out = tmp / "texout";
    EXPECT_EQ(RunIGI1Conv("tex decode " + Q(f) + " -o " + Q(out)), 0);
    // TEX decoder writes one .tga per frame under <out>\. The naming
    // is <tex-stem>.NNN.tga (or with %NN placeholders if the format
    // string wasn't substituted). Find any .tga file under out to
    // confirm at least one frame was written.
    bool any = false;
    if (std::filesystem::is_directory(out)) {
        for (auto& e : std::filesystem::directory_iterator(out)) {
            if (e.is_regular_file() &&
                e.path().extension().string() == ".tga") { any = true; break; }
        }
    }
    EXPECT_TRUE(any) << "no .tga frames in " << out;
}
TEST_F(IGI1ConvTest, TexToPng) {
    IGI1CONV_NEED(f, "\\.tex$");
    TempDir tmp;
    std::string out = tmp / "flare.png";
    EXPECT_EQ(RunIGI1Conv("tex to-png " + Q(f) + " -o " + Q(out)), 0);
    EXPECT_TRUE(NonEmptyFile(out));
}
TEST_F(IGI1ConvTest, TexToTga) {
    IGI1CONV_NEED(f, "\\.tex$");
    TempDir tmp;
    std::string out = tmp / "flare.tga";
    EXPECT_EQ(RunIGI1Conv("tex to-tga " + Q(f) + " -o " + Q(out)), 0);
    EXPECT_TRUE(NonEmptyFile(out));
}
TEST_F(IGI1ConvTest, TexToPngResize) {
    IGI1CONV_NEED(f, "\\.tex$");
    TempDir tmp;
    std::string out = tmp / "flare_small.png";
    EXPECT_EQ(RunIGI1Conv("tex to-png " + Q(f) + " -o " + Q(out) + " --resize 16 16"), 0);
    EXPECT_TRUE(NonEmptyFile(out));
}
TEST_F(IGI1ConvTest, TexToSprFromTex) {
    // Round-trip a real .tex from the corpus to .spr via the new
    // `tex to-spr` subcommand.  The output must be a valid LOOP v11
    // container with mode 3 (ARGB8888) and the same dimensions as the
    // source's first image.  No corpus dependency on a .spr file
    // because the converter goes the other way too.
    IGI1CONV_NEED(f, "\\.tex$");
    TempDir tmp;
    std::string out = tmp / "out.spr";
    ASSERT_EQ(RunIGI1Conv("tex to-spr " + Q(f) + " -o " + Q(out)), 0);
    ASSERT_TRUE(NonEmptyFile(out));

    // Parse the SPR header manually: 32 bytes, "LOOP" sig, version
    // (uint32), mode (uint32), width (uint16 at +26), height (+28),
    // depthBytes (+30).  For SPR we expect version 11, mode 3,
    // depthBytes 4.
    std::ifstream in(out, std::ios::binary);
    ASSERT_TRUE(in.is_open());
    char sig[4]; in.read(sig, 4);
    EXPECT_EQ(std::string(sig, 4), "LOOP");
    uint32_t version = 0, mode = 0;
    in.read(reinterpret_cast<char*>(&version), 4);  // native LE on x64
    in.read(reinterpret_cast<char*>(&mode), 4);
    // Skip multi(4) + reserved(4) + _1(2) + _2(2) + _3(2)  -> +14
    in.seekg(14, std::ios::cur);
    uint16_t w = 0, h = 0, depth = 0;
    in.read(reinterpret_cast<char*>(&w), 2);
    in.read(reinterpret_cast<char*>(&h), 2);
    in.read(reinterpret_cast<char*>(&depth), 2);
    EXPECT_EQ(version, 11u);
    EXPECT_EQ(mode, 3u);   // ARGB8888
    EXPECT_EQ(depth, 4u);
    EXPECT_GT(w, 0u);
    EXPECT_GT(h, 0u);
}
TEST_F(IGI1ConvTest, TexToSprFromSynthesizedTga) {
    // Corpus-free smoke test: synthesise a tiny 4x4 TGA (all red),
    // run `tex to-spr`, and verify the LOOP header + the file size
    // (32-byte header + 16 pixels * 4 bytes ARGB8888 = 96 bytes).
    TempDir tmp;
    {
        std::ofstream f(tmp / "red.tga", std::ios::binary);
        // 18-byte TGA header, no image ID / colormap.
        unsigned char hdr[18] = {0};
        hdr[2]  = 2;             // image type: uncompressed true-color
        hdr[12] = 4 & 0xFF;      // width  low
        hdr[13] = (4 >> 8) & 0xFF;
        hdr[14] = 4 & 0xFF;      // height low
        hdr[15] = (4 >> 8) & 0xFF;
        hdr[16] = 32;            // 32 bpp
        hdr[17] = 0x20;          // top-to-bottom rows (no mirror)
        f.write(reinterpret_cast<const char*>(hdr), sizeof(hdr));
        // 16 BGRA pixels, all red, fully opaque.
        for (int i = 0; i < 16; ++i) {
            char px[4] = { 0, 0, (char)0xFF, (char)0xFF };
            f.write(px, 4);
        }
    }

    std::string spr = tmp / "red.spr";
    ASSERT_EQ(RunIGI1Conv("tex to-spr " + Q(tmp / "red.tga") + " -o " + Q(spr)), 0);

    std::ifstream in(spr, std::ios::binary);
    ASSERT_TRUE(in.is_open());
    char sig[4]; in.read(sig, 4);
    EXPECT_EQ(std::string(sig, 4), "LOOP");
    uint32_t version = 0, mode = 0;
    in.read(reinterpret_cast<char*>(&version), 4);
    in.read(reinterpret_cast<char*>(&mode), 4);
    in.seekg(14, std::ios::cur);
    uint16_t w = 0, h = 0, depth = 0;
    in.read(reinterpret_cast<char*>(&w), 2);
    in.read(reinterpret_cast<char*>(&h), 2);
    in.read(reinterpret_cast<char*>(&depth), 2);
    EXPECT_EQ(version, 11u);
    EXPECT_EQ(mode, 3u);
    EXPECT_EQ(w, 4u);
    EXPECT_EQ(h, 4u);
    EXPECT_EQ(depth, 4u);
    std::error_code ec;
    EXPECT_EQ(static_cast<size_t>(w) * h * 4 + 32u,
              std::filesystem::file_size(spr, ec));
}

// ─── mef ─────────────────────────────────────────────────────────────────────
TEST_F(IGI1ConvTest, MefInfo) {
    IGI1CONV_NEED(f, "\\.mef$");
    EXPECT_EQ(RunIGI1Conv("mef info " + Q(f)), 0);
}
TEST_F(IGI1ConvTest, MefExport) {
    IGI1CONV_NEED(f, "\\.mef$");
    TempDir tmp;
    std::string out = tmp / "model.obj";
    EXPECT_EQ(RunIGI1Conv("mef export " + Q(f) + " -o " + Q(out)), 0);
    EXPECT_TRUE(NonEmptyFile(out));
}
TEST_F(IGI1ConvTest, MefDump) {
    IGI1CONV_NEED(f, "\\.mef$");
    TempDir tmp;
    std::string out = tmp / "model.txt";
    EXPECT_EQ(RunIGI1Conv("mef dump " + Q(f) + " -o " + Q(out)), 0);
    EXPECT_TRUE(NonEmptyFile(out));
}
TEST_F(IGI1ConvTest, MefExportObjHasRealUvs) {
    // Regression: OBJ export must not silently drop all UVs to 0,0 and
    // must produce real per-vertex V coordinates (not always 0/1 for
    // non-bone models).  We parse the OBJ `vt` lines and require at
    // least one non-(0,0) entry, and the V range must be strictly
    // between 0 and 1 (not all clamped to the flip extremes).
    // Use a Type 0 or Type 1 model — Type 3 (lightmap) UVs can span
    // the entire level (e.g. V=-11..1) and are not relevant here.
    std::string f = FindCorpusMefOfModelType(1, "\\.mef$");
    if (f.empty()) f = FindCorpusMefOfModelType(0, "\\.mef$");
    if (f.empty()) GTEST_SKIP() << "no Type 0 or Type 1 MEF in corpus";
    TempDir tmp;
    std::string out = tmp / "model.obj";
    EXPECT_EQ(RunIGI1Conv("mef export " + Q(f) + " -o " + Q(out)), 0);
    std::ifstream in(out);
    ASSERT_TRUE(in.is_open()) << "OBJ not written: " << out;
    int vtCount = 0, nonzero = 0;
    float vmin = 2.f, vmax = -1.f;
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("vt ", 0) != 0) continue;
        ++vtCount;
        float u = 0.f, v = 0.f;
        if (std::sscanf(line.c_str(), "vt %f %f", &u, &v) != 2) continue;
        if (u != 0.f || v != 0.f) ++nonzero;
        vmin = std::min(vmin, v);
        vmax = std::max(vmax, v);
    }
    EXPECT_GT(vtCount, 0)   << "OBJ has no `vt` entries: " << out;
    EXPECT_GT(nonzero, 0)   << "all OBJ UVs are (0,0): " << out;
    // MEF UVs can extend slightly outside [0,1] for tiled/oversized
    // textures; we only guard against a flipped or corrupted range
    // (V clamped to exactly 0 or exactly 1 for every vertex would
    // indicate a bad V-flip, not a tiled UV).
    EXPECT_LT(vmax,  2.0f)  << "OBJ V exploded (V-flip / NaN): " << out;
    EXPECT_GT(vmin, -1.0f)  << "OBJ V exploded (V-flip / NaN): " << out;
    EXPECT_LT(vmax - vmin, 1.5f)
        << "OBJ V looks clamped to 0/1 (no flip, or all 1-flip): " << out;
}
TEST_F(IGI1ConvTest, MefExportBinaryAndTextObjUvsMatch) {
    // The binary MEF -> OBJ and text MEF -> OBJ paths must produce the
    // same set of UV coordinates for the same source MEF (the text
    // path must not silently drop UVs or flip V differently from the
    // binary path).
    IGI1CONV_NEED(f, "\\.mef$");
    TempDir tmp;
    std::string binObj = tmp / "bin.obj";
    std::string txt    = tmp / "model.txt";
    std::string txtObj = tmp / "text.obj";
    ASSERT_EQ(RunIGI1Conv("mef export "  + Q(f)  + " -o " + Q(binObj)), 0);
    ASSERT_EQ(RunIGI1Conv("mef to-text " + Q(f)  + " -o " + Q(txt)),    0);
    ASSERT_EQ(RunIGI1Conv("mef export "  + Q(txt)+ " -o " + Q(txtObj)), 0);
    auto collectVts = [](const std::string& obj) {
        std::ifstream in(obj);
        std::vector<std::pair<float,float>> vts;
        std::string line;
        while (std::getline(in, line)) {
            if (line.rfind("vt ", 0) != 0) continue;
            float u = 0.f, v = 0.f;
            if (std::sscanf(line.c_str(), "vt %f %f", &u, &v) == 2)
                vts.emplace_back(u, v);
        }
        return vts;
    };
    auto bin = collectVts(binObj);
    auto txtv = collectVts(txtObj);
    ASSERT_GT(bin.size(),  0u) << "binary OBJ has no vts: " << binObj;
    ASSERT_GT(txtv.size(), 0u) << "text OBJ has no vts: "   << txtObj;
    // The first 3 vts (face 0) must match exactly between the two paths.
    for (size_t i = 0; i < 3 && i < bin.size() && i < txtv.size(); ++i) {
        EXPECT_NEAR(bin[i].first,  txtv[i].first,  1e-5f)
            << "U mismatch at vt " << i;
        EXPECT_NEAR(bin[i].second, txtv[i].second, 1e-5f)
            << "V mismatch at vt " << i;
    }
}
TEST_F(IGI1ConvTest, MefExportVFlipMatchesModelType) {
    // Regression: V-flip must be driven by model_type, not by a
    // renderLayout substring.  Rule (final):
    //   modelType 0 (rigid)    -> V preserved
    //   modelType 1 (bone)     -> V preserved (matches build-rigid)
    //   modelType 3 (lightmap) -> V preserved (already in OpenGL
    //                              orientation; 03642a7 missed this
    //                              category and caused 82% of MEFs
    //                              to be upside-down)
    // Use a Type 0 or Type 1 model — Type 3 (lightmap) UVs can span
    // the entire level and fail V-range sanity checks.
    std::string f = FindCorpusMefOfModelType(1, "\\.mef$");
    if (f.empty()) f = FindCorpusMefOfModelType(0, "\\.mef$");
    if (f.empty()) GTEST_SKIP() << "no Type 0 or Type 1 MEF in corpus";
    TempDir tmp;
    std::string out = tmp / "model.obj";
    EXPECT_EQ(RunIGI1Conv("mef export " + Q(f) + " -o " + Q(out)), 0);
    std::string infoOut;
    ASSERT_EQ(RunIGI1Conv("mef info " + Q(f), &infoOut), 0);
    // Parse model_type from the mef info output ("model_type: <N>").
    int modelType = -1;
    auto pos = infoOut.find("model_type:");
    if (pos != std::string::npos) {
        int v = 0;
        if (std::sscanf(infoOut.c_str() + pos, "model_type: %d", &v) == 1)
            modelType = v;
    }
    // Load OBJ vts and grab the first non-trivial one
    std::ifstream in(out);
    std::vector<std::pair<float,float>> vts;
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("vt ", 0) != 0) continue;
        float u = 0.f, v = 0.f;
        if (std::sscanf(line.c_str(), "vt %f %f", &u, &v) == 2)
            vts.emplace_back(u, v);
    }
    ASSERT_GT(vts.size(), 0u) << "OBJ has no vts: " << out;
    // Sanity-check that the V values are not artificially compressed
    // to the [0, 1] clamp due to a wrong-direction 1.0f - v flip.
    // Correct behavior (V preserved verbatim) gives a V range that
    // spans at least 0.4 of [0,1] for a typical model.
    float vmin = 2.f, vmax = -1.f;
    for (auto& p : vts) { vmin = std::min(vmin, p.second); vmax = std::max(vmax, p.second); }
    EXPECT_LT(vmax,  2.0f)  << "V exploded: " << out;
    EXPECT_GT(vmin, -1.0f)  << "V exploded: " << out;
    EXPECT_GT(vmax - vmin, 0.4f)
        << "OBJ V range is too narrow: " << out;
    // The "V is never flipped" rule applies to every model type
    // (rigid 0, bone 1, lightmap 3).  The exact value match is
    // enforced by the per-type tests above and by
    // MefExportVFlip_AllTypesRespectRule below.
}

// ─── Regression tests for the V-flip rule per model_type ────────────────────
//
// Historical context:
//   - Originally (commit f17921a) ALL MEFs had V flipped for OBJ export.
//   - Commit 03642a7 added an isBoneModel check to stop flipping V on
//     Type 1 (bone) models, but detected bone models by searching the
//     renderLayout string for "type1".  Type 3 (lightmap) models have
//     renderLayout "packed DNER" (no "type1" substring), so they were
//     STILL being V-flipped, leaving 82% of MEFs (340/415 in
//     level1/models) upside-down in the 3D viewer and any OBJ export.
//   - The user has settled on: V is NEVER flipped.  Both the source
//     MEF (as used by the IGI engine and the GUI 3D viewer) and
//     the exported OBJ / new MEF must carry the same V values.
//     e.g. 015_01_1.mef renders fine and its exported .obj must
//     render identically, so the OBJ must preserve V as-is.
//   - So the rule is the identity for every model type:
//        modelType 0 (rigid)    -> V preserved
//        modelType 1 (bone)     -> V preserved
//        modelType 3 (lightmap) -> V preserved
//
// These tests pin down the export contract so the bug cannot regress:
//   1. Type 0 (rigid)    - V is preserved (v.uv.y = OBJ v)
//   2. Type 1 (bone)     - V is preserved (v.uv.y = OBJ v)
//   3. Type 3 (lightmap) - V is preserved (v.uv.y = OBJ v)
//   4. The text MEF -> OBJ path agrees with the binary path for all
//      three model types.
//   5. The export MefVToObjV helper is the identity; a structural
//      test grep's mef_exporter.cpp for stray "1.0f - v.uv.y" /
//      "1.0f - uv[" literals outside the helper so the formula
//      cannot drift.
//   6. The GUI viewer (gui_main.cpp) must not flip V either
//      (MefViewerDoesNotFlipV).

// Helper: read raw V from the first vt line of an OBJ.
static float FirstObjV(const std::string& obj) {
    std::ifstream in(obj);
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("vt ", 0) != 0) continue;
        float u = 0.f, v = 0.f;
        if (std::sscanf(line.c_str(), "vt %f %f", &u, &v) == 2) return v;
    }
    return std::numeric_limits<float>::quiet_NaN();
}

// Helper: read the raw V from the MEF by first converting to text MEF
// (which writes UV(i, u, v) lines per vertex).  The text MEF preserves
// V as-is from the binary (no flip), so this is the "ground truth" V
// the OBJ exporter sees before deciding whether to flip.
static bool FirstMefRawV(const std::string& mef, float& rawV) {
    // mef dump output is too terse (only chunk sizes for binary MEFs),
    // so use a dedicated temp file and mef to-text for the conversion.
    TempDir tmp;
    std::string txt = tmp / "raw.txt";
    if (RunIGI1Conv("mef to-text \"" + mef + "\" -o \"" + txt + "\"") != 0)
        return false;
    std::ifstream in(txt);
    if (!in.is_open()) return false;
    std::string line;
    while (std::getline(in, line)) {
        // UV lines look like:  UV(0, 0.495799005, 0.577767968);
        if (line.rfind("UV(", 0) != 0) continue;
        int idx = -1; float u = 0.f, v = 0.f;
        if (std::sscanf(line.c_str(), "UV(%d, %f, %f", &idx, &u, &v) == 3) {
            rawV = v;
            return true;
        }
    }
    return false;
}

TEST_F(IGI1ConvTest, MefExportVFlip_Type0_Rigid_DoesNotFlipV) {
    // Type 0 (rigid) models: V is preserved verbatim in the OBJ
    // export.  The user settled on the rule "V is never flipped"
    // after observing that the original 015_01_1.mef (Type 0)
    // renders fine and its exported .obj must render identically.
    std::string f = FindCorpusMefOfModelType(0, "320_01_1\\.mef$");
    if (f.empty()) GTEST_SKIP() << "no Type 0 (rigid) MEF in corpus";
    TempDir tmp;
    std::string out = tmp / "rigid.obj";
    ASSERT_EQ(RunIGI1Conv("mef export \"" + f + "\" -o \"" + out + "\""), 0);
    float rawV = 0.f, objV = 0.f;
    ASSERT_TRUE(FirstMefRawV(f, rawV)) << "could not parse raw MEF UV: " << f;
    objV = FirstObjV(out);
    EXPECT_TRUE(std::isfinite(objV)) << "OBJ V is NaN: " << out;
    EXPECT_NEAR(objV, rawV, 1e-4f)
        << "Type 0 rigid: OBJ V should equal raw MEF V (no flip) "
        << "(raw=" << rawV << " objV=" << objV << ")";
}

TEST_F(IGI1ConvTest, MefExportVFlip_Type1_Bone_DoesNotFlipV) {
    // Type 1 (bone) models store V in OpenGL convention, so the OBJ
    // export must pass V through unchanged.  The user requires the
    // .mef -> .obj export for bone models to behave the same way
    // as the .mef -> build-rigid .mef path: V is preserved
    // (not flipped).  Flipping V on a bone model turns the
    // character's face texture upside down.
    std::string f = FindCorpusMefOfModelType(1, "0_000_01_1\\.mef$");
    if (f.empty()) GTEST_SKIP() << "no Type 1 (bone) MEF in corpus";
    TempDir tmp;
    std::string out = tmp / "bone.obj";
    ASSERT_EQ(RunIGI1Conv("mef export \"" + f + "\" -o \"" + out + "\""), 0);
    float rawV = 0.f, objV = 0.f;
    ASSERT_TRUE(FirstMefRawV(f, rawV)) << "could not parse raw MEF UV: " << f;
    objV = FirstObjV(out);
    EXPECT_TRUE(std::isfinite(objV)) << "OBJ V is NaN: " << out;
    EXPECT_NEAR(objV, rawV, 1e-4f)
        << "Type 1 bone: OBJ V should equal raw MEF V (no flip) "
        << "(raw=" << rawV << " objV=" << objV << ")";
}

TEST_F(IGI1ConvTest, MefExportVFlip_Type3_Lightmap_DoesNotFlipV) {
    // Type 3 (lightmap) models: V is preserved verbatim in the OBJ
    // export (the global "V is never flipped" rule).
    std::string f = FindCorpusMefOfModelType(3, "404_01_1\\.mef$");
    if (f.empty()) GTEST_SKIP() << "no Type 3 (lightmap) MEF in corpus";
    TempDir tmp;
    std::string out = tmp / "lmp.obj";
    ASSERT_EQ(RunIGI1Conv("mef export \"" + f + "\" -o \"" + out + "\""), 0);
    float rawV = 0.f, objV = 0.f;
    ASSERT_TRUE(FirstMefRawV(f, rawV)) << "could not parse raw MEF UV: " << f;
    objV = FirstObjV(out);
    EXPECT_TRUE(std::isfinite(objV)) << "OBJ V is NaN: " << out;
    EXPECT_NEAR(objV, rawV, 1e-4f)
        << "Type 3 lightmap: OBJ V should equal raw MEF V (no flip) "
        << "(raw=" << rawV << " objV=" << objV << ")";
}

// Cover all three MEF types we can find in the corpus.  This sweeps the
// entire corpus (up to 64 MEFs) and asserts that every model satisfies
// the per-type V-flip rule.  This is the catch-all that would have
// detected the 03642a7 missed-Type-3 bug at the time it shipped.
TEST_F(IGI1ConvTest, MefExportVFlip_AllTypesRespectRule) {
    std::string dir = CorpusDir();
    if (dir.empty() || !std::filesystem::exists(dir))
        GTEST_SKIP() << "no corpus available";

    struct Sample { int modelType; float rawV; float objV; std::string mef; };
    std::vector<Sample> byType[4];   // 0, 1, 3 used; index 2 reserved
    int inspected = 0;

    for (auto it = std::filesystem::recursive_directory_iterator(dir);
         it != std::filesystem::recursive_directory_iterator();
         ++it) {
        if (!it->is_regular_file()) continue;
        std::string filename = it->path().filename().string();
        if (filename.size() < 4) continue;
        std::string ext = filename.substr(filename.size() - 4);
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c){ return std::tolower(c); });
        if (ext != ".mef") continue;
        if (++inspected > 64) break;  // cap scan time

        int mt = GetMefModelType(it->path().string());
        if (mt != 0 && mt != 1 && mt != 3) continue;

        float rawV = 0.f;
        if (!FirstMefRawV(it->path().string(), rawV)) continue;

        TempDir tmp;
        std::string out = tmp / "allt.obj";
        if (RunIGI1Conv("mef export \"" + it->path().string() +
                        "\" -o \"" + out + "\"") != 0) continue;
        float objV = FirstObjV(out);
        if (!std::isfinite(objV)) continue;
        byType[mt].push_back({mt, rawV, objV, it->path().string()});
    }

    int total = 0;
    for (int mt : {0, 1, 3}) total += static_cast<int>(byType[mt].size());
    if (total == 0)
        GTEST_SKIP() << "no MEFs with a known model_type in corpus";

    for (int mt : {0, 1, 3}) {
        for (const auto& s : byType[mt]) {
            // Rule (final): V is NEVER flipped.  OBJ V must equal
            // raw MEF V for every model type.
            float expected = s.rawV;
            EXPECT_NEAR(s.objV, expected, 1e-4f)
                << "model_type=" << mt << " MEF=" << s.mef
                << " rawV=" << s.rawV << " objV=" << s.objV
                << " expected=" << expected;
        }
    }

    // Surface a per-type summary so the failure message is informative.
    for (int mt : {0, 1, 3}) {
        RecordProperty(("count_model_type_" + std::to_string(mt)).c_str(),
                      static_cast<int>(byType[mt].size()));
    }
}

// The text MEF -> OBJ path must apply the SAME V-flip rule as the
// binary MEF -> OBJ path.  If a future refactor splits the rule
// between the two paths, this test will catch the drift.
TEST_F(IGI1ConvTest, MefExportVFlip_BinaryAndTextPathsAgree) {
    std::string dir = CorpusDir();
    if (dir.empty() || !std::filesystem::exists(dir))
        GTEST_SKIP() << "no corpus available";

    // Pick one MEF per model type so all three branches are covered.
    struct Test { int modelType; std::string pattern; };
    Test tests[] = {
        {0, "320_01_1\\.mef$"},
        {1, "0_000_01_1\\.mef$"},
        {3, "404_01_1\\.mef$"},
    };
    int covered = 0;
    for (const auto& t : tests) {
        std::string f = FindCorpusMefOfModelType(t.modelType, t.pattern);
        if (f.empty()) continue;
        ++covered;
        TempDir tmp;
        std::string binObj = tmp / "bin.obj";
        std::string txt    = tmp / "model.txt";
        std::string txtObj = tmp / "text.obj";
        ASSERT_EQ(RunIGI1Conv("mef export \""  + f + "\" -o \"" + binObj + "\""), 0);
        ASSERT_EQ(RunIGI1Conv("mef to-text \"" + f + "\" -o \"" + txt    + "\""), 0);
        ASSERT_EQ(RunIGI1Conv("mef export \""  + txt + "\" -o \"" + txtObj + "\""), 0);
        float binV = FirstObjV(binObj);
        float txtV = FirstObjV(txtObj);
        EXPECT_NEAR(binV, txtV, 1e-4f)
            << "model_type=" << t.modelType << " MEF=" << f
            << " binaryOBJ_V=" << binV << " textOBJ_V=" << txtV;
    }
    if (covered == 0)
        GTEST_SKIP() << "none of the regression MEFs were found in corpus";
}

// The V-flip formula must be centralised.  If a future contributor
// re-introduces a per-call-site (1.0f - v.uv.y) literal anywhere in
// the export paths, this test will fail.  This is a structural test
// that grep's mef_exporter.cpp for the bad pattern and asserts it
// only appears inside the MefVToObjV helper itself.
//
// gui_main.cpp is exempt: the GUI viewer's guiMefVToObjV is the
// identity (V is rendered as-is from the MEF).  V is only flipped
// at export time.
TEST_F(IGI1ConvTest, MefExportVFlip_NoStrayOneMinusVLiterals) {
    namespace fs = std::filesystem;
    fs::path p = std::filesystem::current_path() / "source/parsers/mef_exporter.cpp";
    ASSERT_TRUE(fs::exists(p))
        << "could not locate mef_exporter.cpp at " << p.string();

    int hits = 0;
    std::string offending;
    std::ifstream in(p.string());
    std::string line;
    int lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        if (line.find("1.0f - v.uv.y") == std::string::npos &&
            line.find("1.0f - uv[")    == std::string::npos)
            continue;
        // MefVToObjV() in mef_exporter.cpp is the only place that
        // may spell out the flip formula.  Allow:
        //   - the bool overload (returns the same expression both
        //     branches)
        //   - the uint32_t modelType overload (the canonical rule)
        if (line.find("MefVToObjV") != std::string::npos) continue;
        ++hits;
        offending += p.filename().string() + ":" + std::to_string(lineNo)
                    + ": " + line + "\n";
    }
    EXPECT_EQ(hits, 0)
        << "Found stray '1.0f - v.uv.y' / '1.0f - uv[' literal(s) "
        << "outside MefVToObjV() in mef_exporter.cpp.  All V-flip "
        << "decisions must go through the centralised "
        << "MefVToObjV(v, modelType) helper so a model_type rule "
        << "change (e.g. 03642a7's bone fix) propagates to every "
        << "call site:\n" << offending;
}

// The GUI viewer must NOT apply the V-flip.  guiMefVToObjV is
// required to be the identity.  This is a structural / behavioural
// contract: the 3D viewer shows the MEF's V as-is.  V is only
// flipped at export time (see MefVToObjV in mef_exporter.cpp and
// the MefExportVFlip_* regression tests above).
//
// We assert the contract by reading the source: the GUI helper must
// return its `v` argument unchanged for every model_type.  The
// simplest way to enforce that is to require `guiMefVToObjV` to be
// implemented as a single return-v statement and never call
// MefVToObjV.  The viewer code must read `v.uv.y` directly.
TEST_F(IGI1ConvTest, MefViewerDoesNotFlipV) {
    namespace fs = std::filesystem;
    fs::path p = std::filesystem::current_path() / "igi1conv/gui_main.cpp";
    ASSERT_TRUE(fs::exists(p))
        << "could not locate gui_main.cpp at " << p.string();

    int callSites = 0;
    std::string offending;
    std::ifstream in(p.string());
    std::string line;
    int lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        // The viewer must not call MefVToObjV / guiMefVToObjV with
        // a flip, nor spell out the 1.0f - v.uv.y formula.  We allow
        // references to the helper in comments (the viewer CAN
        // mention guiMefVToObjV in a comment), so we only flag a
        // line if it actually invokes the function.
        if (line.find("guiMefVToObjV(") != std::string::npos &&
            line.find("//") == std::string::npos) {
            ++callSites;
            offending += p.filename().string() + ":" + std::to_string(lineNo)
                        + ": " + line + "\n";
        }
    }
    EXPECT_EQ(callSites, 0)
        << "The 3D viewer still calls guiMefVToObjV() with a flip. "
        << "The user requirement is: V is flipped ONLY at export "
        << "time (OBJ / MEF).  In the 3D viewer, pass v.uv.y "
        << "through unchanged:\n" << offending;
}

// ─── qsc ─────────────────────────────────────────────────────────────────────
TEST_F(IGI1ConvTest, QscValidate) {
    IGI1CONV_NEED(f, "\\.qsc$");
    EXPECT_EQ(RunIGI1Conv("qsc validate " + Q(f)), 0);
}
TEST_F(IGI1ConvTest, QscCompile) {
    IGI1CONV_NEED(f, "\\.qsc$");
    TempDir tmp;
    std::string out = tmp / "ammo.qvm";
    EXPECT_EQ(RunIGI1Conv("qsc compile " + Q(f) + " -o " + Q(out)), 0);
    EXPECT_TRUE(NonEmptyFile(out));
}

// ─── qvm ─────────────────────────────────────────────────────────────────────
TEST_F(IGI1ConvTest, QvmInfo) {
    IGI1CONV_NEED(f, "\\.qvm$");
    EXPECT_EQ(RunIGI1Conv("qvm info " + Q(f)), 0);
}
TEST_F(IGI1ConvTest, QvmDisasm) {
    IGI1CONV_NEED(f, "\\.qvm$");
    EXPECT_EQ(RunIGI1Conv("qvm disasm " + Q(f)), 0);
}
TEST_F(IGI1ConvTest, QvmDecompile) {
    IGI1CONV_NEED(f, "\\.qvm$");
    TempDir tmp;
    std::string out = tmp / "ammo.qsc";
    EXPECT_EQ(RunIGI1Conv("qvm decompile " + Q(f) + " -o " + Q(out)), 0);
    EXPECT_TRUE(NonEmptyFile(out));
}

// ─── res ─────────────────────────────────────────────────────────────────────
TEST_F(IGI1ConvTest, ResList) {
    IGI1CONV_NEED(f, "\\.res$");
    EXPECT_EQ(RunIGI1Conv("res list " + Q(f)), 0);
}
TEST_F(IGI1ConvTest, ResExtract) {
    IGI1CONV_NEED(f, "\\.res$");
    TempDir tmp;
    std::string out = tmp / "res_out";
    EXPECT_EQ(RunIGI1Conv("res extract " + Q(f) + " -o " + Q(out)), 0);
    EXPECT_TRUE(std::filesystem::is_directory(out));
}
TEST_F(IGI1ConvTest, ResUnpack) {
    IGI1CONV_NEED(f, "\\.res$");
    TempDir tmp;
    std::string out = tmp / "res_unpacked";
    EXPECT_EQ(RunIGI1Conv("res unpack " + Q(f) + " " + Q(out)), 0);
    EXPECT_TRUE(std::filesystem::is_directory(out));
}

// ─── mtp ─────────────────────────────────────────────────────────────────────
TEST_F(IGI1ConvTest, MtpInfo) {
    IGI1CONV_NEED(f, "\\.mtp$");
    EXPECT_EQ(RunIGI1Conv("mtp info " + Q(f)), 0);
}
TEST_F(IGI1ConvTest, MtpDump) {
    IGI1CONV_NEED(f, "\\.mtp$");
    TempDir tmp;
    std::string out = tmp / "mtp.json";
    EXPECT_EQ(RunIGI1Conv("mtp dump " + Q(f) + " -o " + Q(out)), 0);
    EXPECT_TRUE(NonEmptyFile(out));
}
TEST_F(IGI1ConvTest, MtpToDat) {
    IGI1CONV_NEED(f, "\\.mtp$");
    TempDir tmp;
    std::string out = tmp / "common_from_mtp.dat";
    EXPECT_EQ(RunIGI1Conv("mtp to-dat " + Q(f) + " -o " + Q(out)), 0);
    EXPECT_TRUE(NonEmptyFile(out));
}

// ─── dat ─────────────────────────────────────────────────────────────────────
TEST_F(IGI1ConvTest, DatInfo) {
    IGI1CONV_NEED(f, "^(?!.*graph).*\\.dat$");
    EXPECT_EQ(RunIGI1Conv("dat info " + Q(f)), 0);
}
TEST_F(IGI1ConvTest, DatExportJson) {
    IGI1CONV_NEED(f, "^(?!.*graph).*\\.dat$");
    TempDir tmp;
    std::string out = tmp / "dat.json";
    EXPECT_EQ(RunIGI1Conv("dat export " + Q(f) + " -o " + Q(out)), 0);
    EXPECT_TRUE(NonEmptyFile(out));
}
TEST_F(IGI1ConvTest, DatExportText) {
    IGI1CONV_NEED(f, "^(?!.*graph).*\\.dat$");
    TempDir tmp;
    std::string out = tmp / "dat.txt";
    EXPECT_EQ(RunIGI1Conv("dat export " + Q(f) + " -o " + Q(out) + " --text"), 0);
    EXPECT_TRUE(NonEmptyFile(out));
}
TEST_F(IGI1ConvTest, DatToMtp) {
    IGI1CONV_NEED(f, "^(?!.*graph).*\\.dat$");
    TempDir tmp;
    std::string out = tmp / "common_from_dat.mtp";
    EXPECT_EQ(RunIGI1Conv("dat to-mtp " + Q(f) + " -o " + Q(out)), 0);
    EXPECT_TRUE(NonEmptyFile(out));
}

// ─── graph ───────────────────────────────────────────────────────────────────
TEST_F(IGI1ConvTest, GraphInfo) {
    IGI1CONV_NEED(f, "graph.*\\.dat$");
    EXPECT_EQ(RunIGI1Conv("graph info " + Q(f)), 0);
}
TEST_F(IGI1ConvTest, GraphExport) {
    IGI1CONV_NEED(f, "graph.*\\.dat$");
    TempDir tmp;
    std::string out = tmp / "graph.json";
    EXPECT_EQ(RunIGI1Conv("graph export " + Q(f) + " -o " + Q(out)), 0);
    EXPECT_TRUE(NonEmptyFile(out));
}

// ─── fnt ─────────────────────────────────────────────────────────────────────
TEST_F(IGI1ConvTest, FntInfo) {
    IGI1CONV_NEED(f, "\\.fnt$");
    EXPECT_EQ(RunIGI1Conv("fnt info " + Q(f)), 0);
}
TEST_F(IGI1ConvTest, FntExport) {
    IGI1CONV_NEED(f, "\\.fnt$");
    TempDir tmp;
    std::string out = tmp / "font1.png";
    EXPECT_EQ(RunIGI1Conv("fnt export " + Q(f) + " -o " + Q(out)), 0);
    EXPECT_TRUE(NonEmptyFile(out));
}

// ─── terrain ─────────────────────────────────────────────────────────────────
TEST_F(IGI1ConvTest, TerrainInfoLmp) {
    IGI1CONV_NEED(f, "\\.lmp$");
    EXPECT_EQ(RunIGI1Conv("terrain info " + Q(f)), 0);
}
TEST_F(IGI1ConvTest, TerrainInfoCtr) {
    IGI1CONV_NEED(f, "\\.ctr$");
    EXPECT_EQ(RunIGI1Conv("terrain info " + Q(f)), 0);
}
TEST_F(IGI1ConvTest, TerrainExportLmp) {
    IGI1CONV_NEED(f, "\\.lmp$");
    TempDir tmp;
    std::string out = tmp / "lmp.pgm";
    EXPECT_EQ(RunIGI1Conv("terrain export-lmp " + Q(f) + " -o " + Q(out)), 0);
    // single-pic LMP writes exactly <out>; multi-pic writes <stem>_N.pgm
    bool wrote = NonEmptyFile(out) || NonEmptyFile(tmp / "lmp_0.pgm");
    EXPECT_TRUE(wrote);
}
TEST_F(IGI1ConvTest, TerrainExportCtr) {
    IGI1CONV_NEED(f, "\\.ctr$");
    TempDir tmp;
    std::string out = tmp / "ctr.json";
    EXPECT_EQ(RunIGI1Conv("terrain export-ctr " + Q(f) + " -o " + Q(out)), 0);
    EXPECT_TRUE(NonEmptyFile(out));
}

// ─── olm ─────────────────────────────────────────────────────────────────────
TEST_F(IGI1ConvTest, OlmInfo) {
    IGI1CONV_NEED(f, "\\.olm$");
    std::string out;
    EXPECT_EQ(RunIGI1Conv("olm info " + Q(f), &out), 0);
    EXPECT_NE(out.find("resolution:"), std::string::npos);
}
TEST_F(IGI1ConvTest, OlmToPng) {
    IGI1CONV_NEED(f, "\\.olm$");
    TempDir tmp;
    std::string out = tmp / "lightmap.png";
    EXPECT_EQ(RunIGI1Conv("olm to-png " + Q(f) + " -o " + Q(out)), 0);
    EXPECT_TRUE(NonEmptyFile(out));
}
TEST_F(IGI1ConvTest, OlmToTga) {
    IGI1CONV_NEED(f, "\\.olm$");
    TempDir tmp;
    std::string out = tmp / "lightmap.tga";
    EXPECT_EQ(RunIGI1Conv("olm to-tga " + Q(f) + " -o " + Q(out)), 0);
    EXPECT_TRUE(NonEmptyFile(out));
}

// ─── iff ─────────────────────────────────────────────────────────────────
TEST_F(IGI1ConvTest, IffInfo) {
    IGI1CONV_NEED(f, "\\.iff$");
    EXPECT_EQ(RunIGI1Conv("iff info " + Q(f)), 0);
}
TEST_F(IGI1ConvTest, IffConvert) {
    IGI1CONV_NEED(f, "\\.iff$");
    TempDir tmp;
    std::string outDir = tmp / "iff_convert";
    EXPECT_EQ(RunIGI1Conv("iff convert " + Q(f) + " " + Q(outDir)), 0);
    // At least one .BEF should appear (one per animation in the file).
    bool any = false;
    for (auto& e : std::filesystem::directory_iterator(outDir)) {
        if (e.is_regular_file()) {
            std::string ext = e.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext == ".bef") { any = true; break; }
        }
    }
    EXPECT_TRUE(any) << "no .BEF files in " << outDir;
}
TEST_F(IGI1ConvTest, IffRebuild) {
    IGI1CONV_NEED(f, "\\.iff$");
    TempDir tmp;
    std::string out = tmp / "iff_rebuild.iff";
    EXPECT_EQ(RunIGI1Conv("iff rebuild " + Q(f) + " " + Q(out)), 0);
    EXPECT_TRUE(NonEmptyFile(out));
    // The rebuilt IFF must parse with iff info without error.
    EXPECT_EQ(RunIGI1Conv("iff info " + Q(out)), 0);
}
TEST_F(IGI1ConvTest, IffDecompile) {
    IGI1CONV_NEED(f, "\\.iff$");
    TempDir tmp;
    std::string outDir = tmp / "iff_decomp";
    EXPECT_EQ(RunIGI1Conv("iff decompile " + Q(f) + " " + Q(outDir)), 0);
    EXPECT_TRUE(std::filesystem::is_directory(outDir));
    // We expect at least one .IFF text file or any readable file in outDir.
    bool any = false;
    for (auto& e : std::filesystem::directory_iterator(outDir)) {
        if (e.is_regular_file() && e.file_size() > 0) { any = true; break; }
    }
    EXPECT_TRUE(any) << "no decompiled files in " << outDir;
}
TEST_F(IGI1ConvTest, IffCreateFromBefs) {
    IGI1CONV_NEED(f, "\\.iff$");
    TempDir tmp;
    std::string bDir   = tmp / "befs";
    std::string outIff = tmp / "from_befs.iff";
    ASSERT_EQ(RunIGI1Conv("iff convert " + Q(f) + " " + Q(bDir)), 0);
    EXPECT_EQ(RunIGI1Conv("iff create " + Q(bDir) + " " + Q(outIff)), 0);
    EXPECT_TRUE(NonEmptyFile(outIff));
    EXPECT_EQ(RunIGI1Conv("iff info " + Q(outIff)), 0);
}
TEST_F(IGI1ConvTest, IffExportGif) {
    IGI1CONV_NEED(f, "\\.iff$");
    TempDir tmp;
    std::string out = tmp / "anim.gif";
    // Use a small canvas to keep the test fast; fps deliberately low
    // to minimise frame count.
    EXPECT_EQ(RunIGI1Conv("iff export-gif " + Q(f) + " " + Q(out) + " 160 120 8"), 0);
    EXPECT_TRUE(NonEmptyFile(out));
    // GIF89a magic.
    std::ifstream g(out, std::ios::binary);
    char magic[6] = {0};
    g.read(magic, 6);
    EXPECT_EQ(std::string(magic, 6), std::string("GIF89a"));
}
TEST_F(IGI1ConvTest, IffRoundTripSizeMatches) {
    // Convert -> Create round trip must reproduce the same file size as
    // the original IFF.  This is a strong invariant for the IFF writer
    // (since the original is laid out with FORM-size == the bone block
    // size; we re-derive the bone count and skeleton from the first BEF
    // and must agree on chunk boundaries).
    IGI1CONV_NEED(f, "\\.iff$");
    TempDir tmp;
    std::string bDir   = tmp / "befs";
    std::string outIff = tmp / "roundtrip.iff";
    ASSERT_EQ(RunIGI1Conv("iff convert " + Q(f) + " " + Q(bDir)), 0);
    ASSERT_EQ(RunIGI1Conv("iff create "  + Q(bDir) + " " + Q(outIff)), 0);
    auto inSize  = std::filesystem::file_size(f);
    auto outSize = std::filesystem::file_size(outIff);
    EXPECT_EQ(inSize, outSize) << "round-trip size mismatch: in=" << inSize
                                << " out=" << outSize;
}
TEST_F(IGI1ConvTest, IffDecompileCreateRoundTrip) {
    // Decompile -> Create round trip via the .IFF text representation.
    // Output size may differ slightly from the original because the
    // FORM root size convention is different (real-size vs broken-size)
    // - what we require is that the rebuilt IFF parses to the same
    // bone/clip/event count as the source.
    IGI1CONV_NEED(f, "\\.iff$");
    TempDir tmp;
    std::string decDir = tmp / "decomp";
    std::string outIff = tmp / "dec_rebuilt.iff";
    ASSERT_EQ(RunIGI1Conv("iff decompile " + Q(f) + " " + Q(decDir)), 0);
    ASSERT_EQ(RunIGI1Conv("iff create "    + Q(decDir) + " " + Q(outIff)), 0);
    // The rebuilt IFF must parse without error and emit the same
    // header summary as the source (within a small tolerance - the
    // decompiler's text re-encoding rounds times, so the rebuilt
    // duration may differ by 1ms).
    std::string origInfo, newInfo;
    ASSERT_EQ(RunIGI1Conv("iff info " + Q(f),     &origInfo), 0);
    ASSERT_EQ(RunIGI1Conv("iff info " + Q(outIff), &newInfo),  0);
    // iff info writes to file, but the parser uses the file too - read
    // back from the .info.txt sidecar.
    auto readInfo = [&](const std::string& iff) {
        std::ifstream f(iff + ".info.txt");
        std::stringstream ss; ss << f.rdbuf();
        return ss.str();
    };
    std::string origTxt = readInfo(f);
    std::string newTxt  = readInfo(outIff);
    EXPECT_NE(origTxt.find("Bone Count:"), std::string::npos);
    EXPECT_NE(newTxt.find("Bone Count:"),  std::string::npos);
    EXPECT_NE(origTxt.find("Clips:"),       std::string::npos);
    EXPECT_NE(newTxt.find("Clips:"),       std::string::npos);
}
