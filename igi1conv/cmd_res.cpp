#include "pch.h"
#include "cmd_res.h"
#include "res_parser.h"
#include "res_compiler.h"
#include <filesystem>
#include <map>
#include <iterator>
#include <set>

static void print_usage()
{
    std::cerr <<
        "Usage:\n"
        "  igi1conv res list <input.res>\n"
        "  igi1conv res extract <input.res> -o <output_dir>\n"
        "  igi1conv res extract <input.res> --file <name> -o <output_dir>\n"
        "  igi1conv res compile <file.qsc>\n"
        "  igi1conv res pack <dir> <out.res>\n"
        "  igi1conv res unpack <file.res> <dir>\n"
        "  igi1conv res repack <orig.res> <dir> -o <out.res>\n"
        "  igi1conv res append <input.res> <file1> [file2...] -o <out.res> [--prefix LOCAL:textures/]\n";
}

// Return the value of a named option (e.g. "-o", "--file"), or nullptr if absent.
static const char* opt_val(int argc, char** argv, const char* name)
{
    for (int i = 1; i < argc - 1; ++i)
        if (strcmp(argv[i], name) == 0)
            return argv[i + 1];
    return nullptr;
}

int cmd_res(int argc, char** argv)
{
    // argv[0] = "res", argv[1] = subcommand
    if (argc < 2)
    {
        print_usage();
        return 1;
    }

    std::string sub = argv[1];

    // ── list ──────────────────────────────────────────────────────────────────
    if (sub == "list")
    {
        if (argc < 3)
        {
            std::cerr << "res list: missing <input.res>\n";
            return 1;
        }
        std::string path = argv[2];
        std::string err;
        bool ok = RES_ForEachEntry(path, [](const std::string& name, const uint8_t*, size_t) {
            std::cout << name << "\n";
        }, err);

        if (!ok)
        {
            std::cerr << "res list: " << err << "\n";
            // Distinguish file-not-found from parse error
            if (!std::filesystem::exists(path))
                return 2;
            return 3;
        }
        return 0;
    }

    // ── extract ───────────────────────────────────────────────────────────────
    if (sub == "extract")
    {
        if (argc < 3)
        {
            std::cerr << "res extract: missing <input.res>\n";
            return 1;
        }
        std::string path = argv[2];

        const char* out_dir  = opt_val(argc, argv, "-o");
        const char* only_file = opt_val(argc, argv, "--file");

        if (!out_dir)
        {
            std::cerr << "res extract: missing -o <output_dir>\n";
            return 1;
        }

        if (!std::filesystem::exists(path))
        {
            std::cerr << "res extract: file not found: " << path << "\n";
            return 2;
        }

        // Create output directory if it doesn't exist
        std::error_code ec;
        std::filesystem::create_directories(out_dir, ec);
        if (ec)
        {
            std::cerr << "res extract: cannot create output dir: " << ec.message() << "\n";
            return 4;
        }

        int extracted = 0;
        std::string err;
        bool ok = RES_ForEachEntry(path,
            [&](const std::string& name, const uint8_t* data, size_t size) {
                if (only_file && name != only_file)
                    return;

                // Use the base name of the entry so nested paths don't create
                // unexpected subdirectories in the output dir.
                std::filesystem::path entry_path(name);
                std::filesystem::path out_path =
                    std::filesystem::path(out_dir) / entry_path.filename();

                std::ofstream ofs(out_path, std::ios::binary);
                if (!ofs)
                {
                    std::cerr << "res extract: cannot write: " << out_path << "\n";
                    return;
                }
                ofs.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
                ++extracted;
            }, err);

        if (!ok)
        {
            std::cerr << "res extract: " << err << "\n";
            return 3;
        }

        std::cout << "Extracted " << extracted << " file(s) to " << out_dir << "\n";
        return 0;
    }

    // ── compile ───────────────────────────────────────────────────────────────
    if (sub == "compile")
    {
        if (argc < 3)
        {
            std::cerr << "res compile: missing <file.qsc>\n";
            return 1;
        }
        std::string qsc_path = argv[2];
        if (!std::filesystem::exists(qsc_path))
        {
            std::cerr << "res compile: file not found: " << qsc_path << "\n";
            return 2;
        }
        std::string err;
        if (!RES_Compile(qsc_path, err))
        {
            std::cerr << "res compile: " << err << "\n";
            return 3;
        }
        std::cout << "res compile: success\n";
        return 0;
    }

    // ── pack ──────────────────────────────────────────────────────────────────
    if (sub == "pack")
    {
        if (argc < 4)
        {
            std::cerr << "res pack: usage: igi1conv res pack <dir> <out.res> [--prefix <prefix>]\n";
            return 1;
        }
        std::string dir     = argv[2];
        std::string out_res = argv[3];

        if (!std::filesystem::is_directory(dir))
        {
            std::cerr << "res pack: not a directory: " << dir << "\n";
            return 2;
        }

        // If no prefix is supplied, default to the input directory name.
        // RES_Compile resolves LOCAL:prefix/file relative to the QSC's parent
        // directory, so the prefix must match the folder name on disk or the
        // compiler will skip every file and produce an empty 20-byte ILFF header.
        const char* prefix_c = opt_val(argc, argv, "--prefix");
        std::string prefix = prefix_c ? prefix_c : "";
        if (prefix.empty()) {
            prefix = std::filesystem::path(dir).filename().string() + "/";
        }

        // Normalize separators for RES_GenerateQSC
        for (auto& c : out_res) if (c == '\\') c = '/';

        // QSC lives at the parent of inputDir so that LOCAL:prefix/file resolves
        // as parent_dir/prefix/file (e.g. level1/textures/foo.tex), matching the
        // original game's resource script layout.
        namespace fs = std::filesystem;
        std::string qsc_path = (fs::path(dir).parent_path() / "resource.qsc").string();
        // Pass only the filename so BeginResource stays a relative path the compiler
        // can resolve from the QSC's parent directory.
        std::string res_filename = fs::path(out_res).filename().string();
        std::string err;
        if (!RES_GenerateQSC(dir, qsc_path, res_filename, err, prefix))
        {
            std::cerr << "res pack (generate qsc): " << err << "\n";
            return 3;
        }
        if (!RES_Compile(qsc_path, err))
        {
            std::cerr << "res pack (compile): " << err << "\n";
            return 3;
        }
        // RES_Compile writes to parent(qsc)/res_filename; move to user-specified out_res if different.
        fs::path compiled = fs::path(qsc_path).parent_path() / res_filename;
        fs::path desired(out_res);
        std::error_code ec;
        if (fs::absolute(compiled, ec) != fs::absolute(desired, ec))
        {
            fs::create_directories(desired.parent_path(), ec);
            fs::rename(compiled, desired, ec);
            if (ec)
            {
                std::cerr << "res pack: failed to move archive to " << out_res << ": " << ec.message() << "\n";
                return 4;
            }
        }
        std::cout << "res pack: packed to " << out_res << "\n";
        return 0;
    }

    // ── unpack ────────────────────────────────────────────────────────────────
    if (sub == "unpack")
    {
        if (argc < 4)
        {
            std::cerr << "res unpack: usage: igi1conv res unpack <file.res> <dir>\n";
            return 1;
        }
        std::string res_path = argv[2];
        std::string out_dir  = argv[3];

        if (!std::filesystem::exists(res_path))
        {
            std::cerr << "res unpack: file not found: " << res_path << "\n";
            return 2;
        }

        std::error_code ec;
        std::filesystem::create_directories(out_dir, ec);
        if (ec)
        {
            std::cerr << "res unpack: cannot create dir: " << ec.message() << "\n";
            return 4;
        }

        int extracted = 0;
        std::string err;
        bool ok = RES_ForEachEntry(res_path,
            [&](const std::string& name, const uint8_t* data, size_t size) {
                std::filesystem::path entry_path(name);
                std::filesystem::path out_path =
                    std::filesystem::path(out_dir) / entry_path.filename();

                std::ofstream ofs(out_path, std::ios::binary);
                if (!ofs) { std::cerr << "res unpack: cannot write: " << out_path << "\n"; return; }
                ofs.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
                ++extracted;
            }, err);

        if (!ok)
        {
            std::cerr << "res unpack: " << err << "\n";
            return 3;
        }
        std::cout << "Unpacked " << extracted << " file(s) to " << out_dir << "\n";
        return 0;
    }

    // ── repack ────────────────────────────────────────────────────────────────
    // Rebuild a .res preserving its EXACT original entry names, but swapping in
    // updated file bytes from <dir> (matched by basename). Entries whose basename
    // has no file in <dir> keep their original bytes. This is the safe way to
    // write edited lightmaps back: `res pack`/`--prefix` derive entry names from
    // the on-disk layout, which can't reproduce the game's nested
    // "missions/location0/levelN/lightmaps/objNNN.olm" names from a flat
    // lightmaps_unpacked/ folder. repack keeps the names verbatim.
    if (sub == "repack")
    {
        if (argc < 5)
        {
            std::cerr << "res repack: usage: igi1conv res repack <orig.res> <dir> -o <out.res>\n"
                         "  Rebuilds <orig.res> into <out.res>, replacing each entry whose\n"
                         "  basename matches a file in <dir> with that file's bytes; all other\n"
                         "  entries (and the original entry NAMES) are preserved verbatim.\n";
            return 1;
        }
        std::string orig_res = argv[2];
        std::string dir      = argv[3];
        const char* out_c    = opt_val(argc, argv, "-o");
        if (!out_c) { std::cerr << "res repack: -o <out.res> is required\n"; return 1; }
        std::string out_res = out_c;

        if (!std::filesystem::exists(orig_res)) {
            std::cerr << "res repack: file not found: " << orig_res << "\n";
            return 2;
        }
        if (!std::filesystem::is_directory(dir)) {
            std::cerr << "res repack: not a directory: " << dir << "\n";
            return 1;
        }

        // Index <dir> by filename so each original entry can be matched by basename.
        std::map<std::string, std::filesystem::path> byName;
        for (const auto& e : std::filesystem::directory_iterator(dir)) {
            if (e.is_regular_file()) byName[e.path().filename().string()] = e.path();
        }

        std::vector<RESEntry> entries;
        int replaced = 0, kept = 0;
        std::set<std::string> matchedNames;
        std::string replacementError;
        std::string err;
        bool ok = RES_ForEachEntry(orig_res,
            [&](const std::string& name, const uint8_t* data, size_t size) {
                if (!replacementError.empty()) return;
                RESEntry entry;
                entry.name = name; // preserve the EXACT original entry name
                std::string base = std::filesystem::path(name).filename().string();
                auto it = byName.find(base);
                if (it != byName.end()) {
                    std::ifstream ifs(it->second, std::ios::binary);
                    if (!ifs) {
                        replacementError = "cannot read replacement file: " + it->second.string();
                        return;
                    }
                    entry.data.assign(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
                    if (ifs.bad() || (ifs.fail() && !ifs.eof())) {
                        replacementError = "replacement read failed: " + it->second.string();
                        return;
                    }
                    matchedNames.insert(base);
                    ++replaced;
                } else {
                    entry.data.assign(data, data + size);
                    ++kept;
                }
                entries.push_back(std::move(entry));
            }, err);
        if (!ok) {
            std::cerr << "res repack: failed to read " << orig_res << ": " << err << "\n";
            return 3;
        }
        if (!replacementError.empty()) {
            std::cerr << "res repack: " << replacementError << "\n";
            return 3;
        }

        for (const auto& file : byName) {
            if (matchedNames.find(file.first) == matchedNames.end()) {
                std::cerr << "res repack: no archive entry matches " << file.first
                          << "; refusing to write output\n";
                return 3;
            }
        }

        std::string werr;
        if (!RES_WriteEntries(entries, out_res, werr)) {
            std::cerr << "res repack: write failed: " << werr << "\n";
            return 4;
        }
        std::cout << "res repack: wrote " << out_res << " (" << entries.size()
                   << " entries, " << replaced << " replaced, " << kept << " kept)\n";
        return 0;
    }

    // ── append ────────────────────────────────────────────────────────────────
    if (sub == "append")
    {
        // igi1conv res append <input.res> <file1> [file2...] -o <out.res> [--prefix <p>]
        if (argc < 5)
        {
            std::cerr << "res append: usage: igi1conv res append <input.res> <file1> [file2 ...] -o <out.res> [--prefix LOCAL:textures/]\n";
            return 1;
        }
        std::string src_res = argv[2];

        const char* out_res  = opt_val(argc, argv, "-o");
        const char* prefix_c = opt_val(argc, argv, "--prefix");
        std::string prefix = prefix_c ? prefix_c : "";

        if (!out_res)
        {
            std::cerr << "res append: missing -o <out.res>\n";
            return 1;
        }
        if (!std::filesystem::exists(src_res))
        {
            std::cerr << "res append: file not found: " << src_res << "\n";
            return 2;
        }

        // Collect input files: argv[3..] until we hit "-o" or "--prefix"
        std::vector<std::string> input_files;
        for (int i = 3; i < argc; ++i)
        {
            std::string a = argv[i];
            if (a == "-o" || a == "--prefix") { ++i; continue; }
            input_files.push_back(a);
        }
        if (input_files.empty())
        {
            std::cerr << "res append: no input files specified\n";
            return 1;
        }

        std::vector<RESEntry> new_entries;
        for (const auto& fpath : input_files)
        {
            if (!std::filesystem::exists(fpath))
            {
                std::cerr << "res append: file not found: " << fpath << "\n";
                return 2;
            }
            std::ifstream f(fpath, std::ios::binary | std::ios::ate);
            if (!f) { std::cerr << "res append: cannot read: " << fpath << "\n"; return 4; }
            std::streamsize sz = f.tellg(); f.seekg(0);
            RESEntry e;
            e.name = prefix + std::filesystem::path(fpath).filename().string();
            e.data.resize(sz);
            f.read(reinterpret_cast<char*>(e.data.data()), sz);
            new_entries.push_back(std::move(e));
        }

        std::string err;
        if (!RES_StreamAppend(src_res, new_entries, out_res, err))
        {
            std::cerr << "res append: " << err << "\n";
            return 3;
        }
        std::cout << "res append: appended " << new_entries.size() << " file(s) to " << out_res << "\n";
        return 0;
    }

    std::cerr << "res: unknown subcommand '" << sub << "'\n";
    print_usage();
    return 1;
}
