// test_igi1conv_wav.cpp -- exercise the `wav` command end-to-end against
// synthesized ILSF inputs.  No corpus dependency: every fixture is built
// fresh in a TempDir from a tiny known payload so the assertions are
// deterministic.
//
// Subtests:
//   - InfoRaw / InfoRawResident / InfoAdpcm:    header parser + method gating
//   - ConvertRawToWav / ConvertRawResidentToWav: full decode to standard PCM WAV
//   - ConvertAdpcmDecodes:                      ADPCM is now decoded (was "Refused")
//   - ConvertBadSignature:                      non-ILSF files are rejected
//   - ConvertDirMixed:                          batch walker with per-file errors
//   - ConvertAutoExtFromDir:                    -o <dir> picks .wav
//   - ConvertRejectsMp3:                        MP3 output is no longer supported
#include "igi1conv_test_util.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace igi1conv_test;

namespace {

// Build a minimal ILSF file in-memory:
//   sig "ILSF" + 4 shorts (method, bits, channels, unknown) + 2 uints (rate, count)
//   + N interleaved 16-bit signed LE samples.
struct IlsfParams {
    uint16_t method;     // 0=RAW, 1=RAW_RESIDENT, 2=ADPCM, 3=ADPCM_RESIDENT
    uint16_t bits;       // always 16
    uint16_t channels;   // 1 or 2
    uint16_t unknown;    // 0
    uint32_t rate;       // 11025 / 22050 / 44100
    uint32_t count;      // frame count
};

std::vector<uint8_t> make_ilsf(const IlsfParams& p,
                               const std::vector<int16_t>& samples)
{
    std::vector<uint8_t> out;
    out.reserve(20 + samples.size() * 2);
    auto put_u8 = [&](char c) { out.push_back(static_cast<uint8_t>(c)); };
    auto put_u16 = [&](uint16_t v) {
        out.push_back(static_cast<uint8_t>(v & 0xFF));
        out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    };
    auto put_u32 = [&](uint32_t v) {
        out.push_back(static_cast<uint8_t>(v & 0xFF));
        out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
        out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    };
    put_u8('I'); put_u8('L'); put_u8('S'); put_u8('F');
    put_u16(p.method);
    put_u16(p.bits);
    put_u16(p.channels);
    put_u16(p.unknown);
    put_u32(p.rate);
    put_u32(p.count);
    for (int16_t s : samples) put_u16(static_cast<uint16_t>(s));
    return out;
}

void write_bytes(const std::string& path, const std::vector<uint8_t>& bytes)
{
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
}

// Synthesise a 1 kHz sine wave, mono, 16-bit, ~0.1 s.
std::vector<int16_t> sine_mono_16(double freq, uint32_t rate, double dur_s,
                                 int16_t amp = 8000)
{
    std::vector<int16_t> out;
    out.reserve(static_cast<size_t>(rate * dur_s));
    const double two_pi = 6.283185307179586;
    for (uint32_t i = 0; i < static_cast<uint32_t>(rate * dur_s); ++i) {
        double v = amp * std::sin(two_pi * freq * static_cast<double>(i) / rate);
        if (v >  32767.0) v =  32767.0;
        if (v < -32768.0) v = -32768.0;
        out.push_back(static_cast<int16_t>(v));
    }
    return out;
}

// Read a 32-bit little-endian word at `offset` in `path`.
bool read_u32_le(const std::string& path, size_t offset, uint32_t& out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.seekg(static_cast<std::streamsize>(offset));
    uint8_t b[4];
    f.read(reinterpret_cast<char*>(b), 4);
    if (f.gcount() != 4) return false;
    out = static_cast<uint32_t>(b[0])        |
         (static_cast<uint32_t>(b[1]) <<  8) |
         (static_cast<uint32_t>(b[2]) << 16) |
         (static_cast<uint32_t>(b[3]) << 24);
    return true;
}

bool read_u16_le(const std::string& path, size_t offset, uint16_t& out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.seekg(static_cast<std::streamsize>(offset));
    uint8_t b[2];
    f.read(reinterpret_cast<char*>(b), 2);
    if (f.gcount() != 2) return false;
    out = static_cast<uint16_t>(b[0] | (b[1] << 8));
    return true;
}

// True iff `path` looks like a standard RIFF/WAVE PCM file with the
// expected fmt and data chunk parameters.
bool is_valid_pcm_wav(const std::string& path,
                      uint16_t expect_channels,
                      uint32_t expect_rate,
                      uint16_t expect_bits,
                      uint32_t& data_size_out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    auto rd_u32 = [&]() -> uint32_t {
        char b[4]; f.read(b, 4);
        return static_cast<uint32_t>(static_cast<uint8_t>(b[0]))        |
               (static_cast<uint32_t>(static_cast<uint8_t>(b[1])) <<  8) |
               (static_cast<uint32_t>(static_cast<uint8_t>(b[2])) << 16) |
               (static_cast<uint32_t>(static_cast<uint8_t>(b[3])) << 24);
    };
    auto rd_u16 = [&]() -> uint16_t {
        uint8_t b[2]; f.read(reinterpret_cast<char*>(b), 2);
        return static_cast<uint16_t>(b[0] | (b[1] << 8));
    };
    char riff[4]; f.read(riff, 4);
    if (f.gcount() != 4 || std::string(riff, 4) != "RIFF") return false;
    (void)rd_u32();  // RIFF size
    char wave[4]; f.read(wave, 4);
    if (f.gcount() != 4 || std::string(wave, 4) != "WAVE") return false;
    char fmt_[4]; f.read(fmt_, 4);
    if (f.gcount() != 4 || std::string(fmt_, 4) != "fmt ") return false;
    uint32_t fmt_size = rd_u32();
    if (fmt_size < 16) return false;
    uint16_t pcm_format = rd_u16();
    if (pcm_format != 1) return false;
    uint16_t channels = rd_u16();
    if (channels != expect_channels) return false;
    uint32_t rate; f.read(reinterpret_cast<char*>(&rate), 4);
    if (rate != expect_rate) return false;
    (void)rd_u32();          // byte_rate
    (void)rd_u16();          // block_align
    uint16_t bits = rd_u16();
    if (bits != expect_bits) return false;
    f.seekg(static_cast<std::streamsize>(fmt_size - 16), std::ios::cur);
    char data[4]; f.read(data, 4);
    if (f.gcount() != 4 || std::string(data, 4) != "data") return false;
    data_size_out = rd_u32();
    return true;
}

} // namespace

// ─── info ───────────────────────────────────────────────────────────────────

TEST_F(IGI1ConvTest, WavInfoRaw) {
    TempDir tmp;
    auto samples = sine_mono_16(1000.0, 22050, 0.05);
    IlsfParams p{0, 16, 1, 0, 22050, static_cast<uint32_t>(samples.size())};
    write_bytes(tmp / "tone.wav", make_ilsf(p, samples));

    std::string out;
    ASSERT_EQ(RunIGI1Conv("wav info " + Q(tmp / "tone.wav"), &out), 0);
    EXPECT_NE(out.find("signature:           ILSF"),       std::string::npos);
    EXPECT_NE(out.find("sound_pack_method:   0 (RAW)"),    std::string::npos);
    EXPECT_NE(out.find("channels:            1"),          std::string::npos);
    EXPECT_NE(out.find("frame_rate:          22050 Hz"),   std::string::npos);
    EXPECT_NE(out.find("duration:            "),            std::string::npos);
    EXPECT_NE(out.find("supported:           yes"),        std::string::npos);
}

TEST_F(IGI1ConvTest, WavInfoRawResident) {
    TempDir tmp;
    auto samples = sine_mono_16(440.0, 11025, 0.02);
    IlsfParams p{1, 16, 1, 0, 11025, static_cast<uint32_t>(samples.size())};
    write_bytes(tmp / "tone_rr.wav", make_ilsf(p, samples));

    std::string out;
    ASSERT_EQ(RunIGI1Conv("wav info " + Q(tmp / "tone_rr.wav"), &out), 0);
    EXPECT_NE(out.find("sound_pack_method:   1 (RAW_RESIDENT)"), std::string::npos);
    EXPECT_NE(out.find("supported:           yes"),              std::string::npos);
}

TEST_F(IGI1ConvTest, WavInfoAdpcm) {
    TempDir tmp;
    auto samples = sine_mono_16(440.0, 22050, 0.02);
    IlsfParams p{2, 16, 1, 0, 22050, static_cast<uint32_t>(samples.size())};
    write_bytes(tmp / "tone_a.wav", make_ilsf(p, samples));

    std::string out;
    ASSERT_EQ(RunIGI1Conv("wav info " + Q(tmp / "tone_a.wav"), &out), 0);
    EXPECT_NE(out.find("sound_pack_method:   2 (ADPCM)"), std::string::npos);
    // ADPCM is now decoded (4-bit IMA ADPCM, initial state (0, 0)).
    EXPECT_NE(out.find("supported:           yes"),
              std::string::npos);
}

TEST_F(IGI1ConvTest, WavInfoMissingFile) {
    EXPECT_EQ(RunIGI1Conv("wav info \"C:/nope/no_such_file.wav\""), 2);
}

TEST_F(IGI1ConvTest, WavInfoBadSignature) {
    TempDir tmp;
    // Write a file that is exactly the right size (20 bytes) but starts with
    // "JUNK" instead of "ILSF" - must be rejected as not an IGI .wav.
    std::vector<uint8_t> junk(20, 0xAA);
    junk[0] = 'J'; junk[1] = 'U'; junk[2] = 'N'; junk[3] = 'K';
    write_bytes(tmp / "junk.wav", junk);
    EXPECT_EQ(RunIGI1Conv("wav info " + Q(tmp / "junk.wav")), 3);
}

// ─── convert → .wav ─────────────────────────────────────────────────────────

TEST_F(IGI1ConvTest, WavConvertRawToWav) {
    TempDir tmp;
    auto samples = sine_mono_16(1000.0, 22050, 0.05);
    IlsfParams p{0, 16, 1, 0, 22050, static_cast<uint32_t>(samples.size())};
    write_bytes(tmp / "in.wav", make_ilsf(p, samples));
    std::string out = tmp / "out.wav";

    ASSERT_EQ(RunIGI1Conv("wav convert " + Q(tmp / "in.wav") + " -o " + Q(out)), 0);
    ASSERT_TRUE(NonEmptyFile(out));

    uint32_t data_size = 0;
    ASSERT_TRUE(is_valid_pcm_wav(out, 1, 22050, 16, data_size));
    EXPECT_EQ(data_size, samples.size() * 2u);
}

TEST_F(IGI1ConvTest, WavConvertRawResidentToWav) {
    TempDir tmp;
    auto samples = sine_mono_16(440.0, 11025, 0.05);
    IlsfParams p{1, 16, 1, 0, 11025, static_cast<uint32_t>(samples.size())};
    write_bytes(tmp / "in.wav", make_ilsf(p, samples));
    std::string out = tmp / "out.wav";

    ASSERT_EQ(RunIGI1Conv("wav convert " + Q(tmp / "in.wav") + " -o " + Q(out)), 0);
    uint32_t data_size = 0;
    ASSERT_TRUE(is_valid_pcm_wav(out, 1, 11025, 16, data_size));
    EXPECT_EQ(data_size, samples.size() * 2u);
}

TEST_F(IGI1ConvTest, WavConvertStereoToWav) {
    TempDir tmp;
    auto mono = sine_mono_16(1000.0, 44100, 0.02);
    // Interleave left=mono, right=mono+1/4-sample-shifted for variety.
    std::vector<int16_t> stereo;
    stereo.reserve(mono.size() * 2);
    for (size_t i = 0; i < mono.size(); ++i) {
        stereo.push_back(mono[i]);
        size_t j = (i + mono.size() / 4) % mono.size();
        stereo.push_back(mono[j]);
    }
    IlsfParams p{0, 16, 2, 0, 44100, static_cast<uint32_t>(mono.size())};
    write_bytes(tmp / "in.wav", make_ilsf(p, stereo));
    std::string out = tmp / "out.wav";

    ASSERT_EQ(RunIGI1Conv("wav convert " + Q(tmp / "in.wav") + " -o " + Q(out)), 0);
    uint32_t data_size = 0;
    ASSERT_TRUE(is_valid_pcm_wav(out, 2, 44100, 16, data_size));
    EXPECT_EQ(data_size, stereo.size() * 2u);
}

// ADPCM is now decoded by the 4-bit IMA ADPCM algorithm (initial
// state predictor=0, step_index=0).  The synthesized input below
// stores raw 16-bit PCM samples in the ADPCM payload field, so the
// resulting audio will be nonsense - but the convert must still
// succeed: the WAV writer produces a syntactically valid file with
// `frame_count` samples regardless of the decoded content.  This
// guards the pipeline against any regression that would re-introduce
// the "ADPCM not yet supported" error path.
TEST_F(IGI1ConvTest, WavConvertAdpcmDecodes) {
    TempDir tmp;
    auto samples = sine_mono_16(440.0, 22050, 0.02);
    IlsfParams p{2, 16, 1, 0, 22050, static_cast<uint32_t>(samples.size())};
    write_bytes(tmp / "in.wav", make_ilsf(p, samples));
    std::string out = tmp / "out.wav";
    EXPECT_EQ(RunIGI1Conv("wav convert " + Q(tmp / "in.wav") + " -o " + Q(out)), 0);
    ASSERT_TRUE(NonEmptyFile(out));

    // Verify the output is a valid WAV with the expected framing.
    // Header layout:
    //   off  0..3   "RIFF"
    //   off  4..7   RIFF size
    //   off  8..11  "WAVE"
    //   off 12..15  "fmt "
    //   off 16..19  fmt size
    //   off 20..21  PCM format (1)
    //   off 22..23  channels
    //   off 24..27  sample rate
    //   off 28..31  byte rate
    //   off 32..33  block align
    //   off 34..35  bits per sample
    std::ifstream in(out, std::ios::binary);
    ASSERT_TRUE(in.is_open());
    char riff[4]; in.read(riff, 4);
    EXPECT_EQ(std::string(riff, 4), "RIFF");
    uint32_t riff_size = 0; in.read(reinterpret_cast<char*>(&riff_size), 4);
    EXPECT_GT(riff_size, 36u);
    char wave[4]; in.read(wave, 4);
    EXPECT_EQ(std::string(wave, 4), "WAVE");
    in.seekg(22);
    uint16_t channels = 0, bits = 0;
    in.read(reinterpret_cast<char*>(&channels), 2);
    in.seekg(34);
    in.read(reinterpret_cast<char*>(&bits), 2);
    EXPECT_EQ(channels, 1u);
    EXPECT_EQ(bits, 16u);
}

TEST_F(IGI1ConvTest, WavConvertBadSignature) {
    TempDir tmp;
    std::vector<uint8_t> junk(40, 0xCC);
    junk[0] = 'J'; junk[1] = 'U'; junk[2] = 'N'; junk[3] = 'K';
    write_bytes(tmp / "junk.wav", junk);
    EXPECT_EQ(RunIGI1Conv("wav convert " + Q(tmp / "junk.wav") +
                          " -o " + Q(tmp / "out.wav")), 3);
}

TEST_F(IGI1ConvTest, WavConvertMissingFile) {
    EXPECT_EQ(RunIGI1Conv("wav convert \"C:/nope/x.wav\" "
                          "-o \"C:/nope/y.wav\""), 2);
}

// ─── convert-dir ────────────────────────────────────────────────────────────

TEST_F(IGI1ConvTest, WavConvertDirMixed) {
    TempDir tmp;
    // Source dir with 5 files: 4 decodable (incl. ADPCM), 2 undecodable.
    auto s1 = sine_mono_16(1000.0, 22050, 0.02);
    auto s2 = sine_mono_16(440.0, 11025, 0.02);
    auto s3 = sine_mono_16(440.0, 22050, 0.02);  // ADPCM
    write_bytes(tmp / "good_raw.wav",
                make_ilsf({0,16,1,0,22050,(uint32_t)s1.size()}, s1));
    write_bytes(tmp / "good_rr.wav",
                make_ilsf({1,16,1,0,11025,(uint32_t)s2.size()}, s2));
    write_bytes(tmp / "adpcm.wav",
                make_ilsf({2,16,1,0,22050,(uint32_t)s3.size()}, s3));
    // Garbage files (not ILSF).
    std::vector<uint8_t> junk(20, 0xEE);
    write_bytes(tmp / "junk1.wav", junk);
    junk[0] = 'J'; junk[1] = 'U'; junk[2] = 'N'; junk[3] = 'K';
    write_bytes(tmp / "junk2.wav", junk);
    // Subdirectory with one more decodable file to exercise recursion.
    std::filesystem::create_directory(tmp / "sub");
    {
        std::string sub = tmp / "sub";
        write_bytes(sub + "\\nested.wav",
                    make_ilsf({0,16,1,0,22050,(uint32_t)s1.size()}, s1));
    }

    std::string outDir = tmp / "out";
    int rc = RunIGI1Conv("wav convert-dir " + Q(tmp.str()) + " -o " + Q(outDir));
    EXPECT_EQ(rc, 0) << "expected per-file failures to be reported but the "
                        "batch to still exit 0; got " << rc;

    // All 4 decodable files (good_raw, good_rr, adpcm, sub/nested) must appear.
    EXPECT_TRUE(NonEmptyFile(outDir + "\\good_raw.wav"));
    EXPECT_TRUE(NonEmptyFile(outDir + "\\good_rr.wav"));
    EXPECT_TRUE(NonEmptyFile(outDir + "\\adpcm.wav"));
    EXPECT_TRUE(NonEmptyFile(outDir + "\\sub\\nested.wav"));
    // No undecodable output.
    EXPECT_FALSE(NonEmptyFile(outDir + "\\junk1.wav"));
    EXPECT_FALSE(NonEmptyFile(outDir + "\\junk2.wav"));
}

TEST_F(IGI1ConvTest, WavConvertAutoExtFromDir) {
    // -o <existing-dir> with no --mp3 should produce .wav with the input
    // basename.
    TempDir tmp;
    auto s = sine_mono_16(1000.0, 22050, 0.01);
    write_bytes(tmp / "clip.wav",
                make_ilsf({0,16,1,0,22050,(uint32_t)s.size()}, s));
    std::string outDir = tmp / "outdir";
    std::filesystem::create_directory(outDir);
    ASSERT_EQ(RunIGI1Conv("wav convert " + Q(tmp / "clip.wav") + " -o " + Q(outDir)), 0);
    EXPECT_TRUE(NonEmptyFile(outDir + "\\clip.wav"));
}

// ─── convert → .mp3 is no longer supported ────────────────────────────────

// Earlier versions of the build supported `wav convert ... -o out.mp3`
// via an external `lame.exe` (and later a bundled LAME DLL).  Both
// paths have been removed so the project ships as a single binary
// with zero external dependencies.  This test guards the new
// "no MP3" contract: asking for an `.mp3` output must return
// exit code 1 with a clear error message, and no MP3 file may
// be created.
TEST_F(IGI1ConvTest, WavConvertRejectsMp3) {
    TempDir tmp;
    auto s = sine_mono_16(1000.0, 22050, 0.05);
    write_bytes(tmp / "in.wav",
                make_ilsf({0,16,1,0,22050,(uint32_t)s.size()}, s));
    std::string out = tmp / "out.mp3";

    int rc = RunIGI1Conv("wav convert " + Q(tmp / "in.wav") + " -o " + Q(out), nullptr, 60000);
    EXPECT_EQ(rc, 1) << "expected exit code 1 for .mp3 (no longer supported)";
    EXPECT_FALSE(NonEmptyFile(out)) << "no .mp3 file should be produced";
}

// ─── --no-recursive and ADPCM batch behaviour ───────────────────────────────

// `--no-recursive` on `wav convert-dir` should only convert the
// files in the top-level directory and leave any subdirectory
// `*.wav` files alone.  This guards the "the user picked this
// folder, not the whole tree" semantics.
TEST_F(IGI1ConvTest, WavConvertDirHonoursNoRecursive) {
    TempDir tmp;
    auto s = sine_mono_16(1000.0, 22050, 0.02);
    write_bytes(tmp / "top.wav",
                make_ilsf({0,16,1,0,22050,(uint32_t)s.size()}, s));
    {
        std::string subDir = tmp.str() + "/sub";
        std::filesystem::create_directory(subDir);
        write_bytes(subDir + "/nested.wav",
                    make_ilsf({0,16,1,0,22050,(uint32_t)s.size()}, s));
    }

    std::string outDir = tmp / "out";
    ASSERT_EQ(RunIGI1Conv("wav convert-dir " + Q(tmp.str()) +
                              " -o " + Q(outDir) + " --no-recursive"),
              0);
    EXPECT_TRUE(NonEmptyFile(outDir + "\\top.wav"));
    EXPECT_FALSE(NonEmptyFile(outDir + "\\sub\\nested.wav"));
}

// `wav convert-dir` on a directory with no *.wav files must
// exit 0 (the "nothing to do is not an error" rule) and produce
// no output files.  This is the same rule the rest of the project
// follows for empty / all-failed batches.
TEST_F(IGI1ConvTest, WavConvertDirEmptyIsNotAnError) {
    TempDir tmp;
    // Drop a non-`.wav` file in the source dir so the directory is
    // not literally empty; the batch must still report success with
    // no output because no `*.wav` matched.
    {
        std::string txtPath = tmp.str() + std::string("/readme.txt");
        std::ofstream f(txtPath);
        f << "no wav here";
    }
    std::string outDir = tmp / "out";
    EXPECT_EQ(RunIGI1Conv("wav convert-dir " + Q(tmp.str()) +
                              " -o " + Q(outDir)),
              0);
    // outDir exists (the CLI creates it) but is empty.
    EXPECT_TRUE(std::filesystem::is_directory(outDir));
    EXPECT_TRUE(std::filesystem::is_empty(outDir));
}

// `wav convert -o <dir>` (a directory, not a file) with no `--mp3`
// should auto-derive `<src-stem>.wav` inside the directory.  This
// is the "drop into a folder and get a sensible name" default.
TEST_F(IGI1ConvTest, WavConvertAutoDerivesWavInDir) {
    TempDir tmp;
    auto s = sine_mono_16(440.0, 22050, 0.02);
    {
        std::string inPath = tmp.str() + std::string("/synth_sine.wav");
        write_bytes(inPath,
                    make_ilsf({0,16,1,0,22050,(uint32_t)s.size()}, s));
    }

    std::string outDir = tmp / "out";
    std::filesystem::create_directory(outDir);
    EXPECT_EQ(RunIGI1Conv("wav convert " + Q(tmp.str() + std::string("/synth_sine.wav")) +
                              " -o " + Q(outDir)),
              0);
    EXPECT_TRUE(NonEmptyFile(outDir + "\\synth_sine.wav"));
}

// `wav convert` with an unsupported output extension (anything
// other than `.wav`) must return exit code 1 and not produce a
// file.  This locks the "single output format" contract.
TEST_F(IGI1ConvTest, WavConvertRejectsUnknownExtension) {
    TempDir tmp;
    auto s = sine_mono_16(440.0, 22050, 0.02);
    {
        std::string inPath = tmp.str() + std::string("/in.wav");
        write_bytes(inPath,
                    make_ilsf({0,16,1,0,22050,(uint32_t)s.size()}, s));
    }
    const std::string inPath = tmp.str() + std::string("/in.wav");
    const std::vector<std::string> badExts = {".flac", ".ogg", ".txt", ".bin"};
    for (const auto& ext : badExts) {
        std::string out = tmp.str() + std::string("/out") + ext;
        EXPECT_EQ(RunIGI1Conv("wav convert " + Q(inPath) +
                                  " -o " + Q(out)),
                  1) << "ext=" << ext;
        EXPECT_FALSE(NonEmptyFile(out)) << "ext=" << ext;
    }
}
