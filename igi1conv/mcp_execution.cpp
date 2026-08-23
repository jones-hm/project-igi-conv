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

class CurrentDirectoryGuard {
public:
    bool Enter(const std::string& requestedDirectory, std::string& errorText) {
        if (requestedDirectory.empty()) return true;

        std::error_code error;
        originalDirectory_ = std::filesystem::current_path(error);
        if (error) {
            errorText = "cannot determine current directory";
            return false;
        }

        const std::filesystem::path requested(requestedDirectory);
        error.clear();
        if (!std::filesystem::is_directory(requested, error) || error) {
            errorText = "working_directory is not an existing directory";
            return false;
        }

        error.clear();
        std::filesystem::current_path(requested, error);
        if (error) {
            // Be explicit about restoring even when changing into the target
            // directory fails.  This keeps a failed MCP request from changing
            // the process-wide cwd seen by a later request.
            std::error_code restoreError;
            std::filesystem::current_path(originalDirectory_, restoreError);
            errorText = restoreError ? "cannot enter working_directory and cannot restore current directory"
                                     : "cannot enter working_directory";
            return false;
        }
        changed_ = true;
        return true;
    }

    bool Restore(std::string& errorText) {
        if (!changed_) return true;
        std::error_code error;
        std::filesystem::current_path(originalDirectory_, error);
        if (error) {
            errorText = "cannot restore working directory";
            return false;
        }
        changed_ = false;
        return true;
    }

    ~CurrentDirectoryGuard() {
        if (changed_) {
            std::error_code ignored;
            std::filesystem::current_path(originalDirectory_, ignored);
        }
    }

private:
    std::filesystem::path originalDirectory_;
    bool changed_ = false;
};

} // namespace

McpExecutionResult ExecuteMcpGameCommand(const std::vector<std::string>& command,
                                         const std::string& workingDirectory) {
    McpExecutionResult result;
    std::ostringstream stdoutStream;
    std::ostringstream stderrStream;

    CurrentDirectoryGuard currentDirectory;
    std::string directoryError;
    if (!currentDirectory.Enter(workingDirectory, directoryError)) {
        result.stderrText = directoryError;
        return result;
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

    if (!currentDirectory.Restore(directoryError)) {
        if (result.exitCode == 0) {
            result.exitCode = 1;
        }
        stderrStream << directoryError;
    }

    result.stdoutText = stdoutStream.str();
    result.stderrText = stderrStream.str();
    return result;
}

} // namespace igi1conv
