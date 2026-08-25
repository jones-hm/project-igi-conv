#pragma once

#include "mcp_protocol.h"

#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

namespace igi1conv {

struct McpHttpOptions {
    std::string host = "127.0.0.1";
    std::uint16_t port = 8765;
    std::string endpoint = "/mcp";
    std::string authToken;
    std::vector<std::string> allowedOrigins;

    // Test/process harnesses can request a bounded server lifetime. Zero
    // means serve until the process is terminated.
    std::size_t maxRequests = 0;

    // Bound retained HTTP lifecycle state even when the server itself runs
    // indefinitely. Expired sessions are also removed before each request.
    std::size_t maxSessions = 256;
};

int RunMcpStdio(const McpDispatcher& dispatcher);

// Stream overload used by unit tests; stdout receives protocol responses and
// diagnostics receives only transport diagnostics.
int RunMcpStdio(const McpDispatcher& dispatcher,
                std::istream& input,
                std::ostream& output,
                std::ostream& diagnostics);

int RunMcpHttp(const McpDispatcher& dispatcher, const McpHttpOptions& options);

} // namespace igi1conv
