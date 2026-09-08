#include "cmd_iff.h"
#include <iostream>
#include <cstring>
#include <string>
#include <filesystem>
#include <cstdlib>
#include <vector>
#include <algorithm>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif
#include "../source/parsers/iff_parser.h"
#include "../source/parsers/iff_to_bef.h"
#include "../source/parsers/iff_decompiler.h"
#include "../source/parsers/iff_writer.h"
#include "../source/parsers/iff_bef.h"

namespace fs = std::filesystem;

static unsigned long current_process_id()
{
#ifdef _WIN32
    return static_cast<unsigned long>(::GetCurrentProcessId());
#else
    return static_cast<unsigned long>(::getpid());
#endif
}

static int iff_info(int argc, char** argv);
static int iff_test(int argc, char** argv);
static int iff_convert(int argc, char** argv);
static int iff_decompile(int argc, char** argv);
static int iff_create(int argc, char** argv);
static int iff_export_gif(int argc, char** argv);
static int iff_rebuild(int argc, char** argv);
static int iff_emit_qsc(int argc, char** argv);

int cmd_iff(int argc, char** argv) {
    if (argc < 2) {
        std::cerr <<
            "Usage: igi1conv iff <command> [args...]\n"
            "\n"
            "Commands:\n"
            "  info        <file.iff>                  Print IFF structure / dump\n"
            "  test        <file.iff>                  Validate parse + skeleton eval\n"
            "  decompile   <file.iff> <out_dir>        Binary IFF -> .IFF text + anims_<id>/\n"
            "  convert     <src.iff|dir> <out_dir>     IFF binary -> .BEF text scripts\n"
            "  create      <dir_with_befs> <out.iff>   .BEF scripts -> IFF binary\n"
            "  rebuild     <src.iff> <out.iff>         IFF -> .BEF -> IFF round trip in one step\n"
            "  emit-qsc    <dir_with_befs> <out.qsc>   Generate the Anims.qsc for a folder of .BEFs\n"
            "  export-gif  <file.iff> <out.gif> [w] [h] [fps]\n"
            "                                           Render IFF animation to animated GIF\n";
        return 1;
    }

    std::string command = argv[1];

    if (command == "info")        return iff_info(argc, argv);
    if (command == "test")        return iff_test(argc, argv);
    if (command == "decompile")   return iff_decompile(argc, argv);
    if (command == "convert")     return iff_convert(argc, argv);
    if (command == "create")      return iff_create(argc, argv);
    if (command == "rebuild")     return iff_rebuild(argc, argv);
    if (command == "emit-qsc")    return iff_emit_qsc(argc, argv);
    if (command == "export-gif")  return iff_export_gif(argc, argv);

    std::cerr << "Unknown iff command: " << command << "\n";
    return 1;
}

// ─── info ────────────────────────────────────────────────────────────────
static int iff_info(int argc, char** argv) {
    if (argc < 3) { std::cerr << "Usage: igi1conv iff info <file.iff>\n"; return 1; }
    std::string file = argv[2];
    std::string outfile = file + ".info.txt";
    for (int i = 3; i < argc; i++)
        if (std::strcmp(argv[i], "-o") == 0 && i + 1 < argc) outfile = argv[++i];
    IFF_Dump(file, outfile);
    std::cout << "Dumped IFF info to " << outfile << "\n";
    return 0;
}

// ─── test ────────────────────────────────────────────────────────────────
static int iff_test(int argc, char** argv) {
    if (argc < 3) { std::cerr << "Usage: igi1conv iff test <file.iff>\n"; return 1; }
    std::string file = argv[2];
    IffFile iff = IFF_Parse(file);
    if (!iff.valid) { std::cerr << "Invalid IFF file.\n"; return 1; }
    std::cout << "Bones: " << iff.skeleton.bone_count
              << "  Clips: " << iff.clips.size() << "\n";
    if (!iff.clips.empty()) {
        const auto& clip = iff.clips[0];
        std::cout << "Clip[0] duration=" << clip.duration << " ms\n";
        for (int i = 0; i < iff.skeleton.bone_count; ++i) {
            if (i == 0 && !clip.root_translations.empty())
                std::cout << "  RootTrans[0].x=" << clip.root_translations[0].pos[0] << "\n";
            if (i < (int)clip.bone_rotations.size() && !clip.bone_rotations[i].empty())
                std::cout << "  Bone[" << i << "].rot[0]=" << clip.bone_rotations[i][0].rot[0] << "\n";
        }
    }
    std::cout << "Test OK.\n";
    return 0;
}

// ─── convert (IFF -> BEF) ───────────────────────────────────────────────
static int iff_convert(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "Usage: igi1conv iff convert <src.iff|dir> <out_dir>\n";
        return 1;
    }
    std::string src = argv[2];
    std::string dst = argv[3];
    if (fs::is_regular_file(src)) {
        if (!ConvertIffToBef(src, dst)) return 3;
    } else if (fs::is_directory(src)) {
        int count = 0;
        for (const auto& entry : fs::directory_iterator(src)) {
            if (!entry.is_regular_file()) continue;
            std::string ext = entry.path().extension().string();
            // Case-insensitive check.
            std::string lext = ext; std::transform(lext.begin(), lext.end(), lext.begin(), ::tolower);
            if (lext == ".iff") {
                if (!ConvertIffToBef(entry.path().string(), dst)) return 3;
                ++count;
            }
        }
        std::cout << "\n Converted: " << count << "\n";
    } else {
        std::cerr << "[ERROR] Source path not found: " << src << "\n";
        return 1;
    }
    return 0;
}

// ─── decompile (IFF -> .IFF text + per-clip IFFs) ───────────────────────
static int iff_decompile(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "Usage: igi1conv iff decompile <file.iff> <out_dir>\n";
        return 1;
    }
    std::string src = argv[2];
    std::string dst = argv[3];
    std::string err;
    if (!igi1conv::IFF_Decompile(src, dst, &err)) {
        std::cerr << "[ERROR] iff decompile: " << err << "\n";
        return 3;
    }
    std::cout << "Decompiled " << src << " into " << dst << "\n";
    return 0;
}

// ─── create (BEF dir -> IFF binary) ────────────────────────────────────
static int create_from_bef_dir(const std::string& srcDir, const std::string& outIff,
                               std::string* err)
{
    if (!fs::is_directory(srcDir)) {
        if (err) *err = "source is not a directory: " + srcDir;
        return 1;
    }
    // Accept either:
    //   (a) the BEF-based output of `iff convert` (a flat directory of
    //       .BEF files), or
    //   (b) the .IFF text + per-anim IFF output of `iff decompile`
    //       (a top-level <basename>.IFF plus an `anims_<basename>/`
    //       subfolder of per-anim .IFF text files).
    std::vector<std::string> befs;
    for (const auto& entry : fs::directory_iterator(srcDir)) {
        if (!entry.is_regular_file()) continue;
        std::string ext = entry.path().extension().string();
        std::string lext = ext; std::transform(lext.begin(), lext.end(), lext.begin(), ::tolower);
        if (lext == ".bef") befs.push_back(entry.path().string());
    }
    if (!befs.empty()) {
        std::sort(befs.begin(), befs.end());
        if (!igi1conv::WriteIffFromBefs(befs, outIff, err)) return 3;
        std::cout << "Wrote " << outIff << " (" << befs.size() << " animations)\n";
        return 0;
    }

    // No BEF files - try the decompile-output layout.  The basename is
    // the first <something>.IFF text file we find in the directory.
    std::string baseName;
    for (const auto& entry : fs::directory_iterator(srcDir)) {
        if (!entry.is_regular_file()) continue;
        std::string ext = entry.path().extension().string();
        std::string lext = ext; std::transform(lext.begin(), lext.end(), lext.begin(), ::tolower);
        if (lext == ".iff") {
            // Skip per-anim files inside an anims_* subfolder.
            if (entry.path().parent_path().filename().string().rfind("anims_", 0) == 0)
                continue;
            baseName = entry.path().stem().string();
            break;
        }
    }
    if (baseName.empty()) {
        if (err) *err = "no .BEF files or <basename>.IFF text in: " + srcDir;
        return 1;
    }
    std::vector<igi1conv::BefFile> clips;
    igi1conv::BefFile skel;
    if (!igi1conv::IFF_LoadDecompiledDir(srcDir, baseName, clips, skel, err)) {
        return 1;
    }
    // Materialise the parsed decompile text back to .BEF files in a
    // scratch dir, then run the standard BEF->IFF writer.
    fs::path scratch = fs::temp_directory_path() /
        ("igi1conv_iff_create_" +
         std::to_string(std::rand()) + "_" +
         std::to_string(current_process_id()));
    fs::remove_all(scratch);
    fs::create_directories(scratch);
    // Write a _skeleton.BEF first so the writer can pick up the bones
    // from the first BEF in the directory.
    if (!skel.bones.empty()) {
        igi1conv::BefFile skelBef = skel;
        skelBef.anim_name = baseName + "_skeleton";
        skelBef.length_ms = 1;
        std::string e;
        if (!igi1conv::WriteBefFile((scratch / "_skeleton.BEF").string(), skelBef, &e)) {
            if (err) *err = "skeleton BEF write failed: " + e;
            return 1;
        }
    }
    for (const auto& b : clips) {
        fs::path p = scratch / (b.anim_name + ".BEF");
        std::string e;
        if (!igi1conv::WriteBefFile(p.string(), b, &e)) {
            if (err) *err = "scratch BEF write failed: " + e;
            return 1;
        }
    }
    std::vector<std::string> scratchBefs;
    for (const auto& entry : fs::directory_iterator(scratch)) {
        if (!entry.is_regular_file()) continue;
        std::string ext = entry.path().extension().string();
        std::string lext = ext; std::transform(lext.begin(), lext.end(), lext.begin(), ::tolower);
        if (lext == ".bef") scratchBefs.push_back(entry.path().string());
    }
    std::sort(scratchBefs.begin(), scratchBefs.end());
    if (scratchBefs.size() > 1 && scratchBefs.front().find("_skeleton") == std::string::npos) {
        auto it = std::find_if(scratchBefs.begin(), scratchBefs.end(),
            [](const std::string& p){ return p.find("_skeleton") != std::string::npos; });
        if (it != scratchBefs.end()) std::iter_swap(scratchBefs.begin(), it);
    }
    // Materialise the parsed decompile text back to .BEF files in a
    // scratch dir, then run the standard BEF->IFF writer.
    bool ok = igi1conv::WriteIffFromBefs(scratchBefs, outIff, err);
    fs::remove_all(scratch);
    if (!ok) return 3;
    std::cout << "Wrote " << outIff << " (" << clips.size()
              << " animations, via decompile output)\n";
    return 0;
}

static int iff_create(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "Usage: igi1conv iff create <dir_with_befs> <out.iff>\n";
        return 1;
    }
    std::string src = argv[2];
    std::string dst = argv[3];
    std::string err;
    int rc = create_from_bef_dir(src, dst, &err);
    if (rc != 0) std::cerr << "[ERROR] iff create: " << err << "\n";
    return rc;
}

// ─── rebuild (IFF -> BEF dir -> IFF in one step) ────────────────────────
static int iff_rebuild(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "Usage: igi1conv iff rebuild <src.iff> <out.iff>\n";
        return 1;
    }
    std::string src = argv[2];
    std::string dst = argv[3];

    fs::path tmp = fs::temp_directory_path() /
                   ("igi1conv_iff_rebuild_" +
                    std::to_string(std::rand()) + "_" +
                    std::to_string(current_process_id()));
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    // Step 1: IFF -> BEF
    if (!ConvertIffToBef(src, tmp.string())) {
        std::cerr << "[ERROR] rebuild: convert step failed\n";
        return 3;
    }
    // Step 2: BEF dir -> IFF
    std::string err;
    int rc = create_from_bef_dir(tmp.string(), dst, &err);
    fs::remove_all(tmp);
    if (rc != 0) {
        std::cerr << "[ERROR] rebuild: create step: " << err << "\n";
        return rc;
    }
    std::cout << "Rebuilt " << src << " -> " << dst << "\n";
    return 0;
}

// ─── emit-qsc (BEF dir -> Anims.qsc) ────────────────────────────────────
static int iff_emit_qsc(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "Usage: igi1conv iff emit-qsc <dir_with_befs> <out.qsc>\n";
        return 1;
    }
    std::string src = argv[2];
    std::string dst = argv[3];
    if (!fs::is_directory(src)) {
        std::cerr << "[ERROR] iff emit-qsc: not a directory: " << src << "\n";
        return 1;
    }
    std::vector<std::string> befs;
    for (const auto& entry : fs::directory_iterator(src)) {
        if (!entry.is_regular_file()) continue;
        std::string ext = entry.path().extension().string();
        std::string lext = ext; std::transform(lext.begin(), lext.end(), lext.begin(), ::tolower);
        if (lext == ".bef") befs.push_back(entry.path().string());
    }
    std::sort(befs.begin(), befs.end());
    if (befs.empty()) {
        std::cerr << "[ERROR] iff emit-qsc: no .BEF files in: " << src << "\n";
        return 1;
    }
    std::string err;
    if (!igi1conv::WriteAnimsQsc(befs, dst, &err)) {
        std::cerr << "[ERROR] iff emit-qsc: " << err << "\n";
        return 3;
    }
    std::cout << "Wrote " << dst << " (" << befs.size() << " CreateAnim calls)\n";
    return 0;
}

// ─── export-gif (IFF -> animated GIF) ───────────────────────────────────
//
// We forward-declare the renderer so this translation unit doesn't
// have to drag in <QImage> / OpenGL headers - the heavy lifting lives
// in iff_gif_exporter.cpp which is the standalone, no-Qt path used by
// the CLI.  See also: cmd_iff in the GUI uses Qt to render the same
// frames interactively.
namespace igi1conv {
bool IFF_ExportGif(const std::string& iffPath,
                   const std::string& gifPath,
                   int width, int height, int fps,
                   std::string* err);
}

static int iff_export_gif(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "Usage: igi1conv iff export-gif <file.iff> <out.gif> [width] [height] [fps]\n";
        std::cerr << "  width,height default 640x480; fps default 30\n";
        return 1;
    }
    std::string src = argv[2];
    std::string dst = argv[3];
    int w = (argc > 4) ? std::atoi(argv[4]) : 640;
    int h = (argc > 5) ? std::atoi(argv[5]) : 480;
    int fps = (argc > 6) ? std::atoi(argv[6]) : 30;
    if (w <= 0 || h <= 0 || fps <= 0) {
        std::cerr << "[ERROR] invalid width/height/fps\n";
        return 1;
    }
    std::string err;
    if (!igi1conv::IFF_ExportGif(src, dst, w, h, fps, &err)) {
        std::cerr << "[ERROR] iff export-gif: " << err << "\n";
        return 3;
    }
    std::cout << "Wrote " << dst << " (" << w << "x" << h << " @ " << fps << "fps)\n";
    return 0;
}
