#include "pch.h"
#include "cmd_olm.h"
#include "../../third_party/tinygltf/stb_image_write.h"
#include "../../third_party/tinygltf/stb_image.h"
#include <filesystem>
#include <fstream>
#include <vector>
#include <cstring>

namespace fs = std::filesystem;

static void print_olm_help()
{
    std::cout <<
        "Usage: igi1conv olm <subcommand> [options]\n"
        "\n"
        "Subcommands:\n"
        "  info     <input.olm>\n"
        "  to-png   <input.olm> [-o <out.png>]\n"
        "  to-tga   <input.olm> [-o <out.tga>]\n"
        "  from-png <input.png> -o <out.olm> [--template <ref.olm>]\n"
        "           Build an .olm from a PNG. Dimensions come from the PNG;\n"
        "           --template copies the runtime header fields (uv scale,\n"
        "           version, date) from an existing .olm so the rebuilt file\n"
        "           matches the original's metadata.\n"
        "\n"
        "Exit codes: 0=success 1=bad args 2=file not found 3=parse error 4=write error\n";
}

OLMFile ParseOlm(const std::string& path) {
    OLMFile olm;
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        olm.error = "Failed to open file";
        return olm;
    }

    file.read(reinterpret_cast<char*>(&olm.header), sizeof(OlmMainHeader));
    if (file.gcount() != sizeof(OlmMainHeader)) {
        olm.error = "Failed to read main header";
        return olm;
    }

    if (olm.header.version1 < 0.11f || olm.header.version1 > 0.13f) {
        olm.error = "Invalid version1";
        return olm;
    }

    file.read(reinterpret_cast<char*>(&olm.layer), sizeof(OlmLayerDescriptor));
    if (file.gcount() != sizeof(OlmLayerDescriptor)) {
        olm.error = "Failed to read layer descriptor";
        return olm;
    }

    uint32_t numPixels = olm.layer.pixel_width * olm.layer.pixel_height;
    olm.pixels.resize(numPixels);
    file.read(reinterpret_cast<char*>(olm.pixels.data()), numPixels * sizeof(OlmPixel));
    if (file.gcount() != numPixels * sizeof(OlmPixel)) {
        olm.error = "Failed to read pixel data";
        return olm;
    }

    olm.valid = true;
    return olm;
}

static int do_olm_info(const std::string& input)
{
    if (!fs::exists(input))
    {
        std::cerr << "olm: file not found: " << input << "\n";
        return 2;
    }

    OLMFile olm = ParseOlm(input);
    if (!olm.valid)
    {
        std::cerr << "olm: parse error: " << olm.error << "\n";
        return 3;
    }

    std::cout << "file:       " << input << "\n";
    std::cout << "version1:   " << olm.header.version1 << "\n";
    std::cout << "version2:   " << olm.header.version2 << "\n";
    std::cout << "date:       " << olm.header.year << "-" << olm.header.month << "-" << olm.header.day << " " 
              << olm.header.hour << ":" << olm.header.minute << ":" << olm.header.second << "\n";
    std::cout << "grid:       " << olm.header.width << "x" << olm.header.height << "\n";
    std::cout << "uv_scale:   " << olm.header.uv_scale_u << ", " << olm.header.uv_scale_v << "\n";
    std::cout << "resolution: " << olm.layer.pixel_width << "x" << olm.layer.pixel_height << "\n";
    std::cout << "pixels:     " << olm.pixels.size() << "\n";
    return 0;
}

static void SwapChannels(std::vector<OlmPixel>& pixels) {
    // Swap R and B to match BGRA target
    for (auto& p : pixels) {
        std::swap(p.r, p.b);
    }
}

bool WriteOlm(const std::string& path, const OLMFile& olm, std::string& err) {
    if (olm.layer.pixel_width == 0 || olm.layer.pixel_height == 0) {
        err = "refusing to write zero-sized OLM";
        return false;
    }
    const size_t expected = static_cast<size_t>(olm.layer.pixel_width) * olm.layer.pixel_height;
    if (olm.pixels.size() != expected) {
        err = "pixel count " + std::to_string(olm.pixels.size()) +
              " does not match " + std::to_string(olm.layer.pixel_width) + "x" +
              std::to_string(olm.layer.pixel_height) + " (" + std::to_string(expected) + ")";
        return false;
    }
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f.is_open()) { err = "cannot open output for writing: " + path; return false; }

    f.write(reinterpret_cast<const char*>(&olm.header), sizeof(OlmMainHeader));
    f.write(reinterpret_cast<const char*>(&olm.layer), sizeof(OlmLayerDescriptor));
    f.write(reinterpret_cast<const char*>(olm.pixels.data()),
            static_cast<std::streamsize>(olm.pixels.size() * sizeof(OlmPixel)));
    if (!f.good()) { err = "write error on: " + path; return false; }
    return true;
}

OLMFile BuildOlmFromRGBA(const uint8_t* rgba, uint16_t width, uint16_t height,
                         const OLMFile* templateOlm) {
    OLMFile olm;
    if (templateOlm && templateOlm->valid) {
        // Inherit the original runtime header/layer verbatim so a rebuilt file
        // keeps the same uv scale, version, date, runtime pointers AND the
        // grid/block fields (header.width/height) which are NOT the pixel
        // resolution. Only the layer pixel dimensions track the actual image.
        olm.header = templateOlm->header;
        olm.layer  = templateOlm->layer;
        olm.layer.pixel_width  = width;
        olm.layer.pixel_height = height;
    } else {
        std::memset(&olm.header, 0, sizeof(olm.header));
        std::memset(&olm.layer, 0, sizeof(olm.layer));
        // Minimal valid IGI1 single-layer header (see Lightmap_docs.md §2.3-2.4).
        olm.header.version1 = 0.12f;
        olm.header.version2 = 0.10f;
        olm.header.count1 = 1;
        olm.header.layer_count = 1;
        olm.header.width  = width;   // best-guess grid == pixel dims with no template
        olm.header.height = height;
        olm.header.format = 3; // RGBA
        olm.header.uv_scale_u = 1.0f;
        olm.header.uv_scale_v = 1.0f;
        olm.layer.pixel_width  = width;
        olm.layer.pixel_height = height;
    }

    const size_t count = static_cast<size_t>(width) * height;
    olm.pixels.resize(count);
    for (size_t i = 0; i < count; ++i) {
        // Image order is R,G,B,A; OLM stores with R/B swapped (BGRA on export),
        // so swap back here to land in native OLM channel order.
        olm.pixels[i].r = rgba[i * 4 + 2];
        olm.pixels[i].g = rgba[i * 4 + 1];
        olm.pixels[i].b = rgba[i * 4 + 0];
        olm.pixels[i].a = rgba[i * 4 + 3];
    }
    olm.valid = true;
    return olm;
}

static int do_olm_from_png(const std::string& input, const std::string& outpath,
                           const std::string& templatePath)
{
    if (!fs::exists(input)) {
        std::cerr << "olm: file not found: " << input << "\n";
        return 2;
    }
    if (outpath.empty()) {
        std::cerr << "olm: from-png requires -o <out.olm>\n";
        return 1;
    }

    int w = 0, h = 0, channels = 0;
    unsigned char* pixels = stbi_load(input.c_str(), &w, &h, &channels, 4);
    if (!pixels) {
        std::cerr << "olm: failed to read PNG: " << input << " (" << stbi_failure_reason() << ")\n";
        return 3;
    }

    OLMFile templateOlm;
    if (!templatePath.empty()) {
        templateOlm = ParseOlm(templatePath);
        if (!templateOlm.valid) {
            std::cerr << "olm: warning: template parse failed (" << templateOlm.error
                       << "), using default header\n";
        }
    }

    OLMFile out = BuildOlmFromRGBA(pixels, static_cast<uint16_t>(w), static_cast<uint16_t>(h),
                                   templateOlm.valid ? &templateOlm : nullptr);
    stbi_image_free(pixels);

    std::string err;
    if (!WriteOlm(outpath, out, err)) {
        std::cerr << "olm: write failed: " << err << "\n";
        return 4;
    }
    std::cout << "olm: wrote " << outpath << " (" << w << "x" << h << ")\n";
    return 0;
}

static int do_olm_convert(const std::string& input, std::string outpath, const std::string& format)
{
    if (!fs::exists(input))
    {
        std::cerr << "olm: file not found: " << input << "\n";
        return 2;
    }

    OLMFile olm = ParseOlm(input);
    if (!olm.valid)
    {
        std::cerr << "olm: parse error: " << olm.error << "\n";
        return 3;
    }

    if (outpath.empty()) {
        fs::path p(input);
        p.replace_extension(format);
        outpath = p.string();
    }

    SwapChannels(olm.pixels); // Swap R and B

    int res = 0;
    if (format == "png") {
        res = stbi_write_png(outpath.c_str(), olm.layer.pixel_width, olm.layer.pixel_height, 4, olm.pixels.data(), olm.layer.pixel_width * 4);
    } else if (format == "tga") {
        res = stbi_write_tga(outpath.c_str(), olm.layer.pixel_width, olm.layer.pixel_height, 4, olm.pixels.data());
    }

    if (res == 0) {
        std::cerr << "olm: failed to write " << format << " file: " << outpath << "\n";
        return 4;
    }

    std::cout << "olm: wrote " << outpath << "\n";
    return 0;
}

int cmd_olm(int argc, char** argv)
{
    if (argc < 2)
    {
        print_olm_help();
        return 1;
    }

    std::string subcmd = argv[1];

    if (subcmd == "info")
    {
        if (argc < 3) { print_olm_help(); return 1; }
        return do_olm_info(argv[2]);
    }
    else if (subcmd == "to-png" || subcmd == "to-tga")
    {
        if (argc < 3) { print_olm_help(); return 1; }
        std::string input = argv[2];
        std::string outpath = "";
        
        for (int i = 3; i < argc; ++i)
        {
            std::string arg = argv[i];
            if (arg == "-o" && i + 1 < argc)
            {
                outpath = argv[++i];
            }
        }
        return do_olm_convert(input, outpath, subcmd == "to-png" ? "png" : "tga");
    }
    else if (subcmd == "from-png")
    {
        if (argc < 3) { print_olm_help(); return 1; }
        std::string input = argv[2];
        std::string outpath = "";
        std::string templatePath = "";
        for (int i = 3; i < argc; ++i)
        {
            std::string arg = argv[i];
            if (arg == "-o" && i + 1 < argc) {
                outpath = argv[++i];
            } else if (arg == "--template" && i + 1 < argc) {
                templatePath = argv[++i];
            }
        }
        return do_olm_from_png(input, outpath, templatePath);
    }

    std::cerr << "olm: unknown subcommand '" << subcmd << "'\n";
    print_olm_help();
    return 1;
}
