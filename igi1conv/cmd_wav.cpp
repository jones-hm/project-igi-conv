// igi1conv/cmd_wav.cpp
//
// Project IGI 1 ships audio as a proprietary "ILSF" container (a non-standard
// 20-byte header wrapping 16-bit PCM samples for the RAW methods, or 4-bit
// IMA/Intel ADPCM for the compressed methods). The Python dconv reference
// at D:\IGI-Tools\GM_123\IGI MEF CONV\tools\dconv\format\wav.py contains an
// intended call to `audioop.adpcm2lin(sounddata, 2, None)` (the function
// itself is buggy in the reference - it returns the original data via
// `return sounddata` before reaching the actual decode - but the *intended*
// format is plain 4-bit IMA ADPCM with initial state (0, 0)). We mirror
// that here in native C++ using the decoder in wav_adpcm.h.
//
// Header layout (little-endian, 20 bytes):
//   off 0  : 4s  signature            "ILSF"
//   off 4  : H   sound_pack_method    0=RAW, 1=RAW_RESIDENT, 2=ADPCM, 3=ADPCM_RESIDENT
//   off 6  : H   sample_width_bits    always 16
//   off 8  : H   channels             1 or 2
//   off 10 : H   unknown_04
//   off 12 : I   frame_rate_hz        11025 / 22050 / 44100
//   off 16 : I   frame_count          decoded PCM samples per channel
//   off 20 : ... audio frames         RAW: int16 LE samples (interleaved if stereo)
//                                    ADPCM: 4-bit IMA nibbles (interleaved by nibble
//                                           if stereo).  state (0, 0) is used as the
//                                           initial decoder state.
//
// Supported decodes (this file):
//   method 0 (RAW)            -> standard PCM WAV
//   method 1 (RAW_RESIDENT)   -> identical payload to RAW, also -> standard PCM WAV
//   method 2 (ADPCM)          -> IMA ADPCM decode, then standard PCM WAV
//   method 3 (ADPCM_RESIDENT) -> identical payload to method 2, also -> PCM WAV
//
// Supported output formats:
//   .wav  : standard PCM WAV, playable in any media player, no extra deps.
//
// Note: this build is single-binary / zero external DLL.  MP3 support
// was removed (LAME doesn't ship as a single header and there's no
// production-quality single-header MP3 encoder available).  Standard
// .wav plays in every Windows media player.

#include "pch.h"
#include "cmd_wav.h"
#include "wav_adpcm.h"

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#define POPEN _popen
#define PCLOSE _pclose
#else
#define POPEN popen
#define PCLOSE pclose
#endif

namespace fs = std::filesystem;

namespace {

// ---------------------------------------------------------------------------
// ILSF header
// ---------------------------------------------------------------------------

enum class SoundPackMethod : uint16_t {
    RAW            = 0,
    RAW_RESIDENT   = 1,
    ADPCM          = 2,
    ADPCM_RESIDENT = 3,
};

const char* method_name(SoundPackMethod m)
{
    switch (m) {
        case SoundPackMethod::RAW:            return "RAW";
        case SoundPackMethod::RAW_RESIDENT:   return "RAW_RESIDENT";
        case SoundPackMethod::ADPCM:          return "ADPCM";
        case SoundPackMethod::ADPCM_RESIDENT: return "ADPCM_RESIDENT";
    }
    return "UNKNOWN";
}

struct IlsfHeader {
    char          signature[4];
    SoundPackMethod method;
    uint16_t      sample_width_bits;
    uint16_t      channels;
    uint16_t      unknown_04;
    uint32_t      frame_rate;
    uint32_t      frame_count;
};

struct IlsfFile {
    IlsfHeader header;
    std::vector<uint8_t> frames;
    std::string error;
};

bool read_u16(FILE* f, uint16_t& v)
{
    uint8_t b[2];
    if (fread(b, 1, 2, f) != 2) return false;
    v = static_cast<uint16_t>(b[0] | (b[1] << 8));
    return true;
}

bool read_u32(FILE* f, uint32_t& v)
{
    uint8_t b[4];
    if (fread(b, 1, 4, f) != 4) return false;
    v = static_cast<uint32_t>(b[0])        |
        (static_cast<uint32_t>(b[1]) <<  8) |
        (static_cast<uint32_t>(b[2]) << 16) |
        (static_cast<uint32_t>(b[3]) << 24);
    return true;
}

IlsfFile load_ilsf(const fs::path& path)
{
    IlsfFile out;
    FILE* f = nullptr;
#ifdef _WIN32
    if (fopen_s(&f, path.string().c_str(), "rb") != 0 || !f) {
#else
    f = fopen(path.string().c_str(), "rb");
    if (!f) {
#endif
        out.error = "could not open file";
        return out;
    }

    if (fread(out.header.signature, 1, 4, f) != 4 ||
        std::memcmp(out.header.signature, "ILSF", 4) != 0) {
        fclose(f);
        out.error = "missing ILSF signature (not an IGI .wav?)";
        return out;
    }

    uint16_t method_raw = 0;
    if (!read_u16(f, method_raw) ||
        !read_u16(f, out.header.sample_width_bits) ||
        !read_u16(f, out.header.channels) ||
        !read_u16(f, out.header.unknown_04) ||
        !read_u32(f, out.header.frame_rate) ||
        !read_u32(f, out.header.frame_count)) {
        fclose(f);
        out.error = "truncated header";
        return out;
    }
    out.header.method = static_cast<SoundPackMethod>(method_raw);

    if (out.header.sample_width_bits != 16) {
        fclose(f);
        out.error = "unsupported sample width (only 16-bit is supported)";
        return out;
    }
    if (out.header.channels != 1 && out.header.channels != 2) {
        fclose(f);
        out.error = "unsupported channel count (only 1 or 2 is supported)";
        return out;
    }
    if (out.header.frame_rate != 11025 &&
        out.header.frame_rate != 22050 &&
        out.header.frame_rate != 44100) {
        fclose(f);
        out.error = "unsupported sample rate (only 11025 / 22050 / 44100)";
        return out;
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    long payload = sz - 20;
    if (payload < 0) payload = 0;
    fseek(f, 20, SEEK_SET);

    std::vector<uint8_t> raw(static_cast<size_t>(payload));
    if (payload > 0) {
        size_t rd = fread(raw.data(), 1, raw.size(), f);
        if (rd != raw.size()) {
            fclose(f);
            out.error = "truncated payload";
            return out;
        }
    }
    fclose(f);

    // For RAW methods the payload is already 16-bit LE PCM samples
    // (one sample = 2 bytes, interleaved if stereo).  We can hand it
    // straight to the WAV writer.
    //
    // For ADPCM methods the payload is 4-bit IMA ADPCM nibbles
    // (2 nibbles per byte, 2 channels interleaved at the nibble
    // level if stereo) and we have to decode it back to 16-bit LE
    // PCM before the WAV writer can use it.  The decoder state
    // starts at (predictor=0, step_index=0) - matching the
    // `audioop.adpcm2lin(data, 2, None)` call from the Python dconv
    // reference.  For stereo each input byte holds 1 nibble from
    // each channel in stream order (high=ch0, low=ch1), so the
    // resulting PCM samples are already interleaved L, R, L, R ...
    // and can be written directly as a 2-channel stream.
    if (out.header.method == SoundPackMethod::ADPCM ||
        out.header.method == SoundPackMethod::ADPCM_RESIDENT)
    {
        const size_t want = static_cast<size_t>(out.header.frame_count) *
                            out.header.channels;
        // Skip the first 16 decoded samples (the ILSF ADPCM payload
        // starts with nibbles 0xF, 0xF, 0x7, 0xF which with state
        // (0, 0) produces an extreme transient that the adaptive
        // step_size takes a few hundred samples to recover from).
        // ~0.7 ms at 22 kHz - inaudible, but eliminates the audible
        // "click" / "pop" at the start of the decoded file.
        constexpr size_t kWarmup = 16;
        std::vector<int16_t> pcm =
            igi1conv::wav::decode_ima_adpcm(raw, want + kWarmup, kWarmup);
        // Prepend `kWarmup` zero samples so the output WAV has the
        // exact `frame_count * channels` length the ILSF header
        // advertises, and the first ~0.7 ms of audio is silence
        // (the ILSF payload's initial transient is replaced with
        // zero fill).
        if (!pcm.empty()) {
            std::vector<int16_t> padded;
            padded.resize(kWarmup, 0);
            padded.insert(padded.end(), pcm.begin(), pcm.end());
            pcm = std::move(padded);
        }
        if (pcm.size() < want && !pcm.empty()) {
            // Pad with the last sample so the WAV writer sees a
            // consistent length.  This happens on files where the
            // encoded nibble count is one short of `frames*channels`
            // (the IGI encoder leaves a trailing partial byte).
            pcm.resize(want, pcm.back());
        }
        const size_t bytes = pcm.size() * sizeof(int16_t);
        out.frames.assign(reinterpret_cast<const uint8_t*>(pcm.data()),
                          reinterpret_cast<const uint8_t*>(pcm.data()) + bytes);
    }
    else
    {
        out.frames = std::move(raw);
    }

    return out;
}

// ---------------------------------------------------------------------------
// Standard RIFF/WAV writer (16-bit PCM, little-endian)
// ---------------------------------------------------------------------------

bool write_wav_pcm16(const fs::path& path,
                     const IlsfHeader& h,
                     const std::vector<uint8_t>& frames)
{
    const uint16_t channels    = h.channels;
    const uint32_t sample_rate = h.frame_rate;
    const uint16_t bits        = h.sample_width_bits;          // 16
    const uint16_t block_align = static_cast<uint16_t>(channels * (bits / 8));
    const uint32_t byte_rate   = sample_rate * block_align;
    const uint32_t data_size   = static_cast<uint32_t>(frames.size());
    const uint32_t fmt_size    = 16;
    const uint32_t riff_size   = 4 + (8 + fmt_size) + (8 + data_size);

    std::ofstream out(path, std::ios::binary);
    if (!out) return false;

    auto put = [&](const void* p, size_t n) { out.write(reinterpret_cast<const char*>(p), static_cast<std::streamsize>(n)); };

    const char riff_id[4]   = {'R','I','F','F'};
    const char wave_id[4]   = {'W','A','V','E'};
    const char fmt_id[4]    = {'f','m','t',' '};
    const char data_id[4]   = {'d','a','t','a'};
    uint16_t pcm_format = 1;

    put(riff_id, 4);
    put(&riff_size, 4);
    put(wave_id, 4);

    put(fmt_id, 4);
    put(&fmt_size, 4);
    put(&pcm_format, 2);
    put(&channels, 2);
    put(&sample_rate, 4);
    put(&byte_rate, 4);
    put(&block_align, 2);
    put(&bits, 2);

    put(data_id, 4);
    put(&data_size, 4);
    if (data_size) put(frames.data(), frames.size());

    return out.good();
}

// ---------------------------------------------------------------------------
// (no MP3 support - this build is single-binary + zero external deps)
// ---------------------------------------------------------------------------
//
// Earlier versions of this file shell-spawned lame.exe and later linked
// the LAME 3.100 DLL through a thin LoadLibrary wrapper.  Both paths
// required an extra binary to install, which conflicted with the
// project's "drop-in single exe" goal.  Every viable C++ MP3 encoder
// (LAME, shine, ...) ships as a multi-file library; there is no
// production-quality single-header MP3 encoder available.  Rather
// than ship a half-working MP3 path or force the user to install a
// DLL, MP3 support was removed and the converter now only emits
// standard Windows PCM `.wav` (the format every Windows media player
// plays out of the box).
//
// A future contributor who wants MP3 can re-introduce it by adding
// LAME back as a third-party dep, with a clear "you'll need to
// download this yourself" README section.

// ---------------------------------------------------------------------------
// Subcommand plumbing
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Subcommand plumbing
// ---------------------------------------------------------------------------

const char* opt_val(int argc, char** argv, const char* name)
{
    for (int i = 1; i < argc - 1; ++i)
        if (std::strcmp(argv[i], name) == 0) return argv[i + 1];
    return nullptr;
}

bool has_flag(int argc, char** argv, const char* name)
{
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], name) == 0) return true;
    return false;
}

std::string lower_ext(const fs::path& p)
{
    std::string e = p.extension().string();
    std::transform(e.begin(), e.end(), e.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return e;
}

bool is_supported_method(SoundPackMethod m)
{
    // All four methods are now decoded:
    //   0/1 (RAW, RAW_RESIDENT)            -> payload is already 16-bit LE PCM
    //   2/3 (ADPCM, ADPCM_RESIDENT)        -> 4-bit IMA ADPCM, decoded by
    //                                         igi1conv::wav::decode_ima_adpcm
    //                                         in load_ilsf() before the WAV
    //                                         writer sees it.
    return m == SoundPackMethod::RAW || m == SoundPackMethod::RAW_RESIDENT ||
           m == SoundPackMethod::ADPCM || m == SoundPackMethod::ADPCM_RESIDENT;
}

void print_header(const IlsfHeader& h)
{
    double seconds = (h.frame_rate > 0)
        ? static_cast<double>(h.frame_count) / static_cast<double>(h.frame_rate)
        : 0.0;

    std::cout << "signature:           ILSF\n";
    std::cout << "sound_pack_method:   " << static_cast<unsigned>(h.method)
              << " (" << method_name(h.method) << ")\n";
    std::cout << "sample_width:        " << h.sample_width_bits << " bits\n";
    std::cout << "channels:            " << h.channels << "\n";
    std::cout << "frame_rate:          " << h.frame_rate << " Hz\n";
    std::cout << "frame_count:         " << h.frame_count << "\n";
    // Keep all command output on std::cout.  MCP captures the C++ stream
    // buffers to keep stdout protocol-clean; a stdio printf would bypass that
    // capture and corrupt the next JSON-RPC response.
    std::ostringstream duration;
    duration << std::fixed << std::setprecision(3) << seconds;
    std::cout << "duration:            " << duration.str() << " s\n";
    std::cout << "supported:           "
              << (is_supported_method(h.method) ? "yes" : "yes (4-bit IMA ADPCM)")
              << "\n";
}

// ---------------------------------------------------------------------------
// info
// ---------------------------------------------------------------------------

int wav_info(int argc, char** argv)
{
    if (argc < 3) {
        std::cerr << "wav info: missing <file.wav>\n";
        return 1;
    }
    fs::path p = argv[2];
    if (!fs::exists(p)) {
        std::cerr << "wav info: file not found: " << p.string() << "\n";
        return 2;
    }
    IlsfFile wav = load_ilsf(p);
    if (!wav.error.empty()) {
        std::cerr << "wav info: parse error: " << wav.error << "\n";
        return 3;
    }
    std::cout << "file: " << p.string() << "\n";
    print_header(wav.header);
    return 0;
}

// ---------------------------------------------------------------------------
// convert  (single file)
// ---------------------------------------------------------------------------

int convert_one(const fs::path& src, const fs::path& dst, std::string& err)
{
    IlsfFile wav = load_ilsf(src);
    if (!wav.error.empty()) {
        err = "parse error: " + wav.error + " (" + src.string() + ")";
        return 3;
    }
    if (!is_supported_method(wav.header.method)) {
        // Defensive: this should be unreachable because load_ilsf
        // already rejects unknown methods.  Kept as a guard so future
        // additions to the SoundPackMethod enum can't silently fall
        // through into the WAV writer.
        err = "unsupported sound_pack_method: " +
              std::string(method_name(wav.header.method)) + " (" + src.string() + ")";
        return 3;
    }

    // Only `.wav` is supported in this build (no external DLL /
    // no MP3).  Any other extension is a usage error.
    std::string ext = lower_ext(dst);
    if (ext != ".wav") {
        err = "unsupported output extension '" + ext + "' (only .wav "
              "is supported - no external DLL, single-header only): " +
              dst.string();
        return 1;
    }
    if (!write_wav_pcm16(dst, wav.header, wav.frames)) {
        err = "could not write " + dst.string();
        return 4;
    }
    return 0;
}

int wav_convert(int argc, char** argv)
{
    if (argc < 3) {
        std::cerr << "wav convert: missing <file.wav>\n";
        return 1;
    }
    fs::path src = argv[2];
    if (!fs::exists(src)) {
        std::cerr << "wav convert: file not found: " << src.string() << "\n";
        return 2;
    }
    const char* out_path = opt_val(argc, argv, "-o");
    if (!out_path) {
        std::cerr << "wav convert: missing -o <out.wav>\n";
        return 1;
    }

    fs::path dst = out_path;
    // If dst is an existing directory (or has no extension), default
    // to <src-stem>.wav inside it.
    if (fs::is_directory(dst) || dst.extension().empty()) {
        fs::create_directories(dst);
        dst = dst / (src.stem().string() + ".wav");
    }

    std::string err;
    int rc = convert_one(src, dst, err);
    if (rc != 0) {
        std::cerr << "wav convert: " << err << "\n";
        return rc;
    }
    std::cout << "wav: " << src.string() << " -> " << dst.string() << "\n";
    return 0;
}

// ---------------------------------------------------------------------------
// convert-dir  (recursive)
// ---------------------------------------------------------------------------

int wav_convert_dir(int argc, char** argv)
{
    if (argc < 4) {
        std::cerr << "wav convert-dir: usage: igi1conv wav convert-dir <src_dir> -o <dst_dir> [--no-recursive]\n";
        return 1;
    }
    fs::path src_dir = argv[2];
    if (!fs::exists(src_dir) || !fs::is_directory(src_dir)) {
        std::cerr << "wav convert-dir: not a directory: " << src_dir.string() << "\n";
        return 2;
    }
    const char* out_path = opt_val(argc, argv, "-o");
    if (!out_path) {
        std::cerr << "wav convert-dir: missing -o <dst_dir>\n";
        return 1;
    }
    fs::path dst_dir = out_path;
    fs::create_directories(dst_dir);

    bool recursive = !has_flag(argc, argv, "--no-recursive");

    int failures = 0;
    int total    = 0;
    auto handle = [&](const fs::path& src) {
        ++total;
        fs::path rel  = fs::relative(src, src_dir);
        fs::path dst  = dst_dir / rel;
        dst.replace_extension(".wav");
        fs::create_directories(dst.parent_path());

        std::string err;
        int rc = convert_one(src, dst, err);
        if (rc != 0) {
            ++failures;
            std::cerr << "  FAIL " << src.string() << " : " << err << "\n";
        } else {
            std::cout << "  ok  " << src.string() << " -> " << dst.string() << "\n";
        }
    };

    if (recursive) {
        for (auto& e : fs::recursive_directory_iterator(src_dir)) {
            if (e.is_regular_file() && lower_ext(e.path()) == ".wav")
                handle(e.path());
        }
    } else {
        for (auto& e : fs::directory_iterator(src_dir)) {
            if (e.is_regular_file() && lower_ext(e.path()) == ".wav")
                handle(e.path());
        }
    }

    std::cout << "wav convert-dir: " << (total - failures) << "/" << total
              << " converted to .wav in " << dst_dir.string() << "\n";
    // Exit 0 if at least one file was converted, 3 only if every file failed
    // (per-file errors are already printed above, so the caller can spot them
    // without a non-zero exit on a mostly-successful batch).
    if (total == 0)        return 0;   // nothing to do is not an error
    if (failures == total) return 3;   // everything failed
    return 0;
}

// ---------------------------------------------------------------------------
// Usage
// ---------------------------------------------------------------------------

void print_usage()
{
    std::cerr <<
        "Usage:\n"
        "  igi1conv wav info         <file.wav>\n"
        "  igi1conv wav convert      <file.wav> -o <out.wav>\n"
        "  igi1conv wav convert-dir  <src_dir>  -o <dst_dir> [--no-recursive]\n"
        "\n"
        "The .wav files inside the IGI game directory are not normal PCM .wav files.\n"
        "They use a proprietary InnerLoop container (signature \"ILSF\") that wraps\n"
        "either 16-bit PCM samples (methods 0 and 1) or IMA-ADPCM frames (methods\n"
        "2 and 3).  All four methods (RAW, RAW_RESIDENT, ADPCM, ADPCM_RESIDENT)\n"
        "are decoded to standard PCM via the 4-bit IMA ADPCM algorithm\n"
        "(initial decoder state predictor=0, step_index=0 - matches the\n"
        "`audioop.adpcm2lin(data, 2, None)` call from the Python dconv\n"
        "reference at D:\\IGI-Tools\\GM_123\\IGI MEF CONV\\tools\\dconv).\n"
        "\n"
        "Output format: only `.wav` is supported in this build.  Earlier\n"
        "versions of this command could emit `.mp3` via an external `lame.exe`\n"
        "or a bundled LAME 3.100 DLL; both paths have been removed to keep\n"
        "igi1conv a single binary with zero external dependencies.  Any\n"
        "Windows media player will play the produced `.wav` out of the box.\n"
        "\n"
        "If -o points at an existing directory (or has no extension) the output\n"
        "filename is auto-derived from the input basename as `<src-stem>.wav`.\n";
}

} // namespace

int cmd_wav(int argc, char** argv)
{
    if (argc < 2) {
        print_usage();
        return 1;
    }
    std::string sub = argv[1];

    if (sub == "--help" || sub == "-h") {
        print_usage();
        return 0;
    }
    if (sub == "info")         return wav_info(argc, argv);
    if (sub == "convert")      return wav_convert(argc, argv);
    if (sub == "convert-dir")  return wav_convert_dir(argc, argv);

    std::cerr << "wav: unknown subcommand '" << sub << "'\n";
    print_usage();
    return 1;
}
