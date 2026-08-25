#include "mcp_execution.h"

#include "command_dispatch.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <streambuf>
#include <string>

namespace igi1conv {
namespace {

constexpr std::size_t kMaxCapturedOutputBytes = 1024 * 1024;

class LimitedStreamBuf final : public std::streambuf {
public:
    std::string text() const { return text_; }
    bool truncated() const { return truncated_; }

protected:
    int_type overflow(int_type character = traits_type::eof()) override {
        if (traits_type::eq_int_type(character, traits_type::eof()))
            return traits_type::not_eof(character);
        append(character_type(character));
        return character;
    }

    std::streamsize xsputn(const char* data, std::streamsize count) override {
        if (count <= 0) return 0;
        const std::size_t available = kMaxCapturedOutputBytes - text_.size();
        const std::size_t requested = static_cast<std::size_t>(count);
        const std::size_t copied = std::min(available, requested);
        text_.append(data, copied);
        if (copied != requested) truncated_ = true;
        return count;
    }

private:
    using character_type = char;

    void append(character_type character) {
        if (text_.size() < kMaxCapturedOutputBytes)
            text_.push_back(character);
        else
            truncated_ = true;
    }

    std::string text_;
    bool truncated_ = false;
};

struct StreamCapture {
    explicit StreamCapture(LimitedStreamBuf& stdoutBuffer, LimitedStreamBuf& stderrBuffer)
        : oldOut(std::cout.rdbuf(&stdoutBuffer)),
          oldErr(std::cerr.rdbuf(&stderrBuffer)) {}
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
    LimitedStreamBuf stdoutBuffer;
    LimitedStreamBuf stderrBuffer;

    CurrentDirectoryGuard currentDirectory;
    std::string directoryError;
    if (!currentDirectory.Enter(workingDirectory, directoryError)) {
        result.stderrText = directoryError;
        return result;
    }

    {
        StreamCapture capture(stdoutBuffer, stderrBuffer);
        try {
            result.exitCode = RunCommandVector(command);
        } catch (...) {
            result.exitCode = 1;
            const char message[] = "game command failed unexpectedly";
            stderrBuffer.sputn(message, static_cast<std::streamsize>(sizeof(message) - 1));
        }
    }

    if (!currentDirectory.Restore(directoryError)) {
        if (result.exitCode == 0) {
            result.exitCode = 1;
        }
        const std::string message = directoryError;
        stderrBuffer.sputn(message.data(), static_cast<std::streamsize>(message.size()));
    }

    result.stdoutText = stdoutBuffer.text();
    result.stderrText = stderrBuffer.text();
    if (stdoutBuffer.truncated()) result.stdoutText += "\n[output truncated at 1048576 bytes]";
    if (stderrBuffer.truncated()) result.stderrText += "\n[output truncated at 1048576 bytes]";
    return result;
}

} // namespace igi1conv
