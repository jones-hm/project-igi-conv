#pragma once

#include <QJsonObject>
#include <QString>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace igi1conv {

struct McpExecutionResult {
    int exitCode = 1;
    std::string stdoutText;
    std::string stderrText;
};

using McpCommandExecutor = std::function<McpExecutionResult(
    const std::vector<std::string>& command,
    const std::string& workingDirectory)>;

bool IsSupportedMcpProtocolVersion(const QString& version);
QString NegotiateMcpProtocolVersion(const QString& requested);

// Transport-neutral MCP JSON-RPC dispatcher.  It knows only the game-facing
// operation registry and the QSC game-object contract; transport framing is
// handled by mcp_transport.cpp.
class McpDispatcher {
public:
    explicit McpDispatcher(McpCommandExecutor executor = {});

    // An empty optional represents a JSON-RPC notification, which must not
    // receive a response on either stdio or HTTP.
    std::optional<QJsonObject> Handle(const QJsonObject& request) const;

private:
    McpCommandExecutor executor_;
    mutable bool initialized_ = false;
};

} // namespace igi1conv
