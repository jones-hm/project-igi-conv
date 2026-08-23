#include "pch.h"
#include "command_dispatch.h"
#include "mcp_execution.h"
#include "mcp_transport.h"

#include <charconv>
#include <cstdint>
#include <system_error>
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

namespace {

void PrintMcpHelp() {
    std::cout <<
        "Usage: igi1conv mcp [options]\n"
        "\n"
        "Options:\n"
        "  --transport stdio|http  Transport (default: stdio)\n"
        "  --host <ip|localhost>   HTTP bind address (default: 127.0.0.1)\n"
        "  --port <n>              HTTP port (default: 8765)\n"
        "  --endpoint <path>       HTTP endpoint (default: /mcp)\n"
        "  --auth-token <token>    Required for non-loopback HTTP binds\n"
        "  --origin <origin>       Allow an HTTP Origin (repeatable)\n"
        "  --help                  Show this help\n";
}

bool ParsePort(const std::string& text, std::uint16_t& port) {
    if (text.empty()) return false;
    unsigned int value = 0;
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto parsed = std::from_chars(begin, end, value);
    if (parsed.ec != std::errc() || parsed.ptr != end || value > 65535u)
        return false;
    port = static_cast<std::uint16_t>(value);
    return true;
}

int RunMcpCommand(const std::vector<std::string>& args) {
    igi1conv::McpHttpOptions options;
    std::string transport = "stdio";

    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string& option = args[i];
        if (option == "--help" || option == "-h") {
            PrintMcpHelp();
            return 0;
        }
        if (option == "--transport" || option == "--host" || option == "--port"
            || option == "--endpoint" || option == "--auth-token" || option == "--origin") {
            if (i + 1 >= args.size()) {
                std::cerr << "igi1conv mcp: " << option << " requires a value\n";
                return 1;
            }
            const std::string& value = args[++i];
            if (option == "--transport") {
                transport = value;
            } else if (option == "--host") {
                options.host = value;
            } else if (option == "--port") {
                if (!ParsePort(value, options.port)) {
                    std::cerr << "igi1conv mcp: invalid HTTP port\n";
                    return 1;
                }
            } else if (option == "--endpoint") {
                options.endpoint = value;
            } else if (option == "--auth-token") {
                options.authToken = value;
            } else {
                options.allowedOrigins.push_back(value);
            }
            continue;
        }
        std::cerr << "igi1conv mcp: unknown option '" << option << "'\n";
        return 1;
    }

    if (transport != "stdio" && transport != "http") {
        std::cerr << "igi1conv mcp: --transport must be stdio or http\n";
        return 1;
    }

    igi1conv::McpDispatcher dispatcher(igi1conv::ExecuteMcpGameCommand);
    if (transport == "stdio") return igi1conv::RunMcpStdio(dispatcher);
    return igi1conv::RunMcpHttp(dispatcher, options);
}

} // namespace

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
    if (!args.empty() && args[0] == "mcp")
        return RunMcpCommand(args);
    return igi1conv::RunCommandVector(args);
}
