// igi1conv_test_util.h — self-contained helpers for the igi1conv CLI test suite.
//
// The suite spawns the freshly built igi1conv.exe (next to igi1conv_tests.exe) and
// drives it against a corpus of real IGI game files.  The corpus directory is
// provided at runtime via:
//   - the command-line flag  --game-path=PATH  (parsed by the test main)
//   - or the IGI_GAME_PATH environment variable
// If neither is set, FindCorpusFileByRegex() returns "" and any test that
// calls IGI1CONV_NEED() will GTEST_SKIP() so the suite stays green on
// machines without a corpus.  NO path is hard-coded here.
#pragma once

#include <gtest/gtest.h>
#include <string>
#include <vector>
#include <filesystem>
#include <fstream>
#include <regex>
#include <random>
#include <algorithm>
#include <chrono>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wincrypt.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#include <mach-o/dyld.h>
#endif
#endif

namespace igi1conv_test {

// Set at runtime by the test main when --game-path=PATH is passed, or
// pulled from the IGI_GAME_PATH env var as a fallback.  Never defaults
// to a hard-coded path - tests must opt in to a corpus.
extern std::string g_game_path;

inline void SetGamePath(const std::string& p) { g_game_path = p; }

// Directory of the running test executable (igi1conv.exe lives alongside it).
inline std::string ExeDir() {
#ifdef _WIN32
    char buf[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::filesystem::path p(buf);
#elif defined(__APPLE__)
    uint32_t size = 1024;
    std::vector<char> buf(size);
    if (_NSGetExecutablePath(buf.data(), &size) != 0) buf.resize(size);
    if (_NSGetExecutablePath(buf.data(), &size) != 0) return ".";
    std::filesystem::path p = std::filesystem::weakly_canonical(buf.data());
#else
    std::vector<char> buf(4096);
    const ssize_t length = readlink("/proc/self/exe", buf.data(), buf.size() - 1);
    if (length <= 0) return ".";
    buf[static_cast<size_t>(length)] = '\0';
    std::filesystem::path p(buf.data());
#endif
    return p.parent_path().string();
}

inline std::string IGI1ConvExe() {
#ifdef _WIN32
    return (std::filesystem::path(ExeDir()) / "igi1conv.exe").string();
#else
    return (std::filesystem::path(ExeDir()) / "igi1conv").string();
#endif
}

// Root of the test corpus.  Returns "" if no path was provided - in
// that case tests that call IGI1CONV_NEED() will GTEST_SKIP().
inline const std::string& CorpusDir() {
    // Lazy: pull env var on first call (before the g_game_path override
    // is set, e.g. when IGI1CONV_NEED is evaluated in a static init).
    static const std::string s_envPath = []() -> std::string {
#ifdef _WIN32
        char* env = nullptr;
        size_t len = 0;
        if (_dupenv_s(&env, &len, "IGI_GAME_PATH") == 0 && env) {
            std::string v(env);
            free(env);
            return v;
        }
#else
        if (const char* env = std::getenv("IGI_GAME_PATH")) return env;
#endif
        return std::string();
    }();
    return g_game_path.empty() ? s_envPath : g_game_path;
}

inline std::string Corpus(const std::string& rel) {
    return (std::filesystem::path(CorpusDir()) / rel).string();
}

// Temp directory scoped to a single test, auto-removed on destruction.
class TempDir {
public:
    TempDir() {
        std::filesystem::path workspace_temp = std::filesystem::current_path() / "tests_temp";
        std::filesystem::create_directories(workspace_temp);
        static unsigned counter = 0;
        path_ = (workspace_temp / ("igi1conv_test_" +
                std::to_string(
#ifdef _WIN32
                    GetCurrentProcessId()
#else
                    getpid()
#endif
                ) + "_" +
                std::to_string(++counter))).string();
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    std::string operator/(const std::string& name) const {
        return (std::filesystem::path(path_) / name).string();
    }
    const std::string& str() const { return path_; }
private:
    std::string path_;
};

// Run igi1conv.exe with the given (already-quoted where needed) argument string.
// If captureOut is non-null, the child's stdout+stderr are written there.
// Returns the process exit code, or -1 if the process could not be started.
inline int RunIGI1Conv(const std::string& args, std::string* captureOut = nullptr,
                    uint32_t timeoutMs = 30000) {
    const std::string exePath = IGI1ConvExe();
    std::string cmdLine = "\"" + exePath + "\" " + args;

#ifdef _WIN32
    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };

    // Redirect child output to a temp file (file, not pipe, to avoid the
    // classic full-buffer deadlock without a reader thread).
    std::string outFile;
    HANDLE hOut = INVALID_HANDLE_VALUE;
    if (captureOut) {
        std::filesystem::path workspace_temp = std::filesystem::current_path() / "tests_temp";
        std::filesystem::create_directories(workspace_temp);
        static unsigned file_counter = 0;
        outFile = (workspace_temp / ("gcv_out_" + std::to_string(GetCurrentProcessId()) + "_" + std::to_string(++file_counter) + ".tmp")).string();
        hOut = CreateFileA(outFile.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                           &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    } else {
        hOut = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_WRITE,
                           &sa, OPEN_EXISTING, 0, nullptr);
    }

    STARTUPINFOA si = {};
    si.cb          = sizeof(si);
    si.dwFlags     = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput  = hOut;
    si.hStdError   = hOut;
    si.hStdInput   = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi = {};
    std::vector<char> buf(cmdLine.begin(), cmdLine.end());
    buf.push_back('\0');

    BOOL ok = CreateProcessA(nullptr, buf.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (hOut != INVALID_HANDLE_VALUE) CloseHandle(hOut);
    if (!ok) {
        if (!outFile.empty()) DeleteFileA(outFile.c_str());
        return -1;
    }

    DWORD wait = WaitForSingleObject(pi.hProcess, timeoutMs);
    DWORD code = 1;
    if (wait == WAIT_OBJECT_0)
        GetExitCodeProcess(pi.hProcess, &code);
    else
        TerminateProcess(pi.hProcess, 1);

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (captureOut && !outFile.empty()) {
        std::ifstream f(outFile, std::ios::binary);
        captureOut->assign(std::istreambuf_iterator<char>(f),
                           std::istreambuf_iterator<char>());
        f.close();
        DeleteFileA(outFile.c_str());
    }
    return static_cast<int>(code);
#else
    std::filesystem::path workspace_temp = std::filesystem::current_path() / "tests_temp";
    std::filesystem::create_directories(workspace_temp);
    static unsigned file_counter = 0;
    const std::string outFile = (workspace_temp / (
        "gcv_out_" + std::to_string(getpid()) + "_" +
        std::to_string(++file_counter) + ".tmp")).string();

    const pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        const int output = open(captureOut ? outFile.c_str() : "/dev/null",
            O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (output < 0) _exit(127);
        dup2(output, STDOUT_FILENO);
        dup2(output, STDERR_FILENO);
        close(output);
        execl("/bin/sh", "sh", "-c", cmdLine.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }

    int status = 0;
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(timeoutMs);
    while (waitpid(pid, &status, WNOHANG) == 0) {
        if (std::chrono::steady_clock::now() >= deadline) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            status = 1 << 8;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (captureOut) {
        std::ifstream f(outFile, std::ios::binary);
        captureOut->assign(std::istreambuf_iterator<char>(f),
                           std::istreambuf_iterator<char>());
    }
    std::error_code ec;
    std::filesystem::remove(outFile, ec);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
#endif
}

inline std::string Q(const std::string& s) { return "\"" + s + "\""; }

inline bool NonEmptyFile(const std::string& p) {
    std::error_code ec;
    return std::filesystem::exists(p) && std::filesystem::file_size(p, ec) > 0;
}

// Find a file in the corpus matching the given regex pattern (case-insensitive).
inline std::string FindCorpusFileByRegex(const std::string& pattern_str) {
    std::string dir = CorpusDir();
    if (!std::filesystem::exists(dir)) return "";
    
    std::regex pattern(pattern_str, std::regex_constants::icase);

    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        
        std::string filename = entry.path().filename().string();
        if (std::regex_search(filename, pattern)) {
            // Skip text-based .IFF files (decompiled output, not binary).
            // Binary IFF files never start with \r\n\\  while decompiled
            // text IFF files do (they begin with a human-readable header).
            std::string ext = entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext == ".iff" || ext == ".bff") {
                // Read the first 4 bytes; if they look like text content
                // (\r\n\\ or \n\\  etc.) skip this candidate.
                std::ifstream f(entry.path().string(), std::ios::binary);
                char hdr[4] = {};
                if (f.read(hdr, 4) && (hdr[0] == '\r' || hdr[0] == '\n')) {
                    continue;
                }
            }
            return entry.path().string();
        }
    }
    return "";
}

// Run `mef info` on the given MEF and return the parsed model_type
// (0 = rigid, 1 = bone, 3 = lightmap).  Returns -1 on any failure.
inline int GetMefModelType(const std::string& mefPath) {
    if (mefPath.empty()) return -1;
    std::string out;
    if (RunIGI1Conv("mef info \"" + mefPath + "\"", &out) != 0) return -1;
    auto pos = out.find("model_type:");
    if (pos == std::string::npos) return -1;
    int v = -1;
    if (std::sscanf(out.c_str() + pos, "model_type: %d", &v) != 1) return -1;
    return v;
}

// Find a MEF file in the corpus whose model_type matches `wantedType`.
// Returns "" if no such MEF is found.  Search order:
//   1. Files in the corpus that match `namePattern` AND have the right
//      model_type are returned (so callers can scope by filename).
//   2. If `namePattern` is empty, all *.mef files in the corpus are
//      scanned.
// The first 64 files matching the name pattern are inspected (so
// tests stay fast even on a large corpus).
inline std::string FindCorpusMefOfModelType(int wantedType,
                                            const std::string& namePattern = "") {
    std::string dir = CorpusDir();
    if (!std::filesystem::exists(dir)) return "";

    std::regex pat;
    bool useNameFilter = !namePattern.empty();
    if (useNameFilter) {
        pat = std::regex(namePattern, std::regex_constants::icase);
    }

    int inspected = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        std::string filename = entry.path().filename().string();
        // Case-insensitive .mef extension check.
        if (filename.size() < 4) continue;
        std::string ext = filename.substr(filename.size() - 4);
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c){ return std::tolower(c); });
        if (ext != ".mef") continue;

        if (useNameFilter && !std::regex_search(filename, pat)) continue;
        if (++inspected > 64) break;  // cap scan time

        if (GetMefModelType(entry.path().string()) == wantedType) {
            return entry.path().string();
        }
    }
    return "";
}

// Parse the V coordinate of the first non-(0,0) `vt` line in an OBJ.
// Returns true on success.
inline bool FirstVtFromObj(const std::string& objPath, float& u, float& v) {
    std::ifstream in(objPath);
    if (!in.is_open()) return false;
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("vt ", 0) != 0) continue;
        if (std::sscanf(line.c_str(), "vt %f %f", &u, &v) != 2) continue;
        if (u != 0.0f || v != 0.0f) return true;
    }
    return false;
}

extern bool g_test_qvm;
extern bool g_test_res;
extern bool g_test_mtp;
extern bool g_test_dat;
extern bool g_test_spr;
extern bool g_test_tex;
extern bool g_test_pic;

// Get SHA256 hex string of a file using Windows Crypto API
inline std::string GetFileSHA256(const std::string& filepath) {
    std::ifstream f(filepath, std::ios::binary);
    if (!f.is_open()) return "";
    
#ifdef _WIN32
    HCRYPTPROV hProv = 0;
    HCRYPTHASH hHash = 0;
    
    if (!CryptAcquireContext(&hProv, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) {
        return "";
    }
    
    if (!CryptCreateHash(hProv, CALG_SHA_256, 0, 0, &hHash)) {
        CryptReleaseContext(hProv, 0);
        return "";
    }
    
    char buffer[8192];
    while (f.good()) {
        f.read(buffer, sizeof(buffer));
        std::streamsize bytes = f.gcount();
        if (bytes > 0) {
            CryptHashData(hHash, reinterpret_cast<const BYTE*>(buffer), static_cast<DWORD>(bytes), 0);
        }
    }
    
    DWORD hashLen = 32;
    BYTE hash[32];
    std::string result = "";
    
    if (CryptGetHashParam(hHash, HP_HASHVAL, hash, &hashLen, 0)) {
        char hex[65];
        for (DWORD i = 0; i < hashLen; ++i) {
            snprintf(hex + (i * 2), 3, "%02x", hash[i]);
        }
        result = hex;
    }
    
    CryptDestroyHash(hHash);
    CryptReleaseContext(hProv, 0);
    return result;
#elif defined(__APPLE__)
    CC_SHA256_CTX context;
    CC_SHA256_Init(&context);
    char buffer[8192];
    while (f.good()) {
        f.read(buffer, sizeof(buffer));
        const std::streamsize bytes = f.gcount();
        if (bytes > 0) CC_SHA256_Update(&context, buffer, static_cast<CC_LONG>(bytes));
    }
    unsigned char hash[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256_Final(hash, &context);
    char hex[CC_SHA256_DIGEST_LENGTH * 2 + 1] = {};
    for (size_t i = 0; i < CC_SHA256_DIGEST_LENGTH; ++i)
        snprintf(hex + (i * 2), 3, "%02x", hash[i]);
    return hex;
#else
    return "";
#endif
}

// Find a set of count random files in the corpus matching the given regex pattern
inline std::vector<std::string> FindRandomCorpusFiles(const std::string& pattern_str, int count) {
    std::string dir = CorpusDir();
    std::vector<std::string> matched_paths;
    if (!std::filesystem::exists(dir)) return {};
    
    std::regex pattern(pattern_str, std::regex_constants::icase);
    
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
        std::string path_str = entry.path().string();
        if (path_str.find("build") != std::string::npos ||
            path_str.find("bin") != std::string::npos ||
            path_str.find("scratch") != std::string::npos ||
            path_str.find("input_files") != std::string::npos ||
            path_str.find("converted_files") != std::string::npos ||
            path_str.find("final_files") != std::string::npos ||
            path_str.find(".git") != std::string::npos) {
            continue;
        }
        
        if (!entry.is_regular_file()) continue;
        
        std::string filename = entry.path().filename().string();
        
        if (filename.find("_out") != std::string::npos ||
            filename.find("_temp") != std::string::npos ||
            filename.find("_recompiled") != std::string::npos ||
            filename.find("_converted") != std::string::npos ||
            filename.find("_cleaned") != std::string::npos ||
            filename.find("_back") != std::string::npos ||
            filename.find("_rnd2") != std::string::npos) {
            continue;
        }
        
        if (std::regex_search(filename, pattern)) {
            matched_paths.push_back(entry.path().string());
        }
    }
    
    if (matched_paths.empty()) return {};
    
    // We want unseeded shuffling to select dynamically random files at run-time
    std::random_device rd;
    std::mt19937 g(rd());
    std::shuffle(matched_paths.begin(), matched_paths.end(), g);
    
    if ((int)matched_paths.size() > count) {
        matched_paths.resize(count);
    }
    
    return matched_paths;
}

// Base fixture: ensures igi1conv.exe exists.
class IGI1ConvTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(std::filesystem::exists(IGI1ConvExe()))
            << "igi1conv.exe not found next to test exe: " << IGI1ConvExe();
    }
};

} // namespace igi1conv_test

// Declare a corpus path and SKIP the current test if no matching file is found.
// Used in a (void) test body so GTEST_SKIP returns from the test correctly.
#define IGI1CONV_NEED(var, pattern)                                        \
    std::string var = ::igi1conv_test::FindCorpusFileByRegex(pattern);     \
    if (var.empty())                                                    \
        GTEST_SKIP() << "corpus file missing for regex: " << pattern    \
                     << " (set IGI_GAME_PATH)"
