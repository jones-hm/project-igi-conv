#include "pch.h"
#include "cmd_test.h"
#include <filesystem>
#include <iostream>
#include <map>
#include <vector>
#include <string>
#include <cstdlib>
#include <algorithm>
#include <fstream>
#include <iomanip>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#elif !defined(_WIN32)
#include <unistd.h>
#endif

namespace fs = std::filesystem;

static std::string current_executable_path()
{
#ifdef _WIN32
    char* pgmptr = nullptr;
    _get_pgmptr(&pgmptr);
    return pgmptr ? pgmptr : "igi1conv";
#elif defined(__APPLE__)
    uint32_t size = 1024;
    std::vector<char> buffer(size);
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        buffer.resize(size);
    }
    if (_NSGetExecutablePath(buffer.data(), &size) == 0)
        return fs::weakly_canonical(buffer.data()).string();
    return "igi1conv";
#else
    std::vector<char> buffer(4096);
    const ssize_t length = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (length > 0) {
        buffer[static_cast<size_t>(length)] = '\0';
        return buffer.data();
    }
    return "igi1conv";
#endif
}

static bool run_test_cmd(const std::string& cmd_line) {
    std::cout << "  Running: " << cmd_line << "\n";
    std::string wrapped = "\"" + cmd_line + "\"";
    int ret = std::system(wrapped.c_str());
    return ret == 0;
}

static bool read_file_content(const fs::path& path, std::string& out) {
    std::ifstream ifs(path);
    if (!ifs) return false;
    out.assign((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    return true;
}

static std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

static void compare_binary(const fs::path& f1, const fs::path& f2) {
    std::ifstream s1(f1, std::ios::binary | std::ios::ate);
    std::ifstream s2(f2, std::ios::binary | std::ios::ate);
    if (!s1 || !s2) {
        std::cout << "    [VERIFY] Could not open files for binary compare.\n";
        return;
    }
    
    std::streamsize size1 = s1.tellg();
    std::streamsize size2 = s2.tellg();
    s1.seekg(0);
    s2.seekg(0);
    
    std::cout << "    [VERIFY_BIN] Size: Orig=" << size1 << ", New=" << size2 << "\n";
    
    if (size1 != size2) {
        std::cout << "    [VERIFY_BIN] Mismatch: Files differ in size by " << std::abs(size1 - size2) << " bytes.\n";
        return;
    }
    
    std::vector<char> b1(size1), b2(size2);
    s1.read(b1.data(), size1);
    s2.read(b2.data(), size2);
    
    size_t diff_count = 0;
    for (size_t i = 0; i < size1; ++i) {
        if (b1[i] != b2[i]) diff_count++;
    }
    
    if (diff_count == 0) {
        std::cout << "    [VERIFY_BIN] Match: 100% identical.\n";
    } else {
        double pct = (double)diff_count / size1 * 100.0;
        std::cout << "    [VERIFY_BIN] Mismatch: " << diff_count << " bytes differ (" << std::fixed << std::setprecision(2) << pct << "%).\n";
    }
}

static void compare_text(const fs::path& f1, const fs::path& f2) {
    std::ifstream s1(f1);
    std::ifstream s2(f2);
    if (!s1 || !s2) {
        std::cout << "    [VERIFY] Could not open files for text compare.\n";
        return;
    }
    
    std::string l1, l2;
    size_t lines1 = 0, lines2 = 0;
    size_t diff_count = 0;
    
    while (!s1.eof() || !s2.eof()) {
        bool b1 = (bool)std::getline(s1, l1);
        bool b2 = (bool)std::getline(s2, l2);
        if (b1) lines1++;
        if (b2) lines2++;
        if (b1 && b2) {
            if (l1 != l2) diff_count++;
        }
    }
    
    std::cout << "    [VERIFY_TXT] Lines: Orig=" << lines1 << ", New=" << lines2 << "\n";
    if (lines1 != lines2) {
        std::cout << "    [VERIFY_TXT] Mismatch: Line counts differ by " << (lines1 > lines2 ? lines1 - lines2 : lines2 - lines1) << " lines.\n";
    }
    if (diff_count > 0) {
        std::cout << "    [VERIFY_TXT] Mismatch: " << diff_count << " common lines differ.\n";
    } else if (lines1 == lines2) {
        std::cout << "    [VERIFY_TXT] Match: 100% identical.\n";
    }
}

int cmd_test(int argc, char** argv)
{
    if (argc < 3 || std::string(argv[1]) != "--game-path") {
        std::cerr << "Usage: igi1conv test --game-path <path_to_game>\n";
        return 1;
    }

    fs::path game_path = argv[2];
    if (!fs::exists(game_path) || !fs::is_directory(game_path)) {
        std::cerr << "Error: Game path does not exist or is not a directory.\n";
        return 1;
    }

    fs::path test_dir = game_path / "igi1conv_test_suite";
    if (!fs::exists(test_dir)) {
        fs::create_directories(test_dir);
    }

    std::map<std::string, std::vector<fs::path>> collected;
    std::vector<std::string> known_exts = {".res", ".mef", ".tex", ".spr", ".pic", ".fnt", ".qvm", ".qsc", ".dat", ".mtp", ".lmp", ".ctr", ".hmp"};

    std::cout << "Scanning game path for test files...\n";
    for (const auto& entry : fs::recursive_directory_iterator(game_path)) {
        if (!entry.is_regular_file()) continue;
        
        fs::path p = entry.path();
        std::string ext = to_lower(p.extension().string());
        
        if (std::find(known_exts.begin(), known_exts.end(), ext) != known_exts.end()) {
            if (collected[ext].size() < 2) {
                if (p.string().find("igi1conv_test_suite") != std::string::npos) continue;

                fs::path target = test_dir / (std::to_string(collected[ext].size()) + "_" + p.filename().string());
                try {
                    fs::copy_file(p, target, fs::copy_options::overwrite_existing);
                    collected[ext].push_back(target);
                } catch (...) {}
            }
        }
    }

    std::string exe_path = current_executable_path();
    exe_path = "\"" + exe_path + "\""; // quote executable path

    int success_count = 0;
    int fail_count = 0;

    std::cout << "\nStarting tests in " << test_dir.string() << "\n";
    for (const auto& pair : collected) {
        const std::string& ext = pair.first;
        const auto& files = pair.second;
        for (const auto& f : files) {
            std::cout << "\nTesting " << ext << " file: " << f.filename().string() << "\n";
            std::string path_str = f.string();
            
            if (ext == ".tex" || ext == ".spr" || ext == ".pic") {
                if (run_test_cmd(exe_path + " tex info \"" + path_str + "\"")) {
                    std::cout << "  [OK] Parsed successfully.\n";
                    success_count++;
                } else {
                    std::cout << "  [FAIL] Parse failed.\n";
                    fail_count++;
                }
            } else if (ext == ".mef") {
                if (run_test_cmd(exe_path + " mef info \"" + path_str + "\"")) {
                    std::cout << "  [OK] Parsed successfully.\n";
                    success_count++;
                } else {
                    std::cout << "  [FAIL] Parse failed.\n";
                    fail_count++;
                }
            } else if (ext == ".fnt") {
                if (run_test_cmd(exe_path + " fnt info \"" + path_str + "\"")) {
                    std::cout << "  [OK] Parsed successfully.\n";
                    success_count++;
                } else {
                    std::cout << "  [FAIL] Parse failed.\n";
                    fail_count++;
                }
            } else if (ext == ".qvm") {
                std::string qsc1 = path_str + "_1.qsc";
                std::string qvm2 = path_str + "_2.qvm";
                std::string qsc2 = path_str + "_3.qsc";
                bool ok = run_test_cmd(exe_path + " qvm decompile \"" + path_str + "\" -o \"" + qsc1 + "\"") &&
                          run_test_cmd(exe_path + " qsc compile \"" + qsc1 + "\" -o \"" + qvm2 + "\"") &&
                          run_test_cmd(exe_path + " qvm decompile \"" + qvm2 + "\" -o \"" + qsc2 + "\"");
                if (ok) {
                    compare_binary(path_str, qvm2);
                    compare_text(qsc1, qsc2);
                    std::string s1, s2;
                    if (read_file_content(qsc1, s1) && read_file_content(qsc2, s2) && s1 == s2) {
                        std::cout << "  [OK] Semantic roundtrip matches.\n";
                        success_count++;
                    } else {
                        std::cout << "  [FAIL] Semantic roundtrip differs.\n";
                        fail_count++;
                    }
                } else {
                    std::cout << "  [FAIL] Conversion commands failed.\n";
                    fail_count++;
                }
            } else if (ext == ".mtp") {
                std::string json1 = path_str + "_1.json";
                std::string dat2 = path_str + "_2.dat";
                std::string mtp3 = path_str + "_3.mtp";
                std::string json4 = path_str + "_4.json";
                bool ok = run_test_cmd(exe_path + " mtp dump \"" + path_str + "\" -o \"" + json1 + "\"") &&
                          run_test_cmd(exe_path + " mtp to-dat \"" + path_str + "\" -o \"" + dat2 + "\"") &&
                          run_test_cmd(exe_path + " dat to-mtp \"" + dat2 + "\" -o \"" + mtp3 + "\"") &&
                          run_test_cmd(exe_path + " mtp dump \"" + mtp3 + "\" -o \"" + json4 + "\"");
                if (ok) {
                    compare_binary(path_str, mtp3);
                    compare_text(json1, json4);
                    std::string s1, s2;
                    if (read_file_content(json1, s1) && read_file_content(json4, s2) && s1 == s2) {
                        std::cout << "  [OK] Semantic roundtrip matches.\n";
                        success_count++;
                    } else {
                        std::cout << "  [FAIL] Semantic roundtrip differs.\n";
                        fail_count++;
                    }
                } else {
                    std::cout << "  [FAIL] Conversion commands failed.\n";
                    fail_count++;
                }
            } else if (ext == ".dat") {
                std::string fname_lower = to_lower(f.filename().string());
                if (fname_lower.find("graph") != std::string::npos) {
                    if (run_test_cmd(exe_path + " graph info \"" + path_str + "\"")) {
                        std::cout << "  [OK] Graph parsed successfully.\n";
                        success_count++;
                    } else {
                        std::cout << "  [FAIL] Graph parse failed.\n";
                        fail_count++;
                    }
                } else {
                    std::string json1 = path_str + "_1.json";
                    std::string mtp2 = path_str + "_2.mtp";
                    std::string dat3 = path_str + "_3.dat";
                    std::string json4 = path_str + "_4.json";
                    bool ok = run_test_cmd(exe_path + " dat export \"" + path_str + "\" -o \"" + json1 + "\"") &&
                              run_test_cmd(exe_path + " dat to-mtp \"" + path_str + "\" -o \"" + mtp2 + "\"") &&
                              run_test_cmd(exe_path + " mtp to-dat \"" + mtp2 + "\" -o \"" + dat3 + "\"") &&
                              run_test_cmd(exe_path + " dat export \"" + dat3 + "\" -o \"" + json4 + "\"");
                    if (ok) {
                        compare_binary(path_str, dat3);
                        compare_text(json1, json4);
                        std::string s1, s2;
                        if (read_file_content(json1, s1) && read_file_content(json4, s2) && s1 == s2) {
                            std::cout << "  [OK] Semantic roundtrip matches.\n";
                            success_count++;
                        } else {
                            std::cout << "  [FAIL] Semantic roundtrip differs.\n";
                            fail_count++;
                        }
                    } else {
                        std::cout << "  [FAIL] Conversion commands failed.\n";
                        fail_count++;
                    }
                }
            } else if (ext == ".res") {
                std::string list1 = path_str + "_1.txt";
                std::string dir2 = path_str + "_2_dir";
                std::string res3 = path_str + "_3.res";
                std::string list4 = path_str + "_4.txt";
                bool ok = run_test_cmd(exe_path + " res list \"" + path_str + "\" > \"" + list1 + "\"") &&
                          run_test_cmd(exe_path + " res unpack \"" + path_str + "\" \"" + dir2 + "\"") &&
                          run_test_cmd(exe_path + " res pack \"" + dir2 + "\" \"" + res3 + "\"") &&
                          run_test_cmd(exe_path + " res list \"" + res3 + "\" > \"" + list4 + "\"");
                if (ok) {
                    compare_binary(path_str, res3);
                    compare_text(list1, list4);
                    std::cout << "  [OK] Archive repacked successfully.\n";
                    success_count++;
                } else {
                    std::cout << "  [FAIL] Archive repack failed.\n";
                    fail_count++;
                }
            } else {
                std::cout << "  [SKIP] No advanced test defined.\n";
            }
        }
    }

    std::cout << "\nTest Suite Complete. Passed: " << success_count << ", Failed: " << fail_count << "\n";
    return fail_count == 0 ? 0 : 1;
}
