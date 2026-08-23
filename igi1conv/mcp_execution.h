#pragma once

#include "mcp_protocol.h"

#include <string>
#include <vector>

namespace igi1conv {

// Production executor used by the executable entry points.  It invokes the
// validated in-process dispatcher and never starts a shell or an executable
// supplied by MCP input.
McpExecutionResult ExecuteMcpGameCommand(const std::vector<std::string>& command,
                                         const std::string& workingDirectory);

} // namespace igi1conv
