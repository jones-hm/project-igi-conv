#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace igi1conv {

struct QscTaskSelector {
    std::optional<int32_t> taskId;
    std::string className;
    std::string objectName;
};

struct QscFieldUpdate {
    std::size_t directIndex = 0;
    std::string literal;
    // Named placement fields are defined only for the HumanSoldier layout.
    // Generic indexed updates leave this false so other game task classes can
    // use their own direct-argument semantics.
    bool requiresHumanSoldierLayout = false;
};

struct QscTaskSummary {
    int32_t taskId = -1;
    bool taskIdParsed = false;
    std::string className;
    std::string objectName;
    std::vector<std::string> directArguments;
};

struct QscEditResult {
    bool ok = false;
    std::size_t matchedCalls = 0;
    std::size_t changedFields = 0;
    std::string error;
};

bool ListQscTasks(const std::string& source,
                  std::vector<QscTaskSummary>& tasks,
                  std::string& error);

QscEditResult EditQscTasks(const std::string& source,
                           const QscTaskSelector& selector,
                           const std::vector<QscFieldUpdate>& updates,
                           std::string& output);

std::string QuoteQscString(const std::string& value);

} // namespace igi1conv
