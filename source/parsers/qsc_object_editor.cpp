#include "qsc_object_editor.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string_view>
#include <unordered_set>

namespace igi1conv {
namespace {

struct ArgumentSpan {
    std::size_t begin = 0;
    std::size_t end = 0;
};

struct TaskCall {
    std::size_t keyword = 0;
    std::vector<ArgumentSpan> arguments;
};

bool IsIdentifierBoundary(char c) {
    return !std::isalnum(static_cast<unsigned char>(c)) && c != '_';
}

std::size_t SkipString(const std::string& source, std::size_t pos) {
    ++pos;
    while (pos < source.size()) {
        if (source[pos] == '\\' && pos + 1 < source.size()) {
            pos += 2;
            continue;
        }
        if (source[pos] == '"')
            return pos + 1;
        ++pos;
    }
    return source.size();
}

std::size_t SkipTrivia(const std::string& source, std::size_t pos) {
    while (pos < source.size()) {
        if (std::isspace(static_cast<unsigned char>(source[pos]))) {
            ++pos;
            continue;
        }
        if (source[pos] == '/' && pos + 1 < source.size() && source[pos + 1] == '/') {
            pos += 2;
            while (pos < source.size() && source[pos] != '\n') ++pos;
            continue;
        }
        if (source[pos] == '/' && pos + 1 < source.size() && source[pos + 1] == '*') {
            const auto end = source.find("*/", pos + 2);
            return end == std::string::npos ? source.size() : end + 2;
        }
        break;
    }
    return pos;
}

bool FindCallEnd(const std::string& source, std::size_t openParen,
                 std::size_t& closeParen, std::string& error) {
    int depth = 1;
    std::size_t pos = openParen + 1;
    while (pos < source.size()) {
        if (source[pos] == '"') {
            pos = SkipString(source, pos);
            continue;
        }
        if (source[pos] == '/' && pos + 1 < source.size() && source[pos + 1] == '/') {
            pos += 2;
            while (pos < source.size() && source[pos] != '\n') ++pos;
            continue;
        }
        if (source[pos] == '/' && pos + 1 < source.size() && source[pos + 1] == '*') {
            const auto end = source.find("*/", pos + 2);
            if (end == std::string::npos) {
                error = "unmatched parenthesis: unterminated block comment";
                return false;
            }
            pos = end + 2;
            continue;
        }
        if (source[pos] == '(') {
            ++depth;
        } else if (source[pos] == ')') {
            --depth;
            if (depth == 0) {
                closeParen = pos;
                return true;
            }
        }
        ++pos;
    }
    error = "unmatched parenthesis in Task_New call";
    return false;
}

bool ParseArguments(const std::string& source, std::size_t openParen,
                    std::size_t closeParen, std::vector<ArgumentSpan>& arguments,
                    std::string& error) {
    arguments.clear();
    std::size_t pos = SkipTrivia(source, openParen + 1);
    while (pos < closeParen) {
        const std::size_t begin = pos;
        std::size_t end = pos;
        int nestedDepth = 0;
        bool terminated = false;

        while (pos < closeParen) {
            if (source[pos] == '"') {
                pos = SkipString(source, pos);
                continue;
            }
            if (source[pos] == '/' && pos + 1 < closeParen && source[pos + 1] == '/') {
                pos += 2;
                while (pos < closeParen && source[pos] != '\n') ++pos;
                continue;
            }
            if (source[pos] == '/' && pos + 1 < closeParen && source[pos + 1] == '*') {
                const auto commentEnd = source.find("*/", pos + 2);
                if (commentEnd == std::string::npos || commentEnd >= closeParen) {
                    error = "unmatched parenthesis: unterminated block comment";
                    return false;
                }
                pos = commentEnd + 2;
                continue;
            }
            if (source[pos] == '(') {
                ++nestedDepth;
            } else if (source[pos] == ')') {
                if (nestedDepth == 0) {
                    error = "unmatched parenthesis in direct Task_New arguments";
                    return false;
                }
                --nestedDepth;
            } else if (source[pos] == ',' && nestedDepth == 0) {
                end = pos;
                terminated = true;
                break;
            }
            ++pos;
        }

        if (!terminated) {
            end = pos;
            pos = closeParen;
        } else {
            ++pos;
        }

        while (end > begin && std::isspace(static_cast<unsigned char>(source[end - 1])))
            --end;
        if (begin < end)
            arguments.push_back({begin, end});

        pos = SkipTrivia(source, pos);
    }
    return true;
}

bool ScanTaskCalls(const std::string& source, std::vector<TaskCall>& calls,
                   std::string& error) {
    calls.clear();
    constexpr std::string_view kName = "Task_New";
    std::size_t pos = 0;
    while (pos < source.size()) {
        if (source[pos] == '"') {
            pos = SkipString(source, pos);
            continue;
        }
        if (source[pos] == '/' && pos + 1 < source.size() && source[pos + 1] == '/') {
            pos += 2;
            while (pos < source.size() && source[pos] != '\n') ++pos;
            continue;
        }
        if (source[pos] == '/' && pos + 1 < source.size() && source[pos + 1] == '*') {
            const auto end = source.find("*/", pos + 2);
            if (end == std::string::npos) {
                error = "unterminated block comment";
                return false;
            }
            pos = end + 2;
            continue;
        }
        if (pos + kName.size() <= source.size()
            && source.compare(pos, kName.size(), kName) == 0
            && (pos == 0 || IsIdentifierBoundary(source[pos - 1]))
            && (pos + kName.size() == source.size()
                || IsIdentifierBoundary(source[pos + kName.size()]))) {
            std::size_t openParen = pos + kName.size();
            while (openParen < source.size()
                   && std::isspace(static_cast<unsigned char>(source[openParen])))
                ++openParen;
            if (openParen >= source.size() || source[openParen] != '(') {
                pos += kName.size();
                continue;
            }
            std::size_t closeParen = 0;
            if (!FindCallEnd(source, openParen, closeParen, error))
                return false;
            TaskCall call;
            call.keyword = pos;
            if (!ParseArguments(source, openParen, closeParen, call.arguments, error))
                return false;
            calls.push_back(std::move(call));
            // Continue inside this call so nested Task_New calls are exposed.
            pos += kName.size();
            continue;
        }
        ++pos;
    }
    return true;
}

std::string ArgText(const std::string& source, const ArgumentSpan& span) {
    return source.substr(span.begin, span.end - span.begin);
}

std::string Unquote(const std::string& raw) {
    if (raw.size() < 2 || raw.front() != '"' || raw.back() != '"')
        return {};
    std::string value;
    value.reserve(raw.size() - 2);
    for (std::size_t i = 1; i + 1 < raw.size(); ++i) {
        if (raw[i] == '\\' && i + 2 < raw.size()) {
            const char escaped = raw[++i];
            switch (escaped) {
            case 'n': value.push_back('\n'); break;
            case 'r': value.push_back('\r'); break;
            case 't': value.push_back('\t'); break;
            default: value.push_back(escaped); break;
            }
        } else {
            value.push_back(raw[i]);
        }
    }
    return value;
}

bool ParseInt(const std::string& raw, int32_t& value) {
    if (raw.empty()) return false;
    char* end = nullptr;
    errno = 0;
    const long parsed = std::strtol(raw.c_str(), &end, 10);
    if (errno == ERANGE || end == raw.c_str() || *end != '\0'
        || parsed < std::numeric_limits<int32_t>::min()
        || parsed > std::numeric_limits<int32_t>::max())
        return false;
    value = static_cast<int32_t>(parsed);
    return true;
}

bool IsSafeLiteral(const std::string& raw) {
    if (raw.empty() || raw.find_first_of("\r\n,;") != std::string::npos)
        return false;
    if (raw.front() == '"') {
        if (raw.size() < 2 || raw.back() != '"') return false;
        return SkipString(raw, 0) == raw.size();
    }
    if (raw == "TRUE" || raw == "FALSE" || raw == "true" || raw == "false")
        return true;

    char* end = nullptr;
    errno = 0;
    std::strtod(raw.c_str(), &end);
    return errno != ERANGE && end != raw.c_str() && *end == '\0';
}

bool Matches(const QscTaskSelector& selector, const QscTaskSummary& task) {
    if (!selector.taskId.has_value() && selector.className.empty()
        && selector.objectName.empty())
        return false;
    if (selector.taskId.has_value() && selector.taskId.value() != task.taskId)
        return false;
    if (!selector.className.empty() && selector.className != task.className)
        return false;
    if (!selector.objectName.empty() && selector.objectName != task.objectName)
        return false;
    return true;
}

bool BuildSummaries(const std::string& source, const std::vector<TaskCall>& calls,
                    std::vector<QscTaskSummary>& tasks, std::string& error) {
    tasks.clear();
    for (const auto& call : calls) {
        QscTaskSummary task;
        task.directArguments.reserve(call.arguments.size());
        for (const auto& argument : call.arguments)
            task.directArguments.push_back(ArgText(source, argument));
        if (!task.directArguments.empty())
            ParseInt(task.directArguments[0], task.taskId);
        if (task.directArguments.size() > 1)
            task.className = Unquote(task.directArguments[1]);
        if (task.directArguments.size() > 2)
            task.objectName = Unquote(task.directArguments[2]);
        tasks.push_back(std::move(task));
    }
    if (tasks.empty())
        error = "no Task_New calls found";
    return true;
}

} // namespace

bool ListQscTasks(const std::string& source,
                  std::vector<QscTaskSummary>& tasks,
                  std::string& error) {
    std::vector<TaskCall> calls;
    if (!ScanTaskCalls(source, calls, error)) {
        tasks.clear();
        return false;
    }
    return BuildSummaries(source, calls, tasks, error);
}

QscEditResult EditQscTasks(const std::string& source,
                           const QscTaskSelector& selector,
                           const std::vector<QscFieldUpdate>& updates,
                           std::string& output) {
    QscEditResult result;
    output = source;

    std::vector<TaskCall> calls;
    if (!ScanTaskCalls(source, calls, result.error))
        return result;

    std::vector<QscTaskSummary> tasks;
    std::string summaryError;
    BuildSummaries(source, calls, tasks, summaryError);
    for (const auto& task : tasks) {
        if (Matches(selector, task))
            ++result.matchedCalls;
    }
    if (result.matchedCalls == 0) {
        result.error = "no Task_New call matched the requested selector";
        return result;
    }
    if (result.matchedCalls > 1) {
        result.error = "selector is ambiguous: matched "
                     + std::to_string(result.matchedCalls) + " Task_New calls";
        return result;
    }
    if (updates.empty()) {
        result.error = "at least one field update is required";
        return result;
    }

    std::size_t selected = 0;
    for (; selected < tasks.size(); ++selected) {
        if (Matches(selector, tasks[selected])) break;
    }
    std::unordered_set<std::size_t> seen;
    for (const auto& update : updates) {
        if (!seen.insert(update.directIndex).second) {
            result.error = "duplicate direct argument update index: "
                         + std::to_string(update.directIndex);
            return result;
        }
        if (update.directIndex >= calls[selected].arguments.size()) {
            result.error = "direct argument index is outside the selected Task_New call";
            return result;
        }
        if (!IsSafeLiteral(update.literal)) {
            result.error = "invalid literal for direct argument update: " + update.literal;
            return result;
        }
    }

    std::vector<QscFieldUpdate> ordered = updates;
    std::sort(ordered.begin(), ordered.end(), [](const auto& left, const auto& right) {
        return left.directIndex > right.directIndex;
    });
    for (const auto& update : ordered) {
        const auto& span = calls[selected].arguments[update.directIndex];
        output.replace(span.begin, span.end - span.begin, update.literal);
    }
    result.ok = true;
    result.changedFields = updates.size();
    return result;
}

std::string QuoteQscString(const std::string& value) {
    std::string output = "\"";
    for (const char c : value) {
        if (c == '\\' || c == '"') output.push_back('\\');
        if (c == '\n') output += "\\n";
        else if (c == '\r') output += "\\r";
        else if (c == '\t') output += "\\t";
        else output.push_back(c);
    }
    output.push_back('"');
    return output;
}

} // namespace igi1conv
