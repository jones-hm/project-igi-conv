#include "pch.h"
#include "cmd_tex.h"
#include "cmd_mef.h"
#include "cmd_qsc.h"
#include "cmd_qvm.h"
#include "cmd_res.h"
#include "cmd_mtp.h"
#include "cmd_terrain.h"
#include "cmd_graph.h"
#include "cmd_dat.h"
#include "cmd_fnt.h"
#include "cmd_test.h"
#include "cmd_iff.h"
#include "cmd_wav.h"
#include "cmd_olm.h"
#include "cmd_lightmap.h"
//   0 = success
//   1 = bad args
//   2 = file not found
//   3 = parse error
//   4 = write error

#ifndef IGI1CONV_VERSION
#define IGI1CONV_VERSION "1.10.0"
#endif

static void print_help()
{
    std::cout <<
        "igi1conv v" IGI1CONV_VERSION " \xe2\x80\x94 IGI Game Converter\n"
        "\n"
        "Usage: igi1conv <command> [options]\n"
        "\n"
        "Commands:\n"
        "  tex      TEX/SPR/PIC texture operations (decode, info, to-png, to-tga)\n"
        "  mef      MEF 3D mesh operations (export to OBJ, bundle, dump, info)\n"
        "  qsc      QSC QScript (compile to QVM, validate)\n"
        "  qvm      QVM bytecode (decompile to QSC, disasm, info)\n"
        "  res      RES archive (list, extract, compile, pack, unpack)\n"
        "  mtp      MTP terrain properties (dump to JSON, info, to-dat)\n"
        "  terrain  Terrain height/cube data (export-lmp, export-ctr, info)\n"
        "  graph    AI navigation graph (export to JSON, info)\n"
        "  dat      DAT model-texture data (info, export, to-mtp)\n"
        "  fnt      FNT font file (info, export PNG)\n"
        "  iff      IFF skeletal animation format (info, test, decompile, convert, create, rebuild, emit-qsc, export-gif)\n"
        "  wav      IGI audio (ILSF container -> .wav, info, convert, convert-dir)\n"
        "  olm      OLM lightmap operations (info, to-png, to-tga)\n"
        "  lightmap Lightmap binding resolution (list, resolve by task-id/position)\n"
        "  test     Run advanced test suite on game directory\n"
        "\n"
        "Run 'igi1conv <command> --help' for command-specific help.\n";
}

#ifdef IGI1CONV_WITH_GUI
#include "gui_main.h"
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

int main(int argc, char** argv)
{
    if (argc < 2)
    {
#ifdef IGI1CONV_WITH_GUI
#ifdef _WIN32
        HWND consoleWnd = GetConsoleWindow();
        if (consoleWnd) {
            ShowWindow(consoleWnd, SW_HIDE);
        }
#endif
        return run_gui();
#else
        print_help();
        return 1;
#endif
    }

    if (argc == 2 && std::string(argv[1]) == "--gui")
    {
#ifdef IGI1CONV_WITH_GUI
        return run_gui();
#else
        std::cerr << "igi1conv: this build does not include the GUI\n";
        return 1;
#endif
    }

    std::string cmd = argv[1];

    if (cmd == "--help" || cmd == "-h")
    {
        print_help();
        return 0;
    }

    if (cmd == "--version" || cmd == "-v")
    {
        std::cout << "igi1conv version " IGI1CONV_VERSION "\n";
        return 0;
    }

    // Shift argv so each handler receives its own argc/argv starting at argv[0] = command name
    int sub_argc = argc - 1;
    char** sub_argv = argv + 1;

    if (cmd == "tex")     return cmd_tex(sub_argc, sub_argv);
    if (cmd == "mef")     return cmd_mef(sub_argc, sub_argv);
    if (cmd == "mex")     return cmd_mef(sub_argc, sub_argv);
    if (cmd == "qsc")     return cmd_qsc(sub_argc, sub_argv);
    if (cmd == "qvm")     return cmd_qvm(sub_argc, sub_argv);
    if (cmd == "res")     return cmd_res(sub_argc, sub_argv);
    if (cmd == "mtp")     return cmd_mtp(sub_argc, sub_argv);
    if (cmd == "terrain") return cmd_terrain(sub_argc, sub_argv);
    if (cmd == "graph")   return cmd_graph(sub_argc, sub_argv);
    if (cmd == "dat")     return cmd_dat(sub_argc, sub_argv);
    if (cmd == "fnt")     return cmd_fnt(sub_argc, sub_argv);
    if (cmd == "iff")     return cmd_iff(sub_argc, sub_argv);
    if (cmd == "wav")     return cmd_wav(sub_argc, sub_argv);
    if (cmd == "olm")     return cmd_olm(sub_argc, sub_argv);
    if (cmd == "lightmap") return cmd_lightmap(sub_argc, sub_argv);
    if (cmd == "test")    return cmd_test(sub_argc, sub_argv);

    std::cerr << "igi1conv: unknown command '" << cmd << "'\n";
    std::cerr << "Run 'igi1conv --help' for usage.\n";
    return 1;
}
