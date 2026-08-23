#include <gtest/gtest.h>

#include "mcp_transport.h"

#include <QJsonDocument>
#include <QJsonObject>

#include <sstream>
#include <string>

namespace {

std::string JsonLine(const QJsonObject& object) {
    const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    return std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
}

} // namespace

TEST(McpTransport, StdioFramesResponsesAndKeepsDiagnosticsOffStdout) {
    igi1conv::McpDispatcher dispatcher(
        [](const std::vector<std::string>&, const std::string&) {
            return igi1conv::McpExecutionResult{0, "ok", "diagnostic"};
        });
    std::ostringstream input;
    QJsonObject initialize{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}};
    QJsonObject notification{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}};
    QJsonObject list{{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}};
    input << JsonLine(initialize) << '\n'
          << JsonLine(notification) << '\n'
          << JsonLine(list) << '\n'
          << "not-json\n";

    std::istringstream source(input.str());
    std::ostringstream output;
    std::ostringstream diagnostics;
    ASSERT_EQ(igi1conv::RunMcpStdio(dispatcher, source, output, diagnostics), 0);

    std::istringstream lines(output.str());
    std::string line;
    int responseCount = 0;
    while (std::getline(lines, line)) {
        ASSERT_FALSE(line.empty());
        QJsonParseError parseError;
        const auto response = QJsonDocument::fromJson(
            QByteArray(line.data(), static_cast<int>(line.size())), &parseError);
        ASSERT_EQ(parseError.error, QJsonParseError::NoError) << line;
        ASSERT_TRUE(response.isObject());
        ++responseCount;
    }
    EXPECT_EQ(responseCount, 3);
    EXPECT_TRUE(diagnostics.str().empty());
}

TEST(McpTransport, StdioStopsCleanlyAtEofAndUsesLocalhostHttpDefaults) {
    igi1conv::McpDispatcher dispatcher;
    std::istringstream source;
    std::ostringstream output;
    std::ostringstream diagnostics;
    EXPECT_EQ(igi1conv::RunMcpStdio(dispatcher, source, output, diagnostics), 0);
    EXPECT_TRUE(output.str().empty());

    const igi1conv::McpHttpOptions options;
    EXPECT_EQ(options.host, "127.0.0.1");
    EXPECT_EQ(options.port, 8765);
    EXPECT_EQ(options.endpoint, "/mcp");
    EXPECT_TRUE(options.authToken.empty());
}
