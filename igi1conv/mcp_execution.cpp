#include "mcp_execution.h"

#include "command_dispatch.h"

#include <filesystem>
#include <iostream>
#include <sstream>

namespace igi1conv {
namespace {

struct StreamCapture {
    explicit StreamCapture(std::ostringstream& stdoutStream, std::ostringstream& stderrStream)
        : oldOut(std::cout.rdbuf(stdoutStream.rdbuf())),
          oldErr(std::cerr.rdbuf(stderrStream.rdbuf())) {}
    ~StreamCapture() {
        std::cout.rdbuf(oldOut);
        std::cerr.rdbuf(oldErr);
    }
    std::streambuf* oldOut;
    std::streambuf* oldErr;
};

} // namespace

McpExecutionResult ExecuteMcpGameCommand(const std::vector<std::string>& command,
                                         const std::string& workingDirectory) {
    McpExecutionResult result;
    std::ostringstream stdoutStream;
    std::ostringstream stderrStream;

    std::error_code error;
    const std::filesystem::path originalDirectory = std::filesystem::current_path(error);
    if (error) {
        result.stderrText = "cannot determine current directory";
        return result;
    }

    if (!workingDirectory.empty()) {
        const std::filesystem::path requested(workingDirectory);
        if (!std::filesystem::is_directory(requested, error)) {
            result.stderrText = "working_directory is not an existing directory";
            return result;
        }
        std::filesystem::current_path(requested, error);
        if (error) {
            result.stderrText = "cannot enter working_directory";
            return result;
        }
    }

    {
        StreamCapture capture(stdoutStream, stderrStream);
        try {
            result.exitCode = RunCommandVector(command);
        } catch (...) {
            result.exitCode = 1;
            stderrStream << "game command failed unexpectedly";
        }
    }

    if (!workingDirectory.empty()) {
        std::error_code restoreError;
        std::filesystem::current_path(originalDirectory, restoreError);
        if (restoreError && result.exitCode == 0) {
            result.exitCode = 1;
            stderrStream << "cannot restore working directory";
        }
    }

    result.stdoutText = stdoutStream.str();
    result.stderrText = stderrStream.str();
    return result;
}

} // namespace igi1conv
