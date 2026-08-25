#include "pch.h"

#include "command_dispatch.h"

#include "cmd_dat.h"
#include "cmd_fnt.h"
#include "cmd_graph.h"
#include "cmd_iff.h"
#include "cmd_lightmap.h"
#include "cmd_mef.h"
#include "cmd_mtp.h"
#include "cmd_olm.h"
#include "cmd_qsc.h"
#include "cmd_qvm.h"
#include "cmd_res.h"
#include "cmd_terrain.h"
#include "cmd_test.h"
#include "cmd_tex.h"
#include "cmd_wav.h"

#include "mcp_operations.h"

#include <stdexcept>

namespace igi1conv {
namespace {

#ifndef IGI1CONV_VERSION
#define IGI1CONV_VERSION "1.11.0"
#endif

} // namespace

void PrintMainHelp() {
    std::cout <<
        "igi1conv v" IGI1CONV_VERSION " \xe2\x80\x94 IGI Game Converter\n"
        "\n"
        "Usage: igi1conv <command> [options]\n"
        "\n"
        "Commands:\n"
        "  tex      TEX/SPR/PIC texture operations (decode, info, to-png, to-tga)\n"
        "  mef      MEF 3D mesh operations (export to OBJ, bundle, dump, info)\n"
        "  qsc      QSC QScript (compile to QVM, validate, edit objects)\n"
        "  qvm      QVM bytecode (decompile to QSC, disasm, info)\n"
        "  res      RES archive (list, extract, compile, pack, unpack)\n"
        "  mtp      MTP terrain properties (dump to JSON, info, sync, to-dat)\n"
        "  terrain  Terrain height/cube data (export-lmp, export-ctr, info)\n"
        "  graph    AI navigation graph (export to JSON, info, dump, table)\n"
        "  dat      DAT model-texture data (info, export, to-mtp)\n"
        "  fnt      FNT font file (info, export PNG)\n"
        "  iff      IFF skeletal animation format (info, test, decompile, convert, create, rebuild, emit-qsc, export-gif)\n"
        "  wav      IGI audio (ILSF container -> .wav, info, convert, convert-dir)\n"
        "  olm      OLM lightmap operations (info, to-png, to-tga, from-png)\n"
        "  lightmap Lightmap binding resolution and recalculation\n"
        "  test     Run advanced test suite on game directory\n"
        "  mcp      Start the Model Context Protocol server\n"
        "\n"
        "Run 'igi1conv <command> --help' for command-specific help.\n";
}

int RunCommandVector(const std::vector<std::string>& args) {
    if (args.empty())
        return 1;

    if (args[0] == "--help" || args[0] == "-h") {
        PrintMainHelp();
        return 0;
    }

    if (args[0] == "--version" || args[0] == "-v") {
        std::cout << "igi1conv version " IGI1CONV_VERSION "\n";
        return 0;
    }

    std::vector<std::string> mutableArgs = args;
    std::vector<char*> argv;
    argv.reserve(mutableArgs.size() + 1);
    for (auto& arg : mutableArgs)
        argv.push_back(arg.data());
    argv.push_back(nullptr);
    const int argc = static_cast<int>(mutableArgs.size());

    if (args[0] == "tex")     return cmd_tex(argc, argv.data());
    if (args[0] == "mef")     return cmd_mef(argc, argv.data());
    if (args[0] == "mex")     return cmd_mef(argc, argv.data());
    if (args[0] == "qsc")     return cmd_qsc(argc, argv.data());
    if (args[0] == "qvm")     return cmd_qvm(argc, argv.data());
    if (args[0] == "res")     return cmd_res(argc, argv.data());
    if (args[0] == "mtp")     return cmd_mtp(argc, argv.data());
    if (args[0] == "terrain") return cmd_terrain(argc, argv.data());
    if (args[0] == "graph")   return cmd_graph(argc, argv.data());
    if (args[0] == "dat")     return cmd_dat(argc, argv.data());
    if (args[0] == "fnt")     return cmd_fnt(argc, argv.data());
    if (args[0] == "iff")     return cmd_iff(argc, argv.data());
    if (args[0] == "wav")     return cmd_wav(argc, argv.data());
    if (args[0] == "olm")     return cmd_olm(argc, argv.data());
    if (args[0] == "lightmap") return cmd_lightmap(argc, argv.data());
    if (args[0] == "test")    return cmd_test(argc, argv.data());

    std::cerr << "igi1conv: unknown command '" << args[0] << "'\n";
    std::cerr << "Run 'igi1conv --help' for usage.\n";
    return 1;
}

} // namespace igi1conv
