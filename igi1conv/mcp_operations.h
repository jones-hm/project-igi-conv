#pragma once

#include <string>
#include <vector>

namespace igi1conv {

struct GameOperation {
    std::string name;
    std::string description;
    bool writesGame = false;
    std::vector<std::string> commandPrefix;
};

// The deterministic allowlist for the MCP game-editing surface.  GUI
// settings, viewer state, and the `mcp` command itself are intentionally not
// represented here.
const std::vector<GameOperation>& GameOperations();

// Returns the registered operation with the given MCP name, or nullptr when
// the name is not part of the game-facing allowlist.
const GameOperation* FindGameOperation(const std::string& name);

bool IsAllowedGameCommand(const std::vector<std::string>& argv,
                          std::string& error);

} // namespace igi1conv
