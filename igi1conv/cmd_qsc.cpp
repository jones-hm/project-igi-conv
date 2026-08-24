#include "pch.h"
#include "cmd_qsc.h"
#include "qsc_lexer.h"
#include "qsc_object_editor.h"
#include "qsc_parser.h"
#include "qvm_compiler.h"

#include <charconv>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#endif

static void print_help_qsc()
{
    std::cout <<
        "Usage: igi1conv qsc <subcommand> [options]\n"
        "\n"
        "Subcommands:\n"
        "  compile <input.qsc> -o <output.qvm>   Compile QSC script to QVM bytecode\n"
        "  validate <input.qsc>                   Parse and validate QSC script\n"
        "  list-objects <input.qsc> [--json]      List game Task_New placements\n"
        "  edit-object <input.qsc> -o <output.qsc> Edit a game placement\n"
        "\n"
        "Options:\n"
        "  -o <file>   Output file path\n"
        "  --id <n> | --class <name> | --name <name>  Select one placement\n"
        "  --position <x> <y> <z>   Set game position (arguments 3,4,5)\n"
        "  --rotation <gamma>       Set game rotation/gamma (argument 6)\n"
        "  --model-id <id>          Set game model identifier (argument 7)\n"
        "  --team <n>               Set game team/faction (argument 8)\n"
        "  --bone-hierarchy <n>     Set game bone hierarchy (argument 9)\n"
        "  --stand-animation <n>    Set game stand animation (argument 10)\n"
        "  --set <index>=<literal>  Set any direct Task_New argument\n"
        "  --help      Show this help\n";
}

// Read entire file to string; returns false and prints to stderr on failure.
static bool read_file(const std::string& path, std::string& out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) {
        std::cerr << "igi1conv qsc: cannot open '" << path << "'\n";
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

static bool write_file(const std::string& path, const std::string& source)
{
    const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const std::filesystem::path target(path);
    const std::filesystem::path temporary = target.string() + ".tmp-" + std::to_string(stamp);
    std::ofstream f(temporary, std::ios::binary | std::ios::trunc);
    if (!f.is_open()) {
        std::cerr << "igi1conv qsc: cannot write '" << path << "'\n";
        return false;
    }
    f.write(source.data(), static_cast<std::streamsize>(source.size()));
    if (!f.good()) {
        std::cerr << "igi1conv qsc: write failed for '" << path << "'\n";
        f.close();
        std::error_code cleanupError;
        std::filesystem::remove(temporary, cleanupError);
        return false;
    }
    f.flush();
    if (!f.good()) {
        std::cerr << "igi1conv qsc: flush failed for '" << path << "'\n";
        f.close();
        std::error_code cleanupError;
        std::filesystem::remove(temporary, cleanupError);
        return false;
    }
    f.close();
    if (f.fail()) {
        std::cerr << "igi1conv qsc: close failed for '" << path << "'\n";
        std::error_code cleanupError;
        std::filesystem::remove(temporary, cleanupError);
        return false;
    }

    bool replaced = false;
#ifdef _WIN32
    replaced = MoveFileExW(temporary.wstring().c_str(), target.wstring().c_str(),
                           MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    std::error_code renameError;
    std::filesystem::rename(temporary, target, renameError);
    replaced = !renameError;
#endif
    if (!replaced) {
        std::cerr << "igi1conv qsc: cannot replace '" << path << "' atomically\n";
        std::error_code cleanupError;
        std::filesystem::remove(temporary, cleanupError);
        return false;
    }
    return true;
}

static bool parse_size(const std::string& text, std::size_t& value)
{
    if (text.empty()) return false;
    const char* begin = text.data();
    const char* end = begin + text.size();
    auto parsed = std::from_chars(begin, end, value);
    return parsed.ec == std::errc() && parsed.ptr == end;
}

static bool parse_int32(const std::string& text, int32_t& value)
{
    if (text.empty()) return false;
    const char* begin = text.data();
    const char* end = begin + text.size();
    auto parsed = std::from_chars(begin, end, value);
    return parsed.ec == std::errc() && parsed.ptr == end;
}

static std::string json_string(const std::string& value)
{
    std::string result = "\"";
    const char* hex = "0123456789abcdef";
    for (const unsigned char c : value) {
        switch (c) {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\b': result += "\\b"; break;
        case '\f': result += "\\f"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (c < 0x20) {
                result += "\\u00";
                result.push_back(hex[c >> 4]);
                result.push_back(hex[c & 0x0f]);
            } else {
                result.push_back(static_cast<char>(c));
            }
            break;
        }
    }
    result.push_back('"');
    return result;
}

// Lex + parse a QSC source string; returns 0 on success, 3 on error.
static int parse_qsc(const std::string& source, const std::string& path, qsc::ParseResult& result)
{
    qsc::LexResult lex = qsc::Lex(source);
    if (!lex.ok) {
        std::cerr << "igi1conv qsc: lex error in '" << path << "': " << lex.error << "\n";
        return 3;
    }

    result = qsc::Parse(lex.tokens);
    if (!result.ok) {
        std::cerr << "igi1conv qsc: parse error in '" << path << "': " << result.error << "\n";
        return 3;
    }

    return 0;
}

static int do_compile(int argc, char** argv)
{
    if (argc < 2) {
        std::cerr << "igi1conv qsc compile: missing input file\n";
        std::cerr << "Usage: igi1conv qsc compile <input.qsc> -o <output.qvm>\n";
        return 1;
    }

    std::string input = argv[1];

    // Find -o
    std::string output;
    for (int i = 2; i < argc - 1; ++i) {
        if (std::string(argv[i]) == "-o") {
            output = argv[i + 1];
            break;
        }
    }

    if (output.empty()) {
        std::cerr << "igi1conv qsc compile: missing -o <output.qvm>\n";
        return 1;
    }

    // Check input exists
    std::string source;
    if (!read_file(input, source)) {
        return 2;
    }

    // Parse
    qsc::ParseResult parsed;
    int rc = parse_qsc(source, input, parsed);
    if (rc != 0) return rc;

    // Compile
    std::string compile_error;
    if (!qvm::CompileToFile(*parsed.program, output, &compile_error)) {
        if (!compile_error.empty())
            std::cerr << "igi1conv qsc compile: " << compile_error << "\n";
        else
            std::cerr << "igi1conv qsc compile: failed to write '" << output << "'\n";
        return 4;
    }

    std::cout << "igi1conv qsc: compiled '" << input << "' -> '" << output << "'\n";
    return 0;
}

static int do_validate(int argc, char** argv)
{
    if (argc < 2) {
        std::cerr << "igi1conv qsc validate: missing input file\n";
        std::cerr << "Usage: igi1conv qsc validate <input.qsc>\n";
        return 1;
    }

    std::string input = argv[1];

    std::string source;
    if (!read_file(input, source)) {
        return 2;
    }

    qsc::ParseResult parsed;
    int rc = parse_qsc(source, input, parsed);
    if (rc != 0) return rc;

    std::cout << "igi1conv qsc: '" << input << "' is valid"
              << " (" << parsed.call_count << " calls, " << parsed.arg_count << " args)\n";
    return 0;
}

static int do_list_objects(int argc, char** argv)
{
    if (argc < 2) {
        std::cerr << "igi1conv qsc list-objects: missing input file\n";
        return 1;
    }

    const std::string input = argv[1];
    bool json = false;
    for (int i = 2; i < argc; ++i) {
        if (std::string(argv[i]) == "--json") {
            json = true;
        } else if (std::string(argv[i]) == "--help" || std::string(argv[i]) == "-h") {
            std::cout << "Usage: igi1conv qsc list-objects <input.qsc> [--json]\n";
            return 0;
        } else {
            std::cerr << "igi1conv qsc list-objects: unknown option '" << argv[i] << "'\n";
            return 1;
        }
    }

    std::string source;
    if (!read_file(input, source)) return 2;

    std::vector<igi1conv::QscTaskSummary> tasks;
    std::string error;
    if (!igi1conv::ListQscTasks(source, tasks, error)) {
        std::cerr << "igi1conv qsc list-objects: " << error << "\n";
        return 3;
    }
    if (tasks.empty()) {
        std::cerr << "igi1conv qsc list-objects: " << error << "\n";
        return 3;
    }

    if (json) {
        std::cout << "{\n  \"tasks\": [\n";
        for (std::size_t i = 0; i < tasks.size(); ++i) {
            const auto& task = tasks[i];
            std::cout << "    {\"task_id\":" << task.taskId
                      << ",\"class\":" << json_string(task.className)
                      << ",\"name\":" << json_string(task.objectName)
                      << ",\"direct_arguments\":[";
            for (std::size_t j = 0; j < task.directArguments.size(); ++j) {
                if (j != 0) std::cout << ',';
                std::cout << json_string(task.directArguments[j]);
            }
            std::cout << "]}" << (i + 1 == tasks.size() ? "\n" : ",\n");
        }
        std::cout << "  ]\n}\n";
    } else {
        for (const auto& task : tasks) {
            std::cout << "task_id=" << task.taskId
                      << " class=" << task.className
                      << " name=" << task.objectName
                      << " direct_arguments=" << task.directArguments.size() << "\n";
        }
    }
    return 0;
}

static bool parse_set_update(const std::string& text, igi1conv::QscFieldUpdate& update)
{
    const std::size_t separator = text.find('=');
    if (separator == std::string::npos || separator == 0 || separator + 1 >= text.size())
        return false;
    if (!parse_size(text.substr(0, separator), update.directIndex)) return false;
    update.literal = text.substr(separator + 1);
    return !update.literal.empty();
}

static int do_edit_object(int argc, char** argv)
{
    if (argc < 2) {
        std::cerr << "igi1conv qsc edit-object: missing input file\n";
        return 1;
    }

    const std::string input = argv[1];
    std::string output;
    igi1conv::QscTaskSelector selector;
    std::vector<igi1conv::QscFieldUpdate> updates;

    auto require_value = [&](int& index, const char* option, std::string& value) -> bool {
        if (index + 1 >= argc) {
            std::cerr << "igi1conv qsc edit-object: " << option << " requires a value\n";
            return false;
        }
        value = argv[++index];
        return true;
    };

    for (int i = 2; i < argc; ++i) {
        const std::string option = argv[i];
        if (option == "-o" || option == "--output") {
            if (!require_value(i, option.c_str(), output)) return 1;
        } else if (option == "--id") {
            std::string value;
            if (!require_value(i, "--id", value) || !parse_int32(value, selector.taskId.emplace())) {
                std::cerr << "igi1conv qsc edit-object: --id requires an integer\n";
                return 1;
            }
        } else if (option == "--class") {
            if (!require_value(i, "--class", selector.className)) return 1;
        } else if (option == "--name") {
            if (!require_value(i, "--name", selector.objectName)) return 1;
        } else if (option == "--position") {
            if (i + 3 >= argc) {
                std::cerr << "igi1conv qsc edit-object: --position requires x y z\n";
                return 1;
            }
            updates.push_back({3, argv[++i], true});
            updates.push_back({4, argv[++i], true});
            updates.push_back({5, argv[++i], true});
        } else if (option == "--rotation") {
            std::string value;
            if (!require_value(i, "--rotation", value)) return 1;
            updates.push_back({6, std::move(value), true});
        } else if (option == "--model-id") {
            std::string value;
            if (!require_value(i, "--model-id", value)) return 1;
            updates.push_back({7, igi1conv::QuoteQscString(value), true});
        } else if (option == "--team") {
            std::string value;
            if (!require_value(i, "--team", value)) return 1;
            updates.push_back({8, std::move(value), true});
        } else if (option == "--bone-hierarchy") {
            std::string value;
            if (!require_value(i, "--bone-hierarchy", value)) return 1;
            updates.push_back({9, std::move(value), true});
        } else if (option == "--stand-animation") {
            std::string value;
            if (!require_value(i, "--stand-animation", value)) return 1;
            updates.push_back({10, std::move(value), true});
        } else if (option == "--set") {
            std::string value;
            igi1conv::QscFieldUpdate update;
            if (!require_value(i, "--set", value) || !parse_set_update(value, update)) {
                std::cerr << "igi1conv qsc edit-object: --set requires <index>=<literal>\n";
                return 1;
            }
            updates.push_back(std::move(update));
        } else if (option == "--help" || option == "-h") {
            std::cout << "Usage: igi1conv qsc edit-object <input.qsc> -o <output.qsc> "
                         "(--id <n>|--class <name>|--name <name>) [game field options]\n";
            return 0;
        } else {
            std::cerr << "igi1conv qsc edit-object: unknown option '" << option << "'\n";
            return 1;
        }
    }

    if (output.empty()) {
        std::cerr << "igi1conv qsc edit-object: missing -o <output.qsc>\n";
        return 1;
    }
    if (!selector.taskId.has_value() && selector.className.empty() && selector.objectName.empty()) {
        std::cerr << "igi1conv qsc edit-object: one selector (--id, --class, or --name) is required\n";
        return 1;
    }

    std::error_code pathError;
    if (std::filesystem::equivalent(std::filesystem::path(input),
                                    std::filesystem::path(output), pathError)) {
        std::cerr << "igi1conv qsc edit-object: input and output paths must differ; "
                     "in-place editing is not supported\n";
        return 1;
    }

    std::string source;
    if (!read_file(input, source)) return 2;
    std::string edited;
    const auto result = igi1conv::EditQscTasks(source, selector, updates, edited);
    if (!result.ok) {
        std::cerr << "igi1conv qsc edit-object: " << result.error << "\n";
        return 3;
    }
    if (!write_file(output, edited)) return 4;
    std::cout << "igi1conv qsc: edited " << result.changedFields
              << " game field(s) in '" << output << "'\n";
    return 0;
}

int cmd_qsc(int argc, char** argv)
{
    if (argc < 2 || std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h") {
        print_help_qsc();
        return (argc < 2) ? 1 : 0;
    }

    std::string sub = argv[1];
    int sub_argc = argc - 1;
    char** sub_argv = argv + 1;

    if (sub == "compile")  return do_compile(sub_argc, sub_argv);
    if (sub == "validate") return do_validate(sub_argc, sub_argv);
    if (sub == "list-objects") return do_list_objects(sub_argc, sub_argv);
    if (sub == "edit-object") return do_edit_object(sub_argc, sub_argv);

    std::cerr << "igi1conv qsc: unknown subcommand '" << sub << "'\n";
    std::cerr << "Run 'igi1conv qsc --help' for usage.\n";
    return 1;
}
