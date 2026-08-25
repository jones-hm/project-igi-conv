#include <gtest/gtest.h>

#include "mcp_transport.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>

#include <chrono>
#include <sstream>
#include <string>
#include <thread>

namespace {

std::string JsonLine(const QJsonObject& object) {
    const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    return std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
}

struct HttpResponse {
    int status = 0;
    QByteArray headers;
    QByteArray body;
    bool connected = false;
};

quint16 FindFreePort() {
    QTcpServer probe;
    EXPECT_TRUE(probe.listen(QHostAddress::LocalHost, 0));
    const quint16 port = probe.serverPort();
    probe.close();
    return port;
}

HttpResponse SendHttp(quint16 port, const QByteArray& method, const QByteArray& endpoint,
                      const QByteArray& body, const QByteArray& origin = {},
                      const QByteArray& authorization = {}, const QByteArray& session = {},
                      bool includeHost = true, int declaredContentLength = -1) {
    HttpResponse response;
    for (int attempt = 0; attempt < 50; ++attempt) {
        QTcpSocket socket;
        socket.connectToHost(QHostAddress::LocalHost, port);
        if (!socket.waitForConnected(100)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        response.connected = true;
        QByteArray request = method + " " + endpoint + " HTTP/1.1\r\n";
        if (includeHost)
            request += "Host: 127.0.0.1:" + QByteArray::number(port) + "\r\n";
        request += "Accept: application/json, text/event-stream\r\n"
                   "Content-Type: application/json\r\n"
                   "MCP-Protocol-Version: 2025-11-25\r\n";
        if (!origin.isEmpty()) request += "Origin: " + origin + "\r\n";
        if (!authorization.isEmpty()) request += "Authorization: " + authorization + "\r\n";
        if (!session.isEmpty()) request += "Mcp-Session-Id: " + session + "\r\n";
        const int contentLength = declaredContentLength >= 0
            ? declaredContentLength : body.size();
        request += "Content-Length: " + QByteArray::number(contentLength) + "\r\n\r\n" + body;
        socket.write(request);
        if (!socket.waitForBytesWritten(1000)) return response;
        QByteArray raw;
        while (socket.waitForReadyRead(3000)) raw += socket.readAll();
        raw += socket.readAll();
        const int separator = raw.indexOf("\r\n\r\n");
        if (separator < 0) return response;
        response.headers = raw.left(separator);
        response.body = raw.mid(separator + 4);
        const QList<QByteArray> statusParts = response.headers.left(
            response.headers.indexOf("\r\n")).split(' ');
        if (statusParts.size() >= 2) response.status = statusParts.at(1).toInt();
        return response;
    }
    return response;
}

QByteArray Header(const HttpResponse& response, const QByteArray& name) {
    for (const auto& line : response.headers.split('\n')) {
        const QByteArray trimmed = line.trimmed();
        const int separator = trimmed.indexOf(':');
        if (separator > 0 && trimmed.left(separator).compare(name, Qt::CaseInsensitive) == 0)
            return trimmed.mid(separator + 1).trimmed();
    }
    return {};
}

QJsonObject InitializeRequest() {
    return QJsonObject{
        {"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"},
        {"params", QJsonObject{
            {"protocolVersion", "2025-11-25"}, {"capabilities", QJsonObject{}},
            {"clientInfo", QJsonObject{{"name", "http-test"}, {"version", "1"}}},
        }},
    };
}

void WaitForHttpServer(std::thread& serverThread) {
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    (void)serverThread;
}

} // namespace

TEST(McpTransport, StdioFramesResponsesAndKeepsDiagnosticsOffStdout) {
    igi1conv::McpDispatcher dispatcher(
        [](const std::vector<std::string>&, const std::string&) {
            return igi1conv::McpExecutionResult{0, "ok", "diagnostic"};
        });
    std::ostringstream input;
    QJsonObject initialize{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}};
    initialize.insert("params", QJsonObject{
        {"protocolVersion", "2025-11-25"},
        {"capabilities", QJsonObject{}},
        {"clientInfo", QJsonObject{{"name", "stdio-test"}, {"version", "1"}}},
    });
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

TEST(McpTransport, RejectsOversizedStdioFrameWithoutConsumingTheNextRequest) {
    igi1conv::McpDispatcher dispatcher;
    QJsonObject ping{{"jsonrpc", "2.0"}, {"id", 7}, {"method", "ping"}};
    std::string input(8 * 1024 * 1024 + 1, 'x');
    input.push_back('\n');
    input += JsonLine(ping);
    input.push_back('\n');

    std::istringstream source(input);
    std::ostringstream output;
    std::ostringstream diagnostics;
    ASSERT_EQ(igi1conv::RunMcpStdio(dispatcher, source, output, diagnostics), 0);

    std::istringstream lines(output.str());
    std::string line;
    ASSERT_TRUE(std::getline(lines, line));
    QJsonObject oversized = QJsonDocument::fromJson(QByteArray::fromStdString(line)).object();
    EXPECT_EQ(oversized.value("error").toObject().value("code").toInt(), -32700);
    ASSERT_TRUE(std::getline(lines, line));
    QJsonObject response = QJsonDocument::fromJson(QByteArray::fromStdString(line)).object();
    EXPECT_EQ(response.value("id").toInt(), 7);
    EXPECT_TRUE(diagnostics.str().empty());
}

TEST(McpTransport, ExercisesHttpProtocolAndSecurityPathsInProcess) {
    igi1conv::McpDispatcher dispatcher;
    const quint16 port = FindFreePort();
    igi1conv::McpHttpOptions options;
    options.port = port;
    options.maxRequests = 2;
    int serverResult = -1;
    std::thread serverThread([&] { serverResult = igi1conv::RunMcpHttp(dispatcher, options); });
    WaitForHttpServer(serverThread);

    const QByteArray origin = "http://127.0.0.1:" + QByteArray::number(port);
    const HttpResponse initialize = SendHttp(
        port, "POST", "/mcp", QByteArray::fromStdString(JsonLine(InitializeRequest())), origin);
    ASSERT_TRUE(initialize.connected);
    ASSERT_EQ(initialize.status, 200);
    const QByteArray session = Header(initialize, "Mcp-Session-Id");
    ASSERT_FALSE(session.isEmpty());
    const QJsonObject list{{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}};
    const HttpResponse tools = SendHttp(
        port, "POST", "/mcp", QByteArray::fromStdString(JsonLine(list)), origin, {}, session);
    ASSERT_EQ(tools.status, 200);
    serverThread.join();
    EXPECT_EQ(serverResult, 0);

    const auto runSingle = [&](const igi1conv::McpHttpOptions& serverOptions,
                               const QByteArray& endpoint, const QByteArray& body,
                               const QByteArray& requestOrigin = {},
                               const QByteArray& authorization = {},
                               bool includeHost = true, int declaredContentLength = -1) {
        int result = -1;
        std::thread worker([&] { result = igi1conv::RunMcpHttp(dispatcher, serverOptions); });
        WaitForHttpServer(worker);
        const HttpResponse response = SendHttp(serverOptions.port, "POST", endpoint, body,
                                                requestOrigin, authorization, {}, includeHost,
                                                declaredContentLength);
        worker.join();
        EXPECT_EQ(result, 0);
        return response;
    };

    igi1conv::McpHttpOptions authOptions;
    authOptions.port = FindFreePort();
    authOptions.authToken = "http-secret";
    authOptions.maxRequests = 3;
    int authResult = -1;
    std::thread authThread([&] { authResult = igi1conv::RunMcpHttp(dispatcher, authOptions); });
    WaitForHttpServer(authThread);
    const QByteArray authOrigin = "http://127.0.0.1:" + QByteArray::number(authOptions.port);
    const QByteArray initBody = QByteArray::fromStdString(JsonLine(InitializeRequest()));
    EXPECT_EQ(SendHttp(authOptions.port, "POST", "/mcp", initBody, authOrigin).status, 401);
    EXPECT_EQ(SendHttp(authOptions.port, "POST", "/mcp", initBody, authOrigin,
                       "Bearer wrong").status, 401);
    EXPECT_EQ(SendHttp(authOptions.port, "POST", "/mcp", initBody, authOrigin,
                       "Bearer http-secret").status, 200);
    authThread.join();
    EXPECT_EQ(authResult, 0);

    igi1conv::McpHttpOptions badOriginOptions;
    badOriginOptions.port = FindFreePort();
    badOriginOptions.maxRequests = 1;
    EXPECT_EQ(runSingle(badOriginOptions, "/mcp", initBody, "http://evil.example").status, 403);

    igi1conv::McpHttpOptions missingHostOptions;
    missingHostOptions.port = FindFreePort();
    missingHostOptions.maxRequests = 1;
    EXPECT_EQ(runSingle(missingHostOptions, "/mcp", initBody, {}, {}, false).status, 400);

    igi1conv::McpHttpOptions wrongEndpointOptions;
    wrongEndpointOptions.port = FindFreePort();
    wrongEndpointOptions.maxRequests = 1;
    EXPECT_EQ(runSingle(wrongEndpointOptions, "/wrong", initBody).status, 404);

    igi1conv::McpHttpOptions oversizedOptions;
    oversizedOptions.port = FindFreePort();
    oversizedOptions.maxRequests = 1;
    const int oversizedLength = 8 * 1024 * 1024 + 1;
    const HttpResponse oversizedResponse =
        runSingle(oversizedOptions, "/mcp", {}, {}, {}, true, oversizedLength);
    EXPECT_EQ(oversizedResponse.status, 413);
    EXPECT_TRUE(oversizedResponse.body.contains("too large"));
}
