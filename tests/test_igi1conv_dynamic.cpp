#include "igi1conv_test_util.h"
#include <random>
#include <cstring>

using namespace igi1conv_test;

// Define global flags inside the namespace to match declarations in header
namespace igi1conv_test {
bool g_test_qvm = false;
bool g_test_res = false;
bool g_test_mtp = false;
bool g_test_dat = false;
bool g_test_spr = false;
bool g_test_tex = false;
bool g_test_pic = false;
std::string g_game_path;
}

// Custom main to parse custom test flags before Google Test runs
int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        // --game-path=PATH or --corpus=PATH (alias)
        if (arg.rfind("--game-path=", 0) == 0) {
            igi1conv_test::SetGamePath(arg.substr(std::strlen("--game-path=")));
            continue;
        }
        if (arg.rfind("--corpus=", 0) == 0) {
            igi1conv_test::SetGamePath(arg.substr(std::strlen("--corpus=")));
            continue;
        }
        if (arg == "--test-qvm" || arg == "--qvm") {
            igi1conv_test::g_test_qvm = true;
        } else if (arg == "--test-res" || arg == "--res") {
            igi1conv_test::g_test_res = true;
        } else if (arg == "--test-mtp" || arg == "--mtp") {
            igi1conv_test::g_test_mtp = true;
        } else if (arg == "--test-dat" || arg == "--dat") {
            igi1conv_test::g_test_dat = true;
        } else if (arg == "--test-spr" || arg == "--spr") {
            igi1conv_test::g_test_spr = true;
        } else if (arg == "--test-tex" || arg == "--tex") {
            igi1conv_test::g_test_tex = true;
        } else if (arg == "--test-pic" || arg == "--pic") {
            igi1conv_test::g_test_pic = true;
        } else if (arg == "--test-all" || arg == "--all") {
            igi1conv_test::g_test_qvm = true;
            igi1conv_test::g_test_res = true;
            igi1conv_test::g_test_mtp = true;
            igi1conv_test::g_test_dat = true;
            igi1conv_test::g_test_spr = true;
            igi1conv_test::g_test_tex = true;
            igi1conv_test::g_test_pic = true;
        }
    }
    
    return RUN_ALL_TESTS();
}

// Helper to run a command, expect success, and print stdout/stderr on failure
static bool ExpectRun(const std::string& cmd) {
    std::string err;
    int code = RunIGI1Conv(cmd, &err);
    EXPECT_EQ(code, 0) << "Command failed: igi1conv " << cmd << "\nError output:\n" << err;
    return code == 0;
}

// Helper to generate a random count between 3 and 5
static int GetRandomCount() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(3, 5);
    return dis(gen);
}

// 1. QVM Dynamic Test
TEST_F(IGI1ConvTest, DynamicQVM) {
    if (!g_test_qvm) {
        GTEST_SKIP() << "QVM dynamic test not enabled. Use --test-qvm or --all";
    }
    
    int count = GetRandomCount();
    auto files = FindRandomCorpusFiles("\\.qvm$", count);
    ASSERT_FALSE(files.empty()) << "No QVM files found in corpus.";
    
    std::cout << "[INFO] Running dynamic QVM tests on " << files.size() << " random files.\n";
    for (size_t i = 0; i < files.size(); ++i) {
        const auto& in_file = files[i];
        std::cout << "  Testing QVM round-trip: " << in_file << "\n";
        TempDir tmp;
        std::string filename = std::filesystem::path(in_file).filename().string();
        std::string qsc = tmp / ("QVM_" + std::to_string(i) + "_" + filename + ".qsc");
        std::string out_file = tmp / ("QVM_" + std::to_string(i) + "_" + filename);
        
        // Round 1
        if (ExpectRun("qvm decompile " + Q(in_file) + " -o " + Q(qsc))) {
            ASSERT_TRUE(NonEmptyFile(qsc));
            if (ExpectRun("qsc compile " + Q(qsc) + " -o " + Q(out_file))) {
                ASSERT_TRUE(NonEmptyFile(out_file));
                
                std::string in_hash = GetFileSHA256(in_file);
                std::string out_hash = GetFileSHA256(out_file);
                
                if (in_hash != out_hash) {
                    // Round 2
                    std::string qsc2 = tmp / ("QVM_" + std::to_string(i) + "_" + filename + "_rnd2.qsc");
                    std::string out_file2 = tmp / ("QVM_" + std::to_string(i) + "_" + filename + "_rnd2.qvm");
                    
                    if (ExpectRun("qvm decompile " + Q(out_file) + " -o " + Q(qsc2))) {
                        if (ExpectRun("qsc compile " + Q(qsc2) + " -o " + Q(out_file2))) {
                            std::string out_hash2 = GetFileSHA256(out_file2);
                            EXPECT_EQ(out_hash, out_hash2) << "QVM round-trip hash mismatch on second round for: " << in_file;
                        }
                    }
                }
            }
        }
    }
}

// 2. RES Dynamic Test
TEST_F(IGI1ConvTest, DynamicRES) {
    if (!g_test_res) {
        GTEST_SKIP() << "RES dynamic test not enabled. Use --test-res or --all";
    }
    
    int count = GetRandomCount();
    auto files = FindRandomCorpusFiles("\\.res$", count);
    ASSERT_FALSE(files.empty()) << "No RES files found in corpus.";
    
    std::cout << "[INFO] Running dynamic RES tests on " << files.size() << " random files.\n";
    for (size_t i = 0; i < files.size(); ++i) {
        const auto& in_file = files[i];
        std::cout << "  Testing RES round-trip: " << in_file << "\n";
        TempDir tmp;
        std::string filename = std::filesystem::path(in_file).filename().string();
        // Keep the extracted directory name stable across rounds.  `res pack`
        // derives the archive's LOCAL: prefix from that directory name, so
        // using a different directory for round two changes every entry name
        // and makes an otherwise stable archive look non-deterministic.
        std::string extracted_dir = tmp / ("RES_" + std::to_string(i) + "_payload");
        std::string out_file = tmp / ("RES_" + std::to_string(i) + "_" + filename);
        
        // Round 1
        if (ExpectRun("res unpack " + Q(in_file) + " " + Q(extracted_dir))) {
            if (ExpectRun("res pack " + Q(extracted_dir) + " " + Q(out_file))) {
                ASSERT_TRUE(NonEmptyFile(out_file));
                
                std::string in_hash = GetFileSHA256(in_file);
                std::string out_hash = GetFileSHA256(out_file);
                
                if (in_hash != out_hash) {
                    // Round 2
                    std::string out_file2 = tmp / ("RES_" + std::to_string(i) + "_" + filename + "_rnd2.res");

                    std::error_code ec;
                    std::filesystem::remove_all(extracted_dir, ec);
                    if (ExpectRun("res unpack " + Q(out_file) + " " + Q(extracted_dir))) {
                        if (ExpectRun("res pack " + Q(extracted_dir) + " " + Q(out_file2))) {
                            std::string out_hash2 = GetFileSHA256(out_file2);
                            EXPECT_EQ(out_hash, out_hash2) << "RES round-trip hash mismatch on second round for: " << in_file;
                        }
                    }
                }
            }
        }
    }
}

// 3. MTP Dynamic Test
TEST_F(IGI1ConvTest, DynamicMTP) {
    if (!g_test_mtp) {
        GTEST_SKIP() << "MTP dynamic test not enabled. Use --test-mtp or --all";
    }
    
    int count = GetRandomCount();
    auto files = FindRandomCorpusFiles("\\.mtp$", count);
    ASSERT_FALSE(files.empty()) << "No MTP files found in corpus.";
    
    std::cout << "[INFO] Running dynamic MTP tests on " << files.size() << " random files.\n";
    for (size_t i = 0; i < files.size(); ++i) {
        const auto& in_file = files[i];
        std::cout << "  Testing MTP round-trip: " << in_file << "\n";
        TempDir tmp;
        std::string filename = std::filesystem::path(in_file).filename().string();
        std::string dat_file = tmp / ("MTP_" + std::to_string(i) + "_" + filename + "_converted.dat");
        std::string out_file = tmp / ("MTP_" + std::to_string(i) + "_" + filename);
        
        // Round 1
        if (ExpectRun("mtp to-dat " + Q(in_file) + " -o " + Q(dat_file))) {
            ASSERT_TRUE(NonEmptyFile(dat_file));
            if (ExpectRun("dat to-mtp " + Q(dat_file) + " -o " + Q(out_file))) {
                ASSERT_TRUE(NonEmptyFile(out_file));
                
                std::string in_hash = GetFileSHA256(in_file);
                std::string out_hash = GetFileSHA256(out_file);
                
                if (in_hash != out_hash) {
                    // Round 2
                    std::string dat_file2 = tmp / ("MTP_" + std::to_string(i) + "_" + filename + "_rnd2_converted.dat");
                    std::string out_file2 = tmp / ("MTP_" + std::to_string(i) + "_" + filename + "_rnd2.mtp");
                    
                    if (ExpectRun("mtp to-dat " + Q(out_file) + " -o " + Q(dat_file2))) {
                        if (ExpectRun("dat to-mtp " + Q(dat_file2) + " -o " + Q(out_file2))) {
                            std::string out_hash2 = GetFileSHA256(out_file2);
                            EXPECT_EQ(out_hash, out_hash2) << "MTP round-trip hash mismatch on second round for: " << in_file;
                        }
                    }
                }
            }
        }
    }
}

// 4. DAT Dynamic Test
TEST_F(IGI1ConvTest, DynamicDAT) {
    if (!g_test_dat) {
        GTEST_SKIP() << "DAT dynamic test not enabled. Use --test-dat or --all";
    }
    
    int count = GetRandomCount();
    // Exclude graph files
    auto files = FindRandomCorpusFiles("^(?!.*graph).*\\.[dD][aA][tT]$", count);
    ASSERT_FALSE(files.empty()) << "No DAT files found in corpus.";
    
    std::cout << "[INFO] Running dynamic DAT tests on " << files.size() << " random files.\n";
    for (size_t i = 0; i < files.size(); ++i) {
        const auto& in_file = files[i];
        std::cout << "  Testing DAT round-trip: " << in_file << "\n";
        TempDir tmp;
        std::string filename = std::filesystem::path(in_file).filename().string();
        std::string mtp_file = tmp / ("DAT_" + std::to_string(i) + "_" + filename + "_converted.mtp");
        std::string out_file = tmp / ("DAT_" + std::to_string(i) + "_" + filename);
        
        // Round 1
        if (ExpectRun("dat to-mtp " + Q(in_file) + " -o " + Q(mtp_file))) {
            ASSERT_TRUE(NonEmptyFile(mtp_file));
            if (ExpectRun("mtp to-dat " + Q(mtp_file) + " -o " + Q(out_file))) {
                ASSERT_TRUE(NonEmptyFile(out_file));
                
                std::string in_hash = GetFileSHA256(in_file);
                std::string out_hash = GetFileSHA256(out_file);
                
                if (in_hash != out_hash) {
                    // Round 2
                    std::string mtp_file2 = tmp / ("DAT_" + std::to_string(i) + "_" + filename + "_rnd2_converted.mtp");
                    std::string out_file2 = tmp / ("DAT_" + std::to_string(i) + "_" + filename + "_rnd2.dat");
                    
                    if (ExpectRun("dat to-mtp " + Q(out_file) + " -o " + Q(mtp_file2))) {
                        if (ExpectRun("mtp to-dat " + Q(mtp_file2) + " -o " + Q(out_file2))) {
                            std::string out_hash2 = GetFileSHA256(out_file2);
                            EXPECT_EQ(out_hash, out_hash2) << "DAT round-trip hash mismatch on second round for: " << in_file;
                        }
                    }
                }
            }
        }
    }
}

// 5. SPR Dynamic Test (One-way PNG)
TEST_F(IGI1ConvTest, DynamicSPR) {
    if (!g_test_spr) {
        GTEST_SKIP() << "SPR dynamic test not enabled. Use --test-spr or --all";
    }
    
    int count = GetRandomCount();
    auto files = FindRandomCorpusFiles("\\.spr$", count);
    ASSERT_FALSE(files.empty()) << "No SPR files found in corpus.";
    
    std::cout << "[INFO] Running dynamic SPR tests on " << files.size() << " random files.\n";
    for (size_t i = 0; i < files.size(); ++i) {
        const auto& in_file = files[i];
        std::cout << "  Testing SPR decoding: " << in_file << "\n";
        TempDir tmp;
        std::string filename = std::filesystem::path(in_file).filename().string();
        std::string out_file = tmp / ("SPR_" + std::to_string(i) + "_" + filename + ".png");
        
        if (ExpectRun("tex to-png " + Q(in_file) + " -o " + Q(out_file))) {
            EXPECT_TRUE(NonEmptyFile(out_file)) << "SPR decoding failed to write PNG for: " << in_file;
        }
    }
}

// 6. TEX Dynamic Test (One-way PNG)
TEST_F(IGI1ConvTest, DynamicTEX) {
    if (!g_test_tex) {
        GTEST_SKIP() << "TEX dynamic test not enabled. Use --test-tex or --all";
    }
    
    int count = GetRandomCount();
    auto files = FindRandomCorpusFiles("\\.tex$", count);
    ASSERT_FALSE(files.empty()) << "No TEX files found in corpus.";
    
    std::cout << "[INFO] Running dynamic TEX tests on " << files.size() << " random files.\n";
    for (size_t i = 0; i < files.size(); ++i) {
        const auto& in_file = files[i];
        std::cout << "  Testing TEX decoding: " << in_file << "\n";
        TempDir tmp;
        std::string filename = std::filesystem::path(in_file).filename().string();
        std::string out_file = tmp / ("TEX_" + std::to_string(i) + "_" + filename + ".png");
        
        if (ExpectRun("tex to-png " + Q(in_file) + " -o " + Q(out_file))) {
            EXPECT_TRUE(NonEmptyFile(out_file)) << "TEX decoding failed to write PNG for: " << in_file;
        }
    }
}

// 7. PIC Dynamic Test (One-way PNG)
TEST_F(IGI1ConvTest, DynamicPIC) {
    if (!g_test_pic) {
        GTEST_SKIP() << "PIC dynamic test not enabled. Use --test-pic or --all";
    }
    
    int count = GetRandomCount();
    auto files = FindRandomCorpusFiles("\\.pic$", count);
    ASSERT_FALSE(files.empty()) << "No PIC files found in corpus.";
    
    std::cout << "[INFO] Running dynamic PIC tests on " << files.size() << " random files.\n";
    for (size_t i = 0; i < files.size(); ++i) {
        const auto& in_file = files[i];
        std::cout << "  Testing PIC decoding: " << in_file << "\n";
        TempDir tmp;
        std::string filename = std::filesystem::path(in_file).filename().string();
        std::string out_file = tmp / ("PIC_" + std::to_string(i) + "_" + filename + ".png");
        
        if (ExpectRun("tex to-png " + Q(in_file) + " -o " + Q(out_file))) {
            EXPECT_TRUE(NonEmptyFile(out_file)) << "PIC decoding failed to write PNG for: " << in_file;
        }
    }
}
