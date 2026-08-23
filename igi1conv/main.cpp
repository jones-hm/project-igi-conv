#include "pch.h"
#include "command_dispatch.h"
//   0 = success
//   1 = bad args
//   2 = file not found
//   3 = parse error
//   4 = write error

#include "gui_main.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

int main(int argc, char** argv)
{
    if (argc < 2 || (argc == 2 && std::string(argv[1]) == "--gui"))
    {
#ifdef _WIN32
        HWND consoleWnd = GetConsoleWindow();
        if (consoleWnd) {
            ShowWindow(consoleWnd, SW_HIDE);
        }
#endif
        return run_gui();
    }

    std::vector<std::string> args;
    args.reserve(static_cast<size_t>(argc - 1));
    for (int i = 1; i < argc; ++i)
        args.emplace_back(argv[i]);
    return igi1conv::RunCommandVector(args);
}
