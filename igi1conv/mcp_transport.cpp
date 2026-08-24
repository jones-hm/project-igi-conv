#include "mcp_transport.h"

#include <QByteArray>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QMap>
#include <QUrl>
#include <QTcpServer>
#include <QTcpSocket>

#include <algorithm>
#include <cstddef>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>

namespace igi1conv {
namespace {

constexpr std::size_t kMaxMessageBytes = 8 * 1024 * 1024;
constexpr int kSocketTimeoutMs = 10'000;

QJsonObject JsonRpcError(const QJsonValue& id, int code, const QString& message) {
    QJsonObject error;
    error.insert("code", code);
    error.insert("message", message);
    return QJsonObject{{"jsonrpc", "2.0"}, {"id", id}, {"error", error}};
}

void WriteJsonLine(const QJsonObject& response, std::ostream& output) {
    const QByteArray bytes = QJsonDocument(response).toJson(QJsonDocument::Compact);
    output.write(bytes.constData(), bytes.size());
    output.put('\n');
    output.flush();
}

bool ReadJsonLine(const std::string& line, QJsonObject& request, QJsonObject& errorResponse) {
    if (line.size() > kMaxMessageBytes) {
        errorResponse = JsonRpcError(QJsonValue(QJsonValue::Null), -32700,
                                     QStringLiteral("JSON-RPC message is too large"));
        return false;
    }
    QJsonParseError parseError;
    const QByteArray bytes(line.data(), static_cast<int>(line.size()));
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        errorResponse = JsonRpcError(QJsonValue(QJsonValue::Null), -32700,
                                     QStringLiteral("invalid JSON-RPC message"));
        return false;
    }
    request = document.object();
    return true;
}

// std::getline grows its destination until it finds a delimiter.  MCP input
// is untrusted, so cap the line while reading rather than checking the size
// only after an attacker has already made the process allocate it.
bool ReadBoundedStdioLine(std::istream& input, std::string& line, bool& tooLarge) {
    line.clear();
    tooLarge = false;
    char character = '\0';
    while (input.get(character)) {
        if (character == '\n') return true;
        if (line.size() >= kMaxMessageBytes) {
            tooLarge = true;
            while (input.get(character) && character != '\n') {
                // Discard the rest of this frame so the next request is still
                // independently framed and can be handled safely.
            }
            return true;
        }
        line.push_back(character);
    }
    return !line.empty();
}

bool AcceptsMediaType(const QByteArray& header, const QByteArray& mediaType) {
    const QByteArray wanted = mediaType.toLower();
    for (QByteArray value : header.toLower().split(',')) {
        value = value.trimmed();
        const int parameters = value.indexOf(';');
        if (parameters >= 0) value = value.left(parameters).trimmed();
        if (value == wanted) return true;
    }
    return false;
}

bool IsLoopbackAddress(const QHostAddress& address) {
    return address.isLoopback();
}

bool IsOriginAllowed(const QByteArray& origin, const McpHttpOptions& options,
                     const QHostAddress& bindAddress) {
    if (origin.isEmpty()) return true;
    const std::string originText(origin.constData(), static_cast<std::size_t>(origin.size()));
    if (std::find(options.allowedOrigins.begin(), options.allowedOrigins.end(), originText)
        != options.allowedOrigins.end())
        return true;

    // Without an explicit allowlist, only HTTP origins on loopback are
    // accepted. A remote bind must provide its own origin allowlist.
    if (!IsLoopbackAddress(bindAddress)) return false;
    const QUrl url(QString::fromUtf8(origin));
    if (!url.isValid() || (url.scheme() != QStringLiteral("http")
                           && url.scheme() != QStringLiteral("https")))
        return false;
    QHostAddress originAddress;
    if (url.host() == QStringLiteral("localhost")) {
        originAddress = QHostAddress(QHostAddress::LocalHost);
    } else if (!originAddress.setAddress(url.host())) {
        return false;
    }
    if (!originAddress.isLoopback()) return false;
    if (url.port(-1) != -1 && url.port() != options.port) return false;
    return true;
}

QByteArray HeaderValue(const QMap<QByteArray, QByteArray>& headers, const char* name) {
    return headers.value(QByteArray(name).toLower()).trimmed();
}

QByteArray HttpProtocolVersion(const QMap<QByteArray, QByteArray>& headers,
                               const QJsonObject& request, bool& valid) {
    const QByteArray header = HeaderValue(headers, "mcp-protocol-version");
    if (!header.isEmpty()) {
        const QString version = QString::fromUtf8(header);
        valid = IsSupportedMcpProtocolVersion(version);
        return valid ? header : QByteArray{};
    }

    // The protocol permits a header-less initialization request. For older
    // clients, subsequent header-less requests use the compatibility default.
    if (request.value("method").toString() == QStringLiteral("initialize")) {
        const QJsonObject params = request.value("params").toObject();
        const QString requested = params.value("protocolVersion").toString();
        return NegotiateMcpProtocolVersion(requested).toUtf8();
    }
    return QByteArray("2025-03-26");
}

void WriteHttpResponse(QTcpSocket& socket, int status, const QByteArray& body,
                       const QByteArray& contentType, const QByteArray& origin = {},
                       const QByteArray& protocolVersion = "2025-11-25",
                       bool preflight = false) {
    const QByteArray statusText = status == 200 ? "OK" : status == 202 ? "Accepted"
        : status == 204 ? "No Content"
        : status == 400 ? "Bad Request" : status == 401 ? "Unauthorized"
        : status == 403 ? "Forbidden" : status == 404 ? "Not Found"
        : status == 405 ? "Method Not Allowed" : status == 413 ? "Payload Too Large"
        : "Internal Server Error";
    QByteArray response;
    response += "HTTP/1.1 " + QByteArray::number(status) + " " + statusText + "\r\n";
    response += "Connection: close\r\n";
    response += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    response += "Content-Type: " + contentType + "\r\n";
    response += "MCP-Protocol-Version: " + protocolVersion + "\r\n";
    if (!origin.isEmpty()) {
        response += "Access-Control-Allow-Origin: " + origin + "\r\n";
        response += "Vary: Origin\r\n";
    }
    if (preflight) {
        response += "Access-Control-Allow-Methods: POST, OPTIONS\r\n";
        response += "Access-Control-Allow-Headers: Content-Type, Accept, MCP-Protocol-Version, Authorization, X-MCP-Auth-Token\r\n";
        response += "Access-Control-Max-Age: 600\r\n";
    }
    response += "\r\n";
    response += body;
    socket.write(response);
    socket.waitForBytesWritten(kSocketTimeoutMs);
}

QByteArray ErrorBody(const QString& message) {
    QJsonObject error;
    error.insert("error", message);
    return QJsonDocument(error).toJson(QJsonDocument::Compact);
}

bool ReadHttpRequest(QTcpSocket& socket, QByteArray& request, QString& error) {
    auto readAvailable = [&](qint64 maximum, const QString& failure) {
        const qint64 available = socket.bytesAvailable();
        const qint64 amount = std::min(available, maximum);
        if (amount <= 0) {
            error = failure;
            return false;
        }
        const QByteArray chunk = socket.read(amount);
        if (chunk.isEmpty()) {
            error = failure;
            return false;
        }
        request += chunk;
        return true;
    };

    while (request.indexOf("\r\n\r\n") < 0) {
        if (request.size() >= static_cast<int>(kMaxMessageBytes)) {
            error = QStringLiteral("HTTP headers are too large");
            return false;
        }
        if (!socket.waitForReadyRead(kSocketTimeoutMs)) {
            error = QStringLiteral("timed out waiting for HTTP request");
            return false;
        }
        const qint64 remaining = static_cast<qint64>(kMaxMessageBytes) - request.size();
        if (!readAvailable(remaining, QStringLiteral("failed to read HTTP headers"))) return false;
    }

    const int headerEnd = request.indexOf("\r\n\r\n");
    const QList<QByteArray> headerLines = request.left(headerEnd).split('\n');
    if (headerLines.isEmpty()) {
        error = QStringLiteral("missing HTTP request line");
        return false;
    }
    const QList<QByteArray> requestLine = headerLines.at(0).trimmed().split(' ');
    if (requestLine.size() != 3) {
        error = QStringLiteral("malformed HTTP request line");
        return false;
    }

    QMap<QByteArray, QByteArray> headers;
    for (int i = 1; i < headerLines.size(); ++i) {
        const QByteArray line = headerLines.at(i).trimmed();
        const int separator = line.indexOf(':');
        if (separator <= 0) {
            error = QStringLiteral("malformed HTTP header");
            return false;
        }
        headers.insert(line.left(separator).trimmed().toLower(), line.mid(separator + 1).trimmed());
    }

    bool ok = false;
    const QByteArray contentLengthHeader = HeaderValue(headers, "content-length");
    const bool bodylessRequest = (requestLine.at(0) == "GET" || requestLine.at(0) == "OPTIONS")
        && contentLengthHeader.isEmpty();
    const int contentLength = bodylessRequest ? 0 : contentLengthHeader.toInt(&ok);
    if (!bodylessRequest && (!ok || contentLength < 0
                         || contentLength > static_cast<int>(kMaxMessageBytes))) {
        error = QStringLiteral("missing or invalid Content-Length");
        return false;
    }
    const int bodyStart = headerEnd + 4;
    while (request.size() - bodyStart < contentLength) {
        if (!socket.waitForReadyRead(kSocketTimeoutMs)) {
            error = QStringLiteral("timed out waiting for HTTP body");
            return false;
        }
        const qint64 remaining = contentLength - (request.size() - bodyStart);
        if (!readAvailable(remaining, QStringLiteral("failed to read HTTP body"))) return false;
    }
    request = request.left(bodyStart + contentLength);
    return true;
}

} // namespace

int RunMcpStdio(const McpDispatcher& dispatcher) {
    return RunMcpStdio(dispatcher, std::cin, std::cout, std::cerr);
}

int RunMcpStdio(const McpDispatcher& dispatcher, std::istream& input,
                std::ostream& output, std::ostream& diagnostics) {
    std::string line;
    bool tooLarge = false;
    while (ReadBoundedStdioLine(input, line, tooLarge)) {
        if (tooLarge) {
            WriteJsonLine(JsonRpcError(QJsonValue(QJsonValue::Null), -32700,
                                       QStringLiteral("JSON-RPC message is too large")),
                          output);
            continue;
        }
        QJsonObject request;
        QJsonObject parseError;
        if (!ReadJsonLine(line, request, parseError)) {
            WriteJsonLine(parseError, output);
            continue;
        }
        const auto response = dispatcher.Handle(request);
        if (response.has_value()) WriteJsonLine(*response, output);
    }
    if (input.bad()) {
        diagnostics << "MCP stdio input failed\n";
        return 1;
    }
    return 0;
}

int RunMcpHttp(const McpDispatcher& dispatcher, const McpHttpOptions& options) {
    if (options.endpoint.empty() || options.endpoint.front() != '/') {
        std::cerr << "MCP HTTP endpoint must start with '/'\n";
        return 1;
    }

    QHostAddress bindAddress;
    if (options.host == "localhost") {
        bindAddress = QHostAddress(QHostAddress::LocalHost);
    } else if (!bindAddress.setAddress(QString::fromStdString(options.host))) {
        std::cerr << "MCP HTTP host must be an IP address or localhost\n";
        return 1;
    }
    if (!IsLoopbackAddress(bindAddress) && options.authToken.empty()) {
        std::cerr << "MCP HTTP remote binding requires --auth-token\n";
        return 1;
    }

    QTcpServer server;
    if (!server.listen(bindAddress, options.port)) {
        std::cerr << "MCP HTTP listen failed: " << server.errorString().toStdString() << "\n";
        return 1;
    }
    std::cerr << "MCP HTTP listening on " << server.serverAddress().toString().toStdString()
              << ':' << server.serverPort() << options.endpoint << "\n";

    std::size_t handled = 0;
    while (options.maxRequests == 0 || handled < options.maxRequests) {
        if (!server.waitForNewConnection(-1)) {
            std::cerr << "MCP HTTP accept failed: " << server.errorString().toStdString() << "\n";
            return 1;
        }
        while (server.hasPendingConnections()) {
            QTcpSocket* socket = server.nextPendingConnection();
            if (!socket) continue;
            QByteArray request;
            QString requestError;
            if (!ReadHttpRequest(*socket, request, requestError)) {
                WriteHttpResponse(*socket, 400, ErrorBody(requestError), "application/json");
                delete socket;
                ++handled;
                continue;
            }

            const int headerEnd = request.indexOf("\r\n\r\n");
            const QList<QByteArray> headerLines = request.left(headerEnd).split('\n');
            const QList<QByteArray> requestLine = headerLines.at(0).trimmed().split(' ');
            QMap<QByteArray, QByteArray> headers;
            for (int i = 1; i < headerLines.size(); ++i) {
                const QByteArray line = headerLines.at(i).trimmed();
                const int separator = line.indexOf(':');
                if (separator > 0)
                    headers.insert(line.left(separator).trimmed().toLower(), line.mid(separator + 1).trimmed());
            }

            const QByteArray origin = HeaderValue(headers, "origin");
            const bool originAllowed = IsOriginAllowed(origin, options, bindAddress);
            const QByteArray authorization = HeaderValue(headers, "authorization");
            const QByteArray tokenHeader = HeaderValue(headers, "x-mcp-auth-token");
            const QByteArray expectedToken = QByteArray("Bearer ") + QByteArray::fromStdString(options.authToken);
            const bool authenticated = options.authToken.empty()
                || authorization == expectedToken || tokenHeader == QByteArray::fromStdString(options.authToken);

            const QByteArray method = requestLine.at(0);
            const bool endpointMatches = requestLine.at(1) == QByteArray::fromStdString(options.endpoint);
            if (!originAllowed) {
                WriteHttpResponse(*socket, 403, ErrorBody(QStringLiteral("Origin is not allowed")),
                                  "application/json");
            } else if (!endpointMatches) {
                WriteHttpResponse(*socket, 404, ErrorBody(QStringLiteral("MCP endpoint not found")),
                                  "application/json", origin);
            } else if (method == "OPTIONS") {
                WriteHttpResponse(*socket, 204, {}, "application/json", origin,
                                  "2025-11-25", true);
            } else if (method != "POST") {
                WriteHttpResponse(*socket, 405, ErrorBody(QStringLiteral("MCP endpoint requires POST")),
                                  "application/json", origin);
            } else if (!authenticated) {
                WriteHttpResponse(*socket, 401, ErrorBody(QStringLiteral("MCP authentication required")),
                                  "application/json", origin);
            } else if (!AcceptsMediaType(HeaderValue(headers, "accept"), "application/json")
                       || !AcceptsMediaType(HeaderValue(headers, "accept"), "text/event-stream")) {
                WriteHttpResponse(*socket, 400,
                                  ErrorBody(QStringLiteral("Accept must include application/json and text/event-stream")),
                                  "application/json", origin);
            } else if (!HeaderValue(headers, "content-type").startsWith("application/json")) {
                WriteHttpResponse(*socket, 400, ErrorBody(QStringLiteral("Content-Type must be application/json")),
                                  "application/json", origin);
            } else {
                const QByteArray body = request.mid(headerEnd + 4);
                QJsonParseError parseError;
                const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
                if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
                    const QByteArray responseBody = QJsonDocument(
                        JsonRpcError(QJsonValue(QJsonValue::Null), -32700,
                                     QStringLiteral("invalid JSON-RPC body")))
                        .toJson(QJsonDocument::Compact);
                    WriteHttpResponse(*socket, 400, responseBody, "application/json", origin);
                } else {
                    bool protocolVersionValid = true;
                    const QByteArray protocolVersion = HttpProtocolVersion(
                        headers, document.object(), protocolVersionValid);
                    if (!protocolVersionValid) {
                        WriteHttpResponse(*socket, 400,
                                          ErrorBody(QStringLiteral("unsupported MCP-Protocol-Version")),
                                          "application/json", origin);
                    } else {
                    const auto response = dispatcher.Handle(document.object());
                    if (response.has_value()) {
                        const QByteArray responseBody = QJsonDocument(*response).toJson(QJsonDocument::Compact);
                        QByteArray responseProtocolVersion = protocolVersion;
                        const QJsonObject result = response->value("result").toObject();
                        if (result.value("protocolVersion").isString())
                            responseProtocolVersion = result.value("protocolVersion").toString().toUtf8();
                        WriteHttpResponse(*socket, 200, responseBody, "application/json", origin,
                                          responseProtocolVersion);
                    } else {
                        WriteHttpResponse(*socket, 202, {}, "application/json", origin,
                                          protocolVersion);
                    }
                    }
                }
            }
            socket->disconnectFromHost();
            delete socket;
            ++handled;
        }
    }
    server.close();
    return 0;
}

} // namespace igi1conv
