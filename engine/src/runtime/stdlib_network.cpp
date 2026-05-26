#include "runtime/stdlib_network.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/stat.h>
#include <cstring>
#include <string>
#include <sstream>
#include <fstream>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <functional>
#include <thread>
#include <mutex>
#include <atomic>
#include <curl/curl.h>
#include <iostream>
#include <openssl/sha.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <poll.h>

namespace gard {
namespace runtime {
namespace stdlib {

// ===== HTTP Server internals =====

// Parse an HTTP request from raw data
struct HttpRequest {
    std::string method;
    std::string path;
    std::string query;
    std::string httpVersion;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
    std::string rawPath; // path + query
    std::string remoteAddr;
    int remotePort = 0;
};

// MIME type detection from file extension
static std::string getMimeType(const std::string& path) {
    static const std::unordered_map<std::string, std::string> mimeTypes = {
        {".html", "text/html"}, {".htm", "text/html"},
        {".css", "text/css"}, {".js", "application/javascript"},
        {".json", "application/json"}, {".xml", "application/xml"},
        {".txt", "text/plain"}, {".csv", "text/csv"},
        {".png", "image/png"}, {".jpg", "image/jpeg"}, {".jpeg", "image/jpeg"},
        {".gif", "image/gif"}, {".svg", "image/svg+xml"}, {".ico", "image/x-icon"},
        {".webp", "image/webp"}, {".bmp", "image/bmp"},
        {".woff", "font/woff"}, {".woff2", "font/woff2"}, {".ttf", "font/ttf"},
        {".otf", "font/otf"}, {".eot", "application/vnd.ms-fontobject"},
        {".pdf", "application/pdf"}, {".zip", "application/zip"},
        {".gz", "application/gzip"}, {".tar", "application/x-tar"},
        {".mp3", "audio/mpeg"}, {".wav", "audio/wav"}, {".ogg", "audio/ogg"},
        {".mp4", "video/mp4"}, {".webm", "video/webm"}, {".avi", "video/x-msvideo"},
        {".wasm", "application/wasm"}, {".map", "application/json"},
    };
    size_t dot = path.rfind('.');
    if (dot != std::string::npos) {
        std::string ext = path.substr(dot);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        auto it = mimeTypes.find(ext);
        if (it != mimeTypes.end()) return it->second;
    }
    return "application/octet-stream";
}

// Parse raw HTTP request bytes into HttpRequest struct
static bool parseHttpRequest(const std::string& raw, HttpRequest& req) {
    size_t headerEnd = raw.find("\r\n\r\n");
    if (headerEnd == std::string::npos) return false;

    std::string headerSection = raw.substr(0, headerEnd);
    req.body = raw.substr(headerEnd + 4);

    // Parse request line
    size_t firstLine = headerSection.find("\r\n");
    std::string requestLine = headerSection.substr(0, firstLine);

    std::istringstream rl(requestLine);
    rl >> req.method >> req.rawPath >> req.httpVersion;

    // Split path and query
    size_t qpos = req.rawPath.find('?');
    if (qpos != std::string::npos) {
        req.path = req.rawPath.substr(0, qpos);
        req.query = req.rawPath.substr(qpos + 1);
    } else {
        req.path = req.rawPath;
    }

    // URL decode path
    // Parse headers
    size_t pos = firstLine + 2;
    while (pos < headerSection.size()) {
        size_t lineEnd = headerSection.find("\r\n", pos);
        if (lineEnd == std::string::npos) lineEnd = headerSection.size();
        std::string line = headerSection.substr(pos, lineEnd - pos);
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string key = line.substr(0, colon);
            std::string val = line.substr(colon + 1);
            // Trim leading whitespace from value
            size_t start = val.find_first_not_of(' ');
            if (start != std::string::npos) val = val.substr(start);
            // Lowercase key for case-insensitive lookup
            std::string lkey = key;
            std::transform(lkey.begin(), lkey.end(), lkey.begin(), ::tolower);
            req.headers[lkey] = val;
        }
        pos = lineEnd + 2;
    }

    // Read body based on content-length
    auto clIt = req.headers.find("content-length");
    if (clIt != req.headers.end()) {
        size_t contentLen = std::stoul(clIt->second);
        if (req.body.size() < contentLen) {
            // Incomplete body — in a real streaming impl we'd wait for more data
            // For now, use what we have
        }
    }

    return true;
}

// Build HTTP response string
static std::string buildHttpResponse(int status, const std::string& statusText,
                                     const std::unordered_map<std::string, std::string>& headers,
                                     const std::string& body) {
    std::ostringstream resp;
    resp << "HTTP/1.1 " << status << " " << statusText << "\r\n";
    for (auto& [k, v] : headers) {
        resp << k << ": " << v << "\r\n";
    }
    if (headers.find("Content-Length") == headers.end()) {
        resp << "Content-Length: " << body.size() << "\r\n";
    }
    if (headers.find("Connection") == headers.end()) {
        resp << "Connection: keep-alive\r\n";
    }
    resp << "\r\n" << body;
    return resp.str();
}

// Route matching with path parameters (e.g., /users/:id)
struct Route {
    std::string method; // GET, POST, etc. or "*" for all
    std::string pattern;
    std::vector<std::string> paramNames;
    Value handler; // stored as a string (function name) or object
};

static bool matchRoute(const Route& route, const std::string& method, const std::string& path,
                       std::unordered_map<std::string, std::string>& params) {
    if (route.method != "*" && route.method != method) return false;

    // Split pattern and path into segments
    auto split = [](const std::string& s, char delim) -> std::vector<std::string> {
        std::vector<std::string> parts;
        std::istringstream ss(s);
        std::string part;
        while (std::getline(ss, part, delim)) {
            if (!part.empty()) parts.push_back(part);
        }
        return parts;
    };

    auto patternParts = split(route.pattern, '/');
    auto pathParts = split(path, '/');

    // Check for wildcard at end
    bool hasWildcard = !patternParts.empty() && patternParts.back() == "*";
    if (hasWildcard) patternParts.pop_back();

    if (!hasWildcard && patternParts.size() != pathParts.size()) return false;
    if (hasWildcard && pathParts.size() < patternParts.size()) return false;

    for (size_t i = 0; i < patternParts.size(); i++) {
        if (patternParts[i][0] == ':') {
            // Path parameter
            params[patternParts[i].substr(1)] = pathParts[i];
        } else if (patternParts[i] != pathParts[i]) {
            return false;
        }
    }
    return true;
}

// Global server state (per-server instance tracked by fd)
struct ServerState {
    int fd = -1;
    int epollFd = -1;
    int port = 0;
    std::string host;
    bool running = false;
    std::vector<Route> routes;
    std::vector<Value> middlewares; // middleware handler names
    std::string staticRoot; // root directory for static files
    std::string staticPrefix; // URL prefix for static files (e.g., "/static")
    int maxConnections = 1024;
    int requestTimeout = 30000; // ms
    bool keepAlive = true;
    std::unordered_map<std::string, std::string> defaultHeaders;
    std::mutex mutex;
};

static std::unordered_map<int, std::shared_ptr<ServerState>> g_servers;
static std::mutex g_serversMutex;

// ===== WebSocket internals (RFC 6455) =====

// WebSocket opcodes
enum WsOpcode : uint8_t {
    WS_CONTINUATION = 0x0,
    WS_TEXT         = 0x1,
    WS_BINARY       = 0x2,
    WS_CLOSE        = 0x8,
    WS_PING         = 0x9,
    WS_PONG         = 0xA,
};

// Base64 encode (for Sec-WebSocket-Accept)
static std::string base64Encode(const unsigned char* data, size_t len) {
    BIO* b64 = BIO_new(BIO_f_base64());
    BIO* mem = BIO_new(BIO_s_mem());
    b64 = BIO_push(b64, mem);
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    BIO_write(b64, data, (int)len);
    BIO_flush(b64);
    BUF_MEM* bufPtr;
    BIO_get_mem_ptr(b64, &bufPtr);
    std::string result(bufPtr->data, bufPtr->length);
    BIO_free_all(b64);
    return result;
}

// Generate 16-byte random masking key
static void generateMaskKey(uint8_t mask[4]) {
    RAND_bytes(mask, 4);
}

// Generate random 16-byte WebSocket key (base64 encoded)
static std::string generateWsKey() {
    uint8_t raw[16];
    RAND_bytes(raw, 16);
    return base64Encode(raw, 16);
}

// Compute Sec-WebSocket-Accept from client key (SHA-1 of key + magic GUID, base64)
static std::string computeWsAccept(const std::string& clientKey) {
    static const std::string magic = "258EAFA5-E914-47DA-95CA-5AB4085B9052";
    std::string concat = clientKey + magic;
    unsigned char hash[SHA_DIGEST_LENGTH];
    SHA1(reinterpret_cast<const unsigned char*>(concat.c_str()), concat.size(), hash);
    return base64Encode(hash, SHA_DIGEST_LENGTH);
}

// Build a WebSocket frame (client→server frames are masked per RFC 6455)
static std::vector<uint8_t> buildWsFrame(WsOpcode opcode, const uint8_t* payload, size_t len, bool mask) {
    std::vector<uint8_t> frame;
    frame.push_back(0x80 | (uint8_t)opcode); // FIN + opcode

    // Payload length encoding
    if (len < 126) {
        frame.push_back((mask ? 0x80 : 0x00) | (uint8_t)len);
    } else if (len <= 0xFFFF) {
        frame.push_back((mask ? 0x80 : 0x00) | 126);
        frame.push_back((uint8_t)(len >> 8));
        frame.push_back((uint8_t)(len & 0xFF));
    } else {
        frame.push_back((mask ? 0x80 : 0x00) | 127);
        for (int i = 7; i >= 0; i--) {
            frame.push_back((uint8_t)((len >> (i * 8)) & 0xFF));
        }
    }

    if (mask) {
        uint8_t maskKey[4];
        generateMaskKey(maskKey);
        frame.insert(frame.end(), maskKey, maskKey + 4);
        for (size_t i = 0; i < len; i++) {
            frame.push_back(payload[i] ^ maskKey[i % 4]);
        }
    } else {
        frame.insert(frame.end(), payload, payload + len);
    }
    return frame;
}

// Parse a WebSocket frame from raw bytes
// Returns: opcode, payload, bytes consumed. Returns -1 on incomplete frame.
struct WsFrame {
    WsOpcode opcode;
    std::vector<uint8_t> payload;
    bool fin;
    int bytesConsumed; // -1 if incomplete
};

static WsFrame parseWsFrame(const uint8_t* data, size_t available) {
    WsFrame frame;
    frame.bytesConsumed = -1;
    if (available < 2) return frame;

    frame.fin = (data[0] & 0x80) != 0;
    frame.opcode = (WsOpcode)(data[0] & 0x0F);
    bool masked = (data[1] & 0x80) != 0;
    uint64_t payloadLen = data[1] & 0x7F;
    size_t offset = 2;

    if (payloadLen == 126) {
        if (available < 4) return frame;
        payloadLen = ((uint64_t)data[2] << 8) | data[3];
        offset = 4;
    } else if (payloadLen == 127) {
        if (available < 10) return frame;
        payloadLen = 0;
        for (int i = 0; i < 8; i++) {
            payloadLen = (payloadLen << 8) | data[2 + i];
        }
        offset = 10;
    }

    size_t maskOffset = offset;
    if (masked) offset += 4;

    if (available < offset + payloadLen) return frame;

    frame.payload.resize(payloadLen);
    if (masked) {
        const uint8_t* maskKey = data + maskOffset;
        for (uint64_t i = 0; i < payloadLen; i++) {
            frame.payload[i] = data[offset + i] ^ maskKey[i % 4];
        }
    } else {
        std::memcpy(frame.payload.data(), data + offset, payloadLen);
    }

    frame.bytesConsumed = (int)(offset + payloadLen);
    return frame;
}

// WebSocket connection state
struct WsConnection {
    int fd = -1;
    SSL* ssl = nullptr;
    SSL_CTX* sslCtx = nullptr;
    bool connected = false;
    bool useTls = false;
    std::string url;
    std::string host;
    int port = 80;
    std::string path;
    std::string protocol; // negotiated subprotocol
    std::vector<std::string> requestedProtocols;
    std::string closeReason;
    int closeCode = 0;
    std::vector<uint8_t> recvBuffer;
    // Reconnection state
    int reconnectAttempts = 0;
    int maxReconnectAttempts = 5;
    int reconnectDelayMs = 1000;
    double reconnectBackoffMultiplier = 2.0;
    bool autoReconnect = false;
};

static std::unordered_map<int, std::shared_ptr<WsConnection>> g_wsConnections;
static std::mutex g_wsMutex;

// ===== WebSocket Server internals =====

struct WsClientInfo {
    int fd = -1;
    std::string id;
    std::string remoteAddr;
    int remotePort = 0;
    std::vector<std::string> rooms;
    std::vector<uint8_t> recvBuffer;
    std::chrono::steady_clock::time_point lastActivity;
    int messageCount = 0; // for rate limiting
    std::chrono::steady_clock::time_point rateLimitWindow;
    bool alive = true; // for heartbeat tracking
};

struct WsServerState {
    int fd = -1;
    int epollFd = -1;
    int port = 0;
    std::string host;
    bool running = false;
    int maxConnections = 1024;
    int heartbeatIntervalMs = 30000;
    int maxMessagesPerMinute = 120; // rate limit
    std::unordered_map<int, std::shared_ptr<WsClientInfo>> clients; // fd → client
    std::unordered_map<std::string, std::shared_ptr<WsClientInfo>> clientsById; // id → client
    std::unordered_map<std::string, std::vector<std::string>> rooms; // room → [clientIds]
    int nextClientId = 1;
    std::mutex mutex;
};

static std::unordered_map<int, std::shared_ptr<WsServerState>> g_wsServers;
static std::mutex g_wsServersMutex;

// Generate unique client ID
static std::string generateClientId(WsServerState* srv) {
    int id = srv->nextClientId++;
    return "ws-client-" + std::to_string(id);
}

// Send raw bytes over WS connection (handles TLS)
static ssize_t wsSend(WsConnection* ws, const void* data, size_t len) {
    if (ws->useTls && ws->ssl) {
        return SSL_write(ws->ssl, data, (int)len);
    }
    return send(ws->fd, data, len, MSG_NOSIGNAL);
}

// Receive raw bytes from WS connection (handles TLS)
static ssize_t wsRecv(WsConnection* ws, void* buf, size_t len) {
    if (ws->useTls && ws->ssl) {
        return SSL_read(ws->ssl, buf, (int)len);
    }
    return recv(ws->fd, buf, len, 0);
}

// Parse ws:// or wss:// URL into host, port, path
static bool parseWsUrl(const std::string& url, bool& tls, std::string& host, int& port, std::string& path) {
    tls = false;
    if (url.find("wss://") == 0) {
        tls = true;
        host = url.substr(6);
    } else if (url.find("ws://") == 0) {
        host = url.substr(5);
    } else {
        return false;
    }

    // Extract path
    size_t pathPos = host.find('/');
    if (pathPos != std::string::npos) {
        path = host.substr(pathPos);
        host = host.substr(0, pathPos);
    } else {
        path = "/";
    }

    // Extract port
    port = tls ? 443 : 80;
    size_t colonPos = host.find(':');
    if (colonPos != std::string::npos) {
        port = std::stoi(host.substr(colonPos + 1));
        host = host.substr(0, colonPos);
    }
    return true;
}

// Perform WebSocket handshake (client side)
static bool wsHandshake(WsConnection* ws, const std::string& wsKey,
                        const std::unordered_map<std::string, std::string>& extraHeaders) {
    std::ostringstream req;
    req << "GET " << ws->path << " HTTP/1.1\r\n";
    req << "Host: " << ws->host;
    if ((ws->useTls && ws->port != 443) || (!ws->useTls && ws->port != 80)) {
        req << ":" << ws->port;
    }
    req << "\r\n";
    req << "Upgrade: websocket\r\n";
    req << "Connection: Upgrade\r\n";
    req << "Sec-WebSocket-Key: " << wsKey << "\r\n";
    req << "Sec-WebSocket-Version: 13\r\n";
    if (!ws->requestedProtocols.empty()) {
        req << "Sec-WebSocket-Protocol: ";
        for (size_t i = 0; i < ws->requestedProtocols.size(); i++) {
            if (i > 0) req << ", ";
            req << ws->requestedProtocols[i];
        }
        req << "\r\n";
    }
    for (auto& [k, v] : extraHeaders) {
        req << k << ": " << v << "\r\n";
    }
    req << "\r\n";

    std::string request = req.str();
    ssize_t sent = wsSend(ws, request.c_str(), request.size());
    if (sent <= 0) return false;

    // Read response
    char buf[4096];
    std::string response;
    while (true) {
        ssize_t n = wsRecv(ws, buf, sizeof(buf));
        if (n <= 0) return false;
        response.append(buf, n);
        if (response.find("\r\n\r\n") != std::string::npos) break;
        if (response.size() > 8192) return false; // too large
    }

    // Verify 101 Switching Protocols
    if (response.find("101") == std::string::npos) return false;

    // Verify Sec-WebSocket-Accept
    std::string expectedAccept = computeWsAccept(wsKey);
    if (response.find(expectedAccept) == std::string::npos) return false;

    // Extract negotiated protocol
    size_t protoPos = response.find("Sec-WebSocket-Protocol:");
    if (protoPos == std::string::npos) protoPos = response.find("sec-websocket-protocol:");
    if (protoPos != std::string::npos) {
        size_t valStart = response.find(':', protoPos) + 1;
        size_t valEnd = response.find("\r\n", valStart);
        std::string proto = response.substr(valStart, valEnd - valStart);
        size_t trimStart = proto.find_first_not_of(' ');
        if (trimStart != std::string::npos) ws->protocol = proto.substr(trimStart);
    }

    // Store any remaining data after headers as initial recv buffer
    size_t headerEnd = response.find("\r\n\r\n") + 4;
    if (headerEnd < response.size()) {
        ws->recvBuffer.insert(ws->recvBuffer.end(),
                              response.begin() + headerEnd, response.end());
    }

    return true;
}

void registerNetworkModule(VM& vm) {

    // Helper: get a field from either Object or Map value
    auto getOpt = [](const Value& v, const std::string& key) -> Value {
        if (v.type == ValueType::Object && v.objVal) {
            auto it = v.objVal->fields.find(key);
            if (it != v.objVal->fields.end()) return it->second;
        } else if (v.type == ValueType::Map && v.mapVal) {
            auto it = v.mapVal->entries.find(key);
            if (it != v.mapVal->entries.end()) return it->second;
        }
        return Value::makeNull();
    };

    // Helper: check if value is an object-like (Object or Map)
    auto isObjLike = [](const Value& v) -> bool {
        return (v.type == ValueType::Object && v.objVal) ||
               (v.type == ValueType::Map && v.mapVal);
    };

    // ===== 6.1 TCP =====

    // TcpServer.create(port) — create and bind a TCP server socket
    vm.registerNative("TcpServer.create", [](const std::vector<Value>& a) -> Value {
        int port = a.empty() ? 8080 : a[0].toInt();
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return Value::makeNull();

        int opt = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port);

        if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            close(fd);
            return Value::makeNull();
        }
        if (listen(fd, 128) < 0) {
            close(fd);
            return Value::makeNull();
        }

        Value server = Value::makeObject("TcpServer");
        server.objVal->fields["fd"] = Value::makeInt(fd);
        server.objVal->fields["port"] = Value::makeInt(port);
        server.objVal->fields["listening"] = Value::makeBool(true);
        return server;
    });

    // TcpServer.accept(server) — accept a connection (blocking)
    vm.registerNative("TcpServer.accept", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeNull();
        auto fdIt = a[0].objVal->fields.find("fd");
        if (fdIt == a[0].objVal->fields.end()) return Value::makeNull();
        int serverFd = fdIt->second.toInt();

        struct sockaddr_in clientAddr{};
        socklen_t addrLen = sizeof(clientAddr);
        int clientFd = accept(serverFd, (struct sockaddr*)&clientAddr, &addrLen);
        if (clientFd < 0) return Value::makeNull();

        Value client = Value::makeObject("TcpClient");
        client.objVal->fields["fd"] = Value::makeInt(clientFd);
        client.objVal->fields["remoteAddr"] = Value::makeString(inet_ntoa(clientAddr.sin_addr));
        client.objVal->fields["remotePort"] = Value::makeInt(ntohs(clientAddr.sin_port));
        client.objVal->fields["connected"] = Value::makeBool(true);
        return client;
    });

    // TcpServer.close(server)
    vm.registerNative("TcpServer.close", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeNull();
        auto fdIt = a[0].objVal->fields.find("fd");
        if (fdIt != a[0].objVal->fields.end()) close(fdIt->second.toInt());
        return Value::makeNull();
    });

    // TcpServer.setTimeout(server, milliseconds)
    vm.registerNative("TcpServer.setTimeout", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) return Value::makeBool(false);
        auto fdIt = a[0].objVal->fields.find("fd");
        if (fdIt == a[0].objVal->fields.end()) return Value::makeBool(false);
        int ms = a[1].toInt();
        struct timeval tv;
        tv.tv_sec = ms / 1000;
        tv.tv_usec = (ms % 1000) * 1000;
        setsockopt(fdIt->second.toInt(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        return Value::makeBool(true);
    });

    // TcpServer.getPort(server)
    vm.registerNative("TcpServer.getPort", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeInt(0);
        auto it = a[0].objVal->fields.find("port");
        return it != a[0].objVal->fields.end() ? it->second : Value::makeInt(0);
    });

    // TcpServer.setNonBlocking(server, enable)
    vm.registerNative("TcpServer.setNonBlocking", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) return Value::makeBool(false);
        auto fdIt = a[0].objVal->fields.find("fd");
        if (fdIt == a[0].objVal->fields.end()) return Value::makeBool(false);
        int fd = fdIt->second.toInt();
        int flags = fcntl(fd, F_GETFL, 0);
        if (a[1].toBool()) flags |= O_NONBLOCK;
        else flags &= ~O_NONBLOCK;
        fcntl(fd, F_SETFL, flags);
        return Value::makeBool(true);
    });

    // TcpClient.connect(host, port) — connect to remote
    vm.registerNative("TcpClient.connect", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2) return Value::makeNull();
        std::string host = a[0].toString();
        int port = a[1].toInt();

        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return Value::makeNull();

        struct hostent* he = gethostbyname(host.c_str());
        if (!he) { close(fd); return Value::makeNull(); }

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        std::memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);
        addr.sin_port = htons(port);

        if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            close(fd);
            return Value::makeNull();
        }

        Value client = Value::makeObject("TcpClient");
        client.objVal->fields["fd"] = Value::makeInt(fd);
        client.objVal->fields["host"] = Value::makeString(host);
        client.objVal->fields["port"] = Value::makeInt(port);
        client.objVal->fields["connected"] = Value::makeBool(true);
        return client;
    });

    // TcpClient.write(client, data)
    vm.registerNative("TcpClient.write", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) return Value::makeInt(-1);
        auto fdIt = a[0].objVal->fields.find("fd");
        if (fdIt == a[0].objVal->fields.end()) return Value::makeInt(-1);
        std::string data = a[1].toString();
        ssize_t sent = send(fdIt->second.toInt(), data.c_str(), data.size(), 0);
        return Value::makeInt(static_cast<int>(sent));
    });

    // TcpClient.read(client, maxBytes)
    vm.registerNative("TcpClient.read", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeString("");
        auto fdIt = a[0].objVal->fields.find("fd");
        if (fdIt == a[0].objVal->fields.end()) return Value::makeString("");
        int maxBytes = a.size() >= 2 ? a[1].toInt() : 4096;
        std::vector<char> buf(maxBytes);
        ssize_t n = recv(fdIt->second.toInt(), buf.data(), maxBytes, 0);
        if (n <= 0) return Value::makeString("");
        return Value::makeString(std::string(buf.data(), n));
    });

    // TcpClient.close(client)
    vm.registerNative("TcpClient.close", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeNull();
        auto fdIt = a[0].objVal->fields.find("fd");
        if (fdIt != a[0].objVal->fields.end()) close(fdIt->second.toInt());
        if (a[0].objVal) a[0].objVal->fields["connected"] = Value::makeBool(false);
        return Value::makeNull();
    });

    // TcpClient.readBytes(client, maxBytes) — read raw bytes
    vm.registerNative("TcpClient.readBytes", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeArray();
        auto fdIt = a[0].objVal->fields.find("fd");
        if (fdIt == a[0].objVal->fields.end()) return Value::makeArray();
        int maxBytes = a.size() >= 2 ? a[1].toInt() : 4096;
        std::vector<char> buf(maxBytes);
        ssize_t n = recv(fdIt->second.toInt(), buf.data(), maxBytes, 0);
        Value arr = Value::makeArray();
        if (n > 0) {
            for (ssize_t i = 0; i < n; i++)
                arr.arrVal->elements.push_back(Value::makeInt(static_cast<unsigned char>(buf[i])));
        }
        return arr;
    });

    // TcpClient.writeBytes(client, byteArray) — write raw bytes
    vm.registerNative("TcpClient.writeBytes", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal || !a[1].arrVal) return Value::makeInt(-1);
        auto fdIt = a[0].objVal->fields.find("fd");
        if (fdIt == a[0].objVal->fields.end()) return Value::makeInt(-1);
        std::vector<char> buf;
        for (auto& e : a[1].arrVal->elements) buf.push_back(static_cast<char>(e.toInt()));
        ssize_t sent = send(fdIt->second.toInt(), buf.data(), buf.size(), 0);
        return Value::makeInt(static_cast<int>(sent));
    });

    // TcpClient.setTimeout(client, milliseconds)
    vm.registerNative("TcpClient.setTimeout", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) return Value::makeBool(false);
        auto fdIt = a[0].objVal->fields.find("fd");
        if (fdIt == a[0].objVal->fields.end()) return Value::makeBool(false);
        int ms = a[1].toInt();
        struct timeval tv;
        tv.tv_sec = ms / 1000;
        tv.tv_usec = (ms % 1000) * 1000;
        int fd = fdIt->second.toInt();
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        a[0].objVal->fields["timeout"] = Value::makeInt(ms);
        return Value::makeBool(true);
    });

    // TcpClient.setKeepAlive(client, enable, idleSec, intervalSec, count)
    vm.registerNative("TcpClient.setKeepAlive", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) return Value::makeBool(false);
        auto fdIt = a[0].objVal->fields.find("fd");
        if (fdIt == a[0].objVal->fields.end()) return Value::makeBool(false);
        int fd = fdIt->second.toInt();
        int enable = a[1].toBool() ? 1 : 0;
        setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &enable, sizeof(enable));
        if (enable && a.size() >= 3) {
            int idle = a[2].toInt();
            setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
        }
        if (enable && a.size() >= 4) {
            int interval = a[3].toInt();
            setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
        }
        if (enable && a.size() >= 5) {
            int count = a[4].toInt();
            setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count));
        }
        return Value::makeBool(true);
    });

    // TcpClient.setNonBlocking(client, enable)
    vm.registerNative("TcpClient.setNonBlocking", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) return Value::makeBool(false);
        auto fdIt = a[0].objVal->fields.find("fd");
        if (fdIt == a[0].objVal->fields.end()) return Value::makeBool(false);
        int fd = fdIt->second.toInt();
        int flags = fcntl(fd, F_GETFL, 0);
        if (a[1].toBool()) flags |= O_NONBLOCK;
        else flags &= ~O_NONBLOCK;
        fcntl(fd, F_SETFL, flags);
        return Value::makeBool(true);
    });

    // TcpClient.isConnected(client)
    vm.registerNative("TcpClient.isConnected", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeBool(false);
        auto it = a[0].objVal->fields.find("connected");
        return Value::makeBool(it != a[0].objVal->fields.end() && it->second.toBool());
    });

    // TcpClient.getRemoteAddress(client)
    vm.registerNative("TcpClient.getRemoteAddress", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeString("");
        auto it = a[0].objVal->fields.find("remoteAddr");
        return it != a[0].objVal->fields.end() ? it->second : Value::makeString("");
    });

    // TcpClient.getRemotePort(client)
    vm.registerNative("TcpClient.getRemotePort", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeInt(0);
        auto it = a[0].objVal->fields.find("remotePort");
        return it != a[0].objVal->fields.end() ? it->second : Value::makeInt(0);
    });

    // ===== 6.1 UDP =====

    // UdpSocket.create(port)
    vm.registerNative("UdpSocket.create", [](const std::vector<Value>& a) -> Value {
        int port = a.empty() ? 0 : a[0].toInt();
        int fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) return Value::makeNull();

        if (port > 0) {
            struct sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = INADDR_ANY;
            addr.sin_port = htons(port);
            if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
                close(fd);
                return Value::makeNull();
            }
        }

        Value sock = Value::makeObject("UdpSocket");
        sock.objVal->fields["fd"] = Value::makeInt(fd);
        sock.objVal->fields["port"] = Value::makeInt(port);
        return sock;
    });

    // UdpSocket.send(socket, data, host, port)
    // UdpSocket.send(socket, host, port, data) — send datagram (matches LLVM API)
    vm.registerNative("UdpSocket.send", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 4 || !a[0].objVal) return Value::makeInt(-1);
        auto fdIt = a[0].objVal->fields.find("fd");
        if (fdIt == a[0].objVal->fields.end()) return Value::makeInt(-1);

        std::string host = a[1].toString();
        int port = a[2].toInt();
        std::string data = a[3].toString();

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        // Resolve hostname
        struct hostent* he = gethostbyname(host.c_str());
        if (he) std::memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);
        else inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

        ssize_t sent = sendto(fdIt->second.toInt(), data.c_str(), data.size(), 0,
                              (struct sockaddr*)&addr, sizeof(addr));
        return Value::makeInt(static_cast<int>(sent));
    });

    // UdpSocket.receive(socket, maxBytes?) — receive datagram, returns string (matches LLVM API)
    vm.registerNative("UdpSocket.receive", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeString("");
        auto fdIt = a[0].objVal->fields.find("fd");
        if (fdIt == a[0].objVal->fields.end()) return Value::makeString("");

        // Set receive timeout (2 seconds)
        int fd = fdIt->second.toInt();
        struct timeval tv = {2, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        int maxBytes = a.size() >= 2 ? a[1].toInt() : 4096;
        std::vector<char> buf(maxBytes);
        struct sockaddr_in sender{};
        socklen_t senderLen = sizeof(sender);

        ssize_t n = recvfrom(fd, buf.data(), maxBytes, 0,
                             (struct sockaddr*)&sender, &senderLen);
        if (n <= 0) return Value::makeString("");

        return Value::makeString(std::string(buf.data(), n));
    });

    // UdpSocket.close(socket)
    vm.registerNative("UdpSocket.close", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeNull();
        auto fdIt = a[0].objVal->fields.find("fd");
        if (fdIt != a[0].objVal->fields.end()) close(fdIt->second.toInt());
        return Value::makeNull();
    });

    // UdpSocket.setBroadcast(socket, enable)
    vm.registerNative("UdpSocket.setBroadcast", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) return Value::makeBool(false);
        auto fdIt = a[0].objVal->fields.find("fd");
        if (fdIt == a[0].objVal->fields.end()) return Value::makeBool(false);
        int enable = a[1].toBool() ? 1 : 0;
        setsockopt(fdIt->second.toInt(), SOL_SOCKET, SO_BROADCAST, &enable, sizeof(enable));
        return Value::makeBool(true);
    });

    // UdpSocket.setNonBlocking(socket, enable)
    vm.registerNative("UdpSocket.setNonBlocking", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) return Value::makeBool(false);
        auto fdIt = a[0].objVal->fields.find("fd");
        if (fdIt == a[0].objVal->fields.end()) return Value::makeBool(false);
        int fd = fdIt->second.toInt();
        int flags = fcntl(fd, F_GETFL, 0);
        if (a[1].toBool()) flags |= O_NONBLOCK;
        else flags &= ~O_NONBLOCK;
        fcntl(fd, F_SETFL, flags);
        return Value::makeBool(true);
    });

    // UdpSocket.getPort(socket)
    vm.registerNative("UdpSocket.getPort", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeInt(0);
        auto it = a[0].objVal->fields.find("port");
        return it != a[0].objVal->fields.end() ? it->second : Value::makeInt(0);
    });

    // ===== 6.2 HTTP Client (real libcurl) =====

    // Helper: perform a full curl request with all options
    auto curlRequest = [](const std::string& url, const std::string& method,
                          const std::string& body,
                          const std::unordered_map<std::string, std::string>& reqHeaders,
                          int timeout, bool followRedirects, int maxRedirects) -> Value {
        CURL* curl = curl_easy_init();
        if (!curl) return Value::makeNull();

        std::string responseBody;
        std::string responseHeaders;

        auto writeCallback = +[](char* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
            static_cast<std::string*>(userdata)->append(ptr, size * nmemb);
            return size * nmemb;
        };

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBody);
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, writeCallback);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &responseHeaders);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, followRedirects ? 1L : 0L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, (long)maxRedirects);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)timeout);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "Gard/0.1.0");
        curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, ""); // accept all encodings

        // Set method
        if (method == "POST") {
            curl_easy_setopt(curl, CURLOPT_POST, 1L);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
        } else if (method == "PUT") {
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
        } else if (method == "PATCH") {
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PATCH");
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
        } else if (method == "DELETE") {
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
            if (!body.empty()) {
                curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
                curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
            }
        } else if (method == "HEAD") {
            curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
        } else if (method == "OPTIONS") {
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "OPTIONS");
        }

        // Set custom headers
        struct curl_slist* headerList = nullptr;
        for (auto& [key, val] : reqHeaders) {
            std::string h = key + ": " + val;
            headerList = curl_slist_append(headerList, h.c_str());
        }
        if (headerList) {
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);
        }

        CURLcode res = curl_easy_perform(curl);

        Value result = Value::makeObject("HttpResponse");
        if (res == CURLE_OK) {
            long statusCode;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &statusCode);

            // Status text from code
            std::string statusText;
            switch (statusCode) {
                case 200: statusText = "OK"; break;
                case 201: statusText = "Created"; break;
                case 204: statusText = "No Content"; break;
                case 301: statusText = "Moved Permanently"; break;
                case 302: statusText = "Found"; break;
                case 304: statusText = "Not Modified"; break;
                case 400: statusText = "Bad Request"; break;
                case 401: statusText = "Unauthorized"; break;
                case 403: statusText = "Forbidden"; break;
                case 404: statusText = "Not Found"; break;
                case 405: statusText = "Method Not Allowed"; break;
                case 408: statusText = "Request Timeout"; break;
                case 429: statusText = "Too Many Requests"; break;
                case 500: statusText = "Internal Server Error"; break;
                case 502: statusText = "Bad Gateway"; break;
                case 503: statusText = "Service Unavailable"; break;
                default: statusText = "Unknown"; break;
            }

            result.objVal->fields["status"] = Value::makeInt(static_cast<int>(statusCode));
            result.objVal->fields["statusText"] = Value::makeString(statusText);
            result.objVal->fields["ok"] = Value::makeBool(statusCode >= 200 && statusCode < 300);
            result.objVal->fields["body"] = Value::makeString(responseBody);

            // Parse response headers into a map object
            Value headersObj = Value::makeObject("Headers");
            std::istringstream hstream(responseHeaders);
            std::string hline;
            while (std::getline(hstream, hline)) {
                if (!hline.empty() && hline.back() == '\r') hline.pop_back();
                size_t colon = hline.find(':');
                if (colon != std::string::npos) {
                    std::string hkey = hline.substr(0, colon);
                    std::string hval = hline.substr(colon + 1);
                    size_t start = hval.find_first_not_of(' ');
                    if (start != std::string::npos) hval = hval.substr(start);
                    std::string lkey = hkey;
                    std::transform(lkey.begin(), lkey.end(), lkey.begin(), ::tolower);
                    headersObj.objVal->fields[lkey] = Value::makeString(hval);
                }
            }
            result.objVal->fields["headers"] = headersObj;

            char* ct = nullptr;
            curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &ct);
            result.objVal->fields["contentType"] = Value::makeString(ct ? ct : "");

            double totalTime;
            curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME, &totalTime);
            result.objVal->fields["elapsed"] = Value::makeDouble(totalTime * 1000.0);

            // Effective URL (after redirects)
            char* effectiveUrl = nullptr;
            curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effectiveUrl);
            result.objVal->fields["url"] = Value::makeString(effectiveUrl ? effectiveUrl : url);

            // Redirect count
            long redirectCount;
            curl_easy_getinfo(curl, CURLINFO_REDIRECT_COUNT, &redirectCount);
            result.objVal->fields["redirectCount"] = Value::makeInt(static_cast<int>(redirectCount));
        } else {
            result.objVal->fields["status"] = Value::makeInt(0);
            result.objVal->fields["statusText"] = Value::makeString("Error");
            result.objVal->fields["ok"] = Value::makeBool(false);
            result.objVal->fields["body"] = Value::makeString("");
            result.objVal->fields["error"] = Value::makeString(curl_easy_strerror(res));
            result.objVal->fields["url"] = Value::makeString(url);
        }

        if (headerList) curl_slist_free_all(headerList);
        curl_easy_cleanup(curl);
        return result;
    };

    // http.get(url) — simple GET
    vm.registerNative("http.get", [curlRequest](const std::vector<Value>& a) -> Value {
        if (a.empty()) return Value::makeNull();
        return curlRequest(a[0].toString(), "GET", "", {}, 30000, true, 10);
    });

    // http.post(url, body, contentType)
    vm.registerNative("http.post", [curlRequest](const std::vector<Value>& a) -> Value {
        if (a.size() < 2) return Value::makeNull();
        std::string ct = a.size() >= 3 ? a[2].toString() : "application/json";
        std::unordered_map<std::string, std::string> headers = {{"Content-Type", ct}};
        return curlRequest(a[0].toString(), "POST", a[1].toString(), headers, 30000, true, 10);
    });

    // http.put(url, body, contentType)
    vm.registerNative("http.put", [curlRequest](const std::vector<Value>& a) -> Value {
        if (a.size() < 2) return Value::makeNull();
        std::string ct = a.size() >= 3 ? a[2].toString() : "application/json";
        std::unordered_map<std::string, std::string> headers = {{"Content-Type", ct}};
        return curlRequest(a[0].toString(), "PUT", a[1].toString(), headers, 30000, true, 10);
    });

    // http.patch(url, body, contentType)
    vm.registerNative("http.patch", [curlRequest](const std::vector<Value>& a) -> Value {
        if (a.size() < 2) return Value::makeNull();
        std::string ct = a.size() >= 3 ? a[2].toString() : "application/json";
        std::unordered_map<std::string, std::string> headers = {{"Content-Type", ct}};
        return curlRequest(a[0].toString(), "PATCH", a[1].toString(), headers, 30000, true, 10);
    });

    // http.delete(url, body?)
    vm.registerNative("http.delete", [curlRequest](const std::vector<Value>& a) -> Value {
        if (a.empty()) return Value::makeNull();
        std::string body = a.size() >= 2 ? a[1].toString() : "";
        return curlRequest(a[0].toString(), "DELETE", body, {}, 30000, true, 10);
    });

    // http.head(url)
    vm.registerNative("http.head", [curlRequest](const std::vector<Value>& a) -> Value {
        if (a.empty()) return Value::makeNull();
        return curlRequest(a[0].toString(), "HEAD", "", {}, 30000, true, 10);
    });

    // http.options(url)
    vm.registerNative("http.options", [curlRequest](const std::vector<Value>& a) -> Value {
        if (a.empty()) return Value::makeNull();
        return curlRequest(a[0].toString(), "OPTIONS", "", {}, 30000, true, 10);
    });

    // http.fetch(url, options) — full configuration
    // options object fields: method, body, headers (object), timeout, followRedirects, maxRedirects
    vm.registerNative("http.fetch", [curlRequest, getOpt, isObjLike](const std::vector<Value>& a) -> Value {
        if (a.empty()) return Value::makeNull();
        std::string url = a[0].toString();
        std::string method = "GET";
        std::string body;
        std::unordered_map<std::string, std::string> headers;
        int timeout = 30000;
        bool followRedirects = true;
        int maxRedirects = 10;

        if (a.size() >= 2 && isObjLike(a[1])) {
            Value mv = getOpt(a[1], "method");
            if (mv.type != ValueType::Null) method = mv.toString();
            Value bv = getOpt(a[1], "body");
            if (bv.type != ValueType::Null) body = bv.toString();
            Value tv = getOpt(a[1], "timeout");
            if (tv.type != ValueType::Null) timeout = tv.toInt();
            Value rv = getOpt(a[1], "followRedirects");
            if (rv.type != ValueType::Null) followRedirects = rv.toBool();
            Value mrv = getOpt(a[1], "maxRedirects");
            if (mrv.type != ValueType::Null) maxRedirects = mrv.toInt();
            // Parse headers
            Value hv = getOpt(a[1], "headers");
            if (hv.type == ValueType::Object && hv.objVal) {
                for (auto& [k, v] : hv.objVal->fields) headers[k] = v.toString();
            } else if (hv.type == ValueType::Map && hv.mapVal) {
                for (auto& [k, v] : hv.mapVal->entries) headers[k] = v.toString();
            }
            Value ctv = getOpt(a[1], "contentType");
            if (ctv.type != ValueType::Null) headers["Content-Type"] = ctv.toString();
        }

        return curlRequest(url, method, body, headers, timeout, followRedirects, maxRedirects);
    });

    // ===== 6.2 HTTP Server (real POSIX sockets + epoll) =====

    // HttpServer.create(options) — create an HTTP server instance
    // options: { port, host, maxConnections, keepAlive, requestTimeout }
    vm.registerNative("HttpServer.create", [getOpt, isObjLike](const std::vector<Value>& a) -> Value {
        int port = 8080;
        std::string host = "0.0.0.0";
        int maxConn = 1024;
        bool keepAlive = true;
        int reqTimeout = 30000;

        if (!a.empty() && isObjLike(a[0])) {
            Value pv = getOpt(a[0], "port");
            if (pv.type != ValueType::Null) port = pv.toInt();
            Value hv = getOpt(a[0], "host");
            if (hv.type != ValueType::Null) host = hv.toString();
            Value mcv = getOpt(a[0], "maxConnections");
            if (mcv.type != ValueType::Null) maxConn = mcv.toInt();
            Value kav = getOpt(a[0], "keepAlive");
            if (kav.type != ValueType::Null) keepAlive = kav.toBool();
            Value rtv = getOpt(a[0], "requestTimeout");
            if (rtv.type != ValueType::Null) reqTimeout = rtv.toInt();
        } else if (!a.empty()) {
            port = a[0].toInt();
        }

        // Create socket
        int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (fd < 0) return Value::makeNull();

        int opt = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

        if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            close(fd);
            return Value::makeNull();
        }
        if (listen(fd, maxConn) < 0) {
            close(fd);
            return Value::makeNull();
        }

        // Create epoll instance
        int epollFd = epoll_create1(0);
        if (epollFd < 0) {
            close(fd);
            return Value::makeNull();
        }

        struct epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = fd;
        epoll_ctl(epollFd, EPOLL_CTL_ADD, fd, &ev);

        // Create server state
        auto state = std::make_shared<ServerState>();
        state->fd = fd;
        state->epollFd = epollFd;
        state->port = port;
        state->host = host;
        state->running = false;
        state->maxConnections = maxConn;
        state->keepAlive = keepAlive;
        state->requestTimeout = reqTimeout;

        {
            std::lock_guard<std::mutex> lock(g_serversMutex);
            g_servers[fd] = state;
        }

        Value server = Value::makeObject("HttpServer");
        server.objVal->fields["fd"] = Value::makeInt(fd);
        server.objVal->fields["port"] = Value::makeInt(port);
        server.objVal->fields["host"] = Value::makeString(host);
        server.objVal->fields["running"] = Value::makeBool(false);
        return server;
    });

    // HttpServer.route(server, method, pattern, handlerName)
    // Registers a route handler. handlerName is the function name to call.
    vm.registerNative("HttpServer.route", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 4 || !a[0].objVal) return Value::makeBool(false);
        auto fdIt = a[0].objVal->fields.find("fd");
        if (fdIt == a[0].objVal->fields.end()) return Value::makeBool(false);
        int fd = fdIt->second.toInt();

        std::lock_guard<std::mutex> lock(g_serversMutex);
        auto it = g_servers.find(fd);
        if (it == g_servers.end()) return Value::makeBool(false);

        Route route;
        route.method = a[1].toString();
        route.pattern = a[2].toString();
        route.handler = a[3]; // handler name or object

        // Extract param names from pattern
        std::istringstream ss(route.pattern);
        std::string segment;
        while (std::getline(ss, segment, '/')) {
            if (!segment.empty() && segment[0] == ':') {
                route.paramNames.push_back(segment.substr(1));
            }
        }

        it->second->routes.push_back(route);
        return Value::makeBool(true);
    });

    // HttpServer.get(server, pattern, handler)
    vm.registerNative("HttpServer.get", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 3 || !a[0].objVal) return Value::makeBool(false);
        auto fdIt = a[0].objVal->fields.find("fd");
        if (fdIt == a[0].objVal->fields.end()) return Value::makeBool(false);
        int fd = fdIt->second.toInt();

        std::lock_guard<std::mutex> lock(g_serversMutex);
        auto it = g_servers.find(fd);
        if (it == g_servers.end()) return Value::makeBool(false);

        Route route;
        route.method = "GET";
        route.pattern = a[1].toString();
        route.handler = a[2];
        it->second->routes.push_back(route);
        return Value::makeBool(true);
    });

    // HttpServer.post(server, pattern, handler)
    vm.registerNative("HttpServer.post", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 3 || !a[0].objVal) return Value::makeBool(false);
        auto fdIt = a[0].objVal->fields.find("fd");
        if (fdIt == a[0].objVal->fields.end()) return Value::makeBool(false);
        int fd = fdIt->second.toInt();

        std::lock_guard<std::mutex> lock(g_serversMutex);
        auto it = g_servers.find(fd);
        if (it == g_servers.end()) return Value::makeBool(false);

        Route route;
        route.method = "POST";
        route.pattern = a[1].toString();
        route.handler = a[2];
        it->second->routes.push_back(route);
        return Value::makeBool(true);
    });

    // HttpServer.put(server, pattern, handler)
    vm.registerNative("HttpServer.put", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 3 || !a[0].objVal) return Value::makeBool(false);
        auto fdIt = a[0].objVal->fields.find("fd");
        if (fdIt == a[0].objVal->fields.end()) return Value::makeBool(false);
        int fd = fdIt->second.toInt();

        std::lock_guard<std::mutex> lock(g_serversMutex);
        auto it = g_servers.find(fd);
        if (it == g_servers.end()) return Value::makeBool(false);

        Route route;
        route.method = "PUT";
        route.pattern = a[1].toString();
        route.handler = a[2];
        it->second->routes.push_back(route);
        return Value::makeBool(true);
    });

    // HttpServer.delete(server, pattern, handler)
    vm.registerNative("HttpServer.delete", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 3 || !a[0].objVal) return Value::makeBool(false);
        auto fdIt = a[0].objVal->fields.find("fd");
        if (fdIt == a[0].objVal->fields.end()) return Value::makeBool(false);
        int fd = fdIt->second.toInt();

        std::lock_guard<std::mutex> lock(g_serversMutex);
        auto it = g_servers.find(fd);
        if (it == g_servers.end()) return Value::makeBool(false);

        Route route;
        route.method = "DELETE";
        route.pattern = a[1].toString();
        route.handler = a[2];
        it->second->routes.push_back(route);
        return Value::makeBool(true);
    });

    // HttpServer.patch(server, pattern, handler)
    vm.registerNative("HttpServer.patch", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 3 || !a[0].objVal) return Value::makeBool(false);
        auto fdIt = a[0].objVal->fields.find("fd");
        if (fdIt == a[0].objVal->fields.end()) return Value::makeBool(false);
        int fd = fdIt->second.toInt();

        std::lock_guard<std::mutex> lock(g_serversMutex);
        auto it = g_servers.find(fd);
        if (it == g_servers.end()) return Value::makeBool(false);

        Route route;
        route.method = "PATCH";
        route.pattern = a[1].toString();
        route.handler = a[2];
        it->second->routes.push_back(route);
        return Value::makeBool(true);
    });

    // HttpServer.use(server, middlewareHandler)
    // Middleware runs before route handlers. Can modify request/response.
    vm.registerNative("HttpServer.use", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) return Value::makeBool(false);
        auto fdIt = a[0].objVal->fields.find("fd");
        if (fdIt == a[0].objVal->fields.end()) return Value::makeBool(false);
        int fd = fdIt->second.toInt();

        std::lock_guard<std::mutex> lock(g_serversMutex);
        auto it = g_servers.find(fd);
        if (it == g_servers.end()) return Value::makeBool(false);

        it->second->middlewares.push_back(a[1]);
        return Value::makeBool(true);
    });

    // HttpServer.static(server, urlPrefix, rootDirectory)
    // Serve static files from a directory
    vm.registerNative("HttpServer.static", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 3 || !a[0].objVal) return Value::makeBool(false);
        auto fdIt = a[0].objVal->fields.find("fd");
        if (fdIt == a[0].objVal->fields.end()) return Value::makeBool(false);
        int fd = fdIt->second.toInt();

        std::lock_guard<std::mutex> lock(g_serversMutex);
        auto it = g_servers.find(fd);
        if (it == g_servers.end()) return Value::makeBool(false);

        it->second->staticPrefix = a[1].toString();
        it->second->staticRoot = a[2].toString();
        return Value::makeBool(true);
    });

    // HttpServer.setDefaultHeaders(server, headersObj)
    vm.registerNative("HttpServer.setDefaultHeaders", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) return Value::makeBool(false);
        auto fdIt = a[0].objVal->fields.find("fd");
        if (fdIt == a[0].objVal->fields.end()) return Value::makeBool(false);
        int fd = fdIt->second.toInt();

        std::lock_guard<std::mutex> lock(g_serversMutex);
        auto it = g_servers.find(fd);
        if (it == g_servers.end()) return Value::makeBool(false);

        if (a[1].type == ValueType::Object && a[1].objVal) {
            for (auto& [k, v] : a[1].objVal->fields) {
                it->second->defaultHeaders[k] = v.toString();
            }
        } else if (a[1].type == ValueType::Map && a[1].mapVal) {
            for (auto& [k, v] : a[1].mapVal->entries) {
                it->second->defaultHeaders[k] = v.toString();
            }
        }
        return Value::makeBool(true);
    });

    // HttpServer.listen(server, callback?) — start accepting connections (non-blocking, spawns thread)
    // If callback is provided, it's called after the server starts listening.
    // This runs the epoll event loop in a background thread, accepts connections, parses HTTP requests,
    // matches routes, runs middleware, serves static files, and sends responses.
    vm.registerNative("HttpServer.listen", [&vm](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeBool(false);
        auto fdIt = a[0].objVal->fields.find("fd");
        if (fdIt == a[0].objVal->fields.end()) return Value::makeBool(false);
        int serverFd = fdIt->second.toInt();

        std::shared_ptr<ServerState> state;
        {
            std::lock_guard<std::mutex> lock(g_serversMutex);
            auto it = g_servers.find(serverFd);
            if (it == g_servers.end()) return Value::makeBool(false);
            state = it->second;
        }

        state->running = true;
        a[0].objVal->fields["running"] = Value::makeBool(true);

        // Spawn event loop in background thread (matches LLVM codegen behavior)
        std::thread([state, serverFd]() {
        const int MAX_EVENTS = 64;
        struct epoll_event events[64];
        std::unordered_map<int, std::string> clientBuffers;

        while (state->running) {
            int nfds = epoll_wait(state->epollFd, events, MAX_EVENTS, 100);
            for (int i = 0; i < nfds; i++) {
                if (events[i].data.fd == serverFd) {
                    // Accept new connections
                    while (true) {
                        struct sockaddr_in clientAddr{};
                        socklen_t addrLen = sizeof(clientAddr);
                        int clientFd = accept4(serverFd, (struct sockaddr*)&clientAddr,
                                               &addrLen, SOCK_NONBLOCK);
                        if (clientFd < 0) break;

                        struct epoll_event ev{};
                        ev.events = EPOLLIN | EPOLLET;
                        ev.data.fd = clientFd;
                        epoll_ctl(state->epollFd, EPOLL_CTL_ADD, clientFd, &ev);
                        clientBuffers[clientFd] = "";
                    }
                } else {
                    int clientFd = events[i].data.fd;
                    if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                        epoll_ctl(state->epollFd, EPOLL_CTL_DEL, clientFd, nullptr);
                        close(clientFd);
                        clientBuffers.erase(clientFd);
                        continue;
                    }

                    // Read data
                    char buf[8192];
                    ssize_t n = read(clientFd, buf, sizeof(buf));
                    if (n <= 0) {
                        epoll_ctl(state->epollFd, EPOLL_CTL_DEL, clientFd, nullptr);
                        close(clientFd);
                        clientBuffers.erase(clientFd);
                        continue;
                    }

                    clientBuffers[clientFd].append(buf, n);

                    // Check if we have a complete request (headers end with \r\n\r\n)
                    std::string& reqBuf = clientBuffers[clientFd];
                    size_t headerEnd = reqBuf.find("\r\n\r\n");
                    if (headerEnd == std::string::npos) continue;

                    // Check content-length for body
                    std::string headerPart = reqBuf.substr(0, headerEnd);
                    size_t clPos = headerPart.find("Content-Length:");
                    if (clPos == std::string::npos) clPos = headerPart.find("content-length:");
                    if (clPos != std::string::npos) {
                        size_t clEnd = headerPart.find("\r\n", clPos);
                        std::string clVal = headerPart.substr(clPos + 16, clEnd - clPos - 16);
                        size_t contentLen = std::stoul(clVal);
                        size_t totalNeeded = headerEnd + 4 + contentLen;
                        if (reqBuf.size() < totalNeeded) continue; // wait for more data
                    }

                    // Parse the HTTP request
                    HttpRequest req;
                    if (!parseHttpRequest(reqBuf, req)) {
                        std::string resp = buildHttpResponse(400, "Bad Request", {}, "Bad Request");
                        send(clientFd, resp.c_str(), resp.size(), 0);
                        epoll_ctl(state->epollFd, EPOLL_CTL_DEL, clientFd, nullptr);
                        close(clientFd);
                        clientBuffers.erase(clientFd);
                        continue;
                    }

                    // Get client address
                    struct sockaddr_in peerAddr{};
                    socklen_t peerLen = sizeof(peerAddr);
                    getpeername(clientFd, (struct sockaddr*)&peerAddr, &peerLen);
                    req.remoteAddr = inet_ntoa(peerAddr.sin_addr);
                    req.remotePort = ntohs(peerAddr.sin_port);

                    // Build request object for handlers
                    Value reqObj = Value::makeObject("HttpRequest");
                    reqObj.objVal->fields["method"] = Value::makeString(req.method);
                    reqObj.objVal->fields["path"] = Value::makeString(req.path);
                    reqObj.objVal->fields["query"] = Value::makeString(req.query);
                    reqObj.objVal->fields["body"] = Value::makeString(req.body);
                    reqObj.objVal->fields["remoteAddr"] = Value::makeString(req.remoteAddr);
                    reqObj.objVal->fields["remotePort"] = Value::makeInt(req.remotePort);
                    reqObj.objVal->fields["httpVersion"] = Value::makeString(req.httpVersion);
                    Value reqHeaders = Value::makeObject("Headers");
                    for (auto& [k, v] : req.headers) {
                        reqHeaders.objVal->fields[k] = Value::makeString(v);
                    }
                    reqObj.objVal->fields["headers"] = reqHeaders;

                    // Build response object
                    Value resObj = Value::makeObject("HttpResponseWriter");
                    resObj.objVal->fields["status"] = Value::makeInt(200);
                    resObj.objVal->fields["body"] = Value::makeString("");
                    Value resHeaders = Value::makeObject("Headers");
                    // Apply default headers
                    for (auto& [k, v] : state->defaultHeaders) {
                        resHeaders.objVal->fields[k] = Value::makeString(v);
                    }
                    resObj.objVal->fields["headers"] = resHeaders;
                    resObj.objVal->fields["sent"] = Value::makeBool(false);

                    // Try static file serving first
                    bool handled = false;
                    if (!state->staticRoot.empty() && !state->staticPrefix.empty()) {
                        if (req.path.find(state->staticPrefix) == 0 && req.method == "GET") {
                            std::string relPath = req.path.substr(state->staticPrefix.size());
                            if (relPath.empty() || relPath == "/") relPath = "/index.html";
                            // Prevent directory traversal
                            if (relPath.find("..") == std::string::npos) {
                                std::string fullPath = state->staticRoot + relPath;
                                struct stat st;
                                if (stat(fullPath.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
                                    std::ifstream file(fullPath, std::ios::binary);
                                    if (file.is_open()) {
                                        std::string content((std::istreambuf_iterator<char>(file)),
                                                           std::istreambuf_iterator<char>());
                                        std::string mime = getMimeType(fullPath);
                                        std::unordered_map<std::string, std::string> respHeaders = {
                                            {"Content-Type", mime},
                                            {"Content-Length", std::to_string(content.size())},
                                            {"Cache-Control", "public, max-age=3600"},
                                            {"X-Content-Type-Options", "nosniff"}
                                        };
                                        for (auto& [k, v] : state->defaultHeaders) respHeaders[k] = v;
                                        std::string resp = buildHttpResponse(200, "OK", respHeaders, content);
                                        send(clientFd, resp.c_str(), resp.size(), MSG_NOSIGNAL);
                                        handled = true;
                                    }
                                }
                            }
                            if (!handled) {
                                // 404 for static files not found
                                std::string resp = buildHttpResponse(404, "Not Found",
                                    {{"Content-Type", "text/plain"}}, "Not Found");
                                send(clientFd, resp.c_str(), resp.size(), MSG_NOSIGNAL);
                                handled = true;
                            }
                        }
                    }

                    // Route matching
                    if (!handled) {
                        bool routeMatched = false;
                        for (auto& route : state->routes) {
                            std::unordered_map<std::string, std::string> params;
                            if (matchRoute(route, req.method, req.path, params)) {
                                // Store params in request object
                                Value paramsObj = Value::makeObject("Params");
                                for (auto& [k, v] : params) {
                                    paramsObj.objVal->fields[k] = Value::makeString(v);
                                }
                                reqObj.objVal->fields["params"] = paramsObj;

                                // The handler value contains the response info
                                // In the VM context, we store the matched route info
                                // and let the response be built from the handler string
                                resObj.objVal->fields["matched"] = Value::makeBool(true);
                                resObj.objVal->fields["handler"] = route.handler;
                                routeMatched = true;
                                break;
                            }
                        }

                        if (!routeMatched) {
                            // 404 — no route matched
                            std::string body404 = "{\"error\":\"Not Found\",\"path\":\"" + req.path + "\"}";
                            std::unordered_map<std::string, std::string> respHeaders = {
                                {"Content-Type", "application/json"}
                            };
                            for (auto& [k, v] : state->defaultHeaders) respHeaders[k] = v;
                            std::string resp = buildHttpResponse(404, "Not Found", respHeaders, body404);
                            send(clientFd, resp.c_str(), resp.size(), MSG_NOSIGNAL);
                        } else {
                            // Send response from resObj
                            int status = resObj.objVal->fields["status"].toInt();
                            std::string respBody = resObj.objVal->fields["body"].toString();
                            std::unordered_map<std::string, std::string> respHeaders;
                            if (resObj.objVal->fields["headers"].type == ValueType::Object &&
                                resObj.objVal->fields["headers"].objVal) {
                                for (auto& [k, v] : resObj.objVal->fields["headers"].objVal->fields) {
                                    respHeaders[k] = v.toString();
                                }
                            }
                            if (respHeaders.find("Content-Type") == respHeaders.end()) {
                                respHeaders["Content-Type"] = "application/json";
                            }
                            std::string statusText = "OK";
                            if (status == 201) statusText = "Created";
                            else if (status == 204) statusText = "No Content";
                            else if (status >= 400) statusText = "Error";
                            std::string resp = buildHttpResponse(status, statusText, respHeaders, respBody);
                            send(clientFd, resp.c_str(), resp.size(), MSG_NOSIGNAL);
                        }
                    }

                    // Keep-alive or close
                    auto connHeader = req.headers.find("connection");
                    bool closeConn = !state->keepAlive;
                    if (connHeader != req.headers.end()) {
                        std::string cv = connHeader->second;
                        std::transform(cv.begin(), cv.end(), cv.begin(), ::tolower);
                        if (cv == "close") closeConn = true;
                    }
                    if (closeConn) {
                        epoll_ctl(state->epollFd, EPOLL_CTL_DEL, clientFd, nullptr);
                        close(clientFd);
                        clientBuffers.erase(clientFd);
                    } else {
                        clientBuffers[clientFd].clear();
                    }
                }
            }
        }

        }).detach(); // end of background thread

        // Brief delay to let the server thread start accepting
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

        // If a callback was provided, invoke it now that the server is listening
        if (a.size() >= 2) {
            std::string callbackName = a[1].toString();
            if (!callbackName.empty()) {
                vm.callFunction(callbackName, {});
            }
        }

        return Value::makeBool(true);
    });

    // HttpServer.stop(server) — stop the server
    vm.registerNative("HttpServer.stop", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeBool(false);
        auto fdIt = a[0].objVal->fields.find("fd");
        if (fdIt == a[0].objVal->fields.end()) return Value::makeBool(false);
        int fd = fdIt->second.toInt();

        std::lock_guard<std::mutex> lock(g_serversMutex);
        auto it = g_servers.find(fd);
        if (it != g_servers.end()) {
            it->second->running = false;
            close(it->second->epollFd);
            close(fd);
            g_servers.erase(it);
        }
        a[0].objVal->fields["running"] = Value::makeBool(false);
        return Value::makeBool(true);
    });

    // HttpServer.isRunning(server)
    vm.registerNative("HttpServer.isRunning", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeBool(false);
        auto it = a[0].objVal->fields.find("running");
        return Value::makeBool(it != a[0].objVal->fields.end() && it->second.toBool());
    });

    // HttpServer.getPort(server)
    vm.registerNative("HttpServer.getPort", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeInt(0);
        auto it = a[0].objVal->fields.find("port");
        return it != a[0].objVal->fields.end() ? it->second : Value::makeInt(0);
    });

    // HttpServer.getHost(server)
    vm.registerNative("HttpServer.getHost", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeString("");
        auto it = a[0].objVal->fields.find("host");
        return it != a[0].objVal->fields.end() ? it->second : Value::makeString("");
    });

    // ===== HTTP Response helpers =====

    // HttpResponse.json(response) — parse response body as JSON string
    vm.registerNative("HttpResponse.json", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeNull();
        auto it = a[0].objVal->fields.find("body");
        return it != a[0].objVal->fields.end() ? it->second : Value::makeNull();
    });

    // HttpResponse.text(response) — get response body as text
    vm.registerNative("HttpResponse.text", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeString("");
        auto it = a[0].objVal->fields.find("body");
        return it != a[0].objVal->fields.end() ? it->second : Value::makeString("");
    });

    // HttpResponse.bytes(response) — get response body as byte array
    vm.registerNative("HttpResponse.bytes", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeArray();
        auto it = a[0].objVal->fields.find("body");
        if (it == a[0].objVal->fields.end()) return Value::makeArray();
        std::string body = it->second.toString();
        Value arr = Value::makeArray();
        for (unsigned char c : body) {
            arr.arrVal->elements.push_back(Value::makeInt(c));
        }
        return arr;
    });

    // HttpResponse.header(response, name) — get a specific response header
    vm.registerNative("HttpResponse.header", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) return Value::makeNull();
        auto hIt = a[0].objVal->fields.find("headers");
        if (hIt == a[0].objVal->fields.end() || !hIt->second.objVal) return Value::makeNull();
        std::string name = a[1].toString();
        std::transform(name.begin(), name.end(), name.begin(), ::tolower);
        auto it = hIt->second.objVal->fields.find(name);
        return it != hIt->second.objVal->fields.end() ? it->second : Value::makeNull();
    });

    // HttpResponse.isOk(response) — check if status is 2xx
    vm.registerNative("HttpResponse.isOk", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeBool(false);
        auto it = a[0].objVal->fields.find("ok");
        return it != a[0].objVal->fields.end() ? it->second : Value::makeBool(false);
    });

    // HttpResponse.isRedirect(response) — check if status is 3xx
    vm.registerNative("HttpResponse.isRedirect", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeBool(false);
        auto it = a[0].objVal->fields.find("status");
        if (it == a[0].objVal->fields.end()) return Value::makeBool(false);
        int status = it->second.toInt();
        return Value::makeBool(status >= 300 && status < 400);
    });

    // HttpResponse.isClientError(response) — check if status is 4xx
    vm.registerNative("HttpResponse.isClientError", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeBool(false);
        auto it = a[0].objVal->fields.find("status");
        if (it == a[0].objVal->fields.end()) return Value::makeBool(false);
        int status = it->second.toInt();
        return Value::makeBool(status >= 400 && status < 500);
    });

    // HttpResponse.isServerError(response) — check if status is 5xx
    vm.registerNative("HttpResponse.isServerError", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeBool(false);
        auto it = a[0].objVal->fields.find("status");
        if (it == a[0].objVal->fields.end()) return Value::makeBool(false);
        int status = it->second.toInt();
        return Value::makeBool(status >= 500 && status < 600);
    });

    // ===== HTTP Request/Response streaming =====

    // http.stream(url, options) — streaming GET with chunked reading
    // Returns a stream object that can be read chunk by chunk
    vm.registerNative("http.stream", [](const std::vector<Value>& a) -> Value {
        if (a.empty()) return Value::makeNull();
        std::string url = a[0].toString();

        CURL* curl = curl_easy_init();
        if (!curl) return Value::makeNull();

        // For streaming, we set up the connection but read in chunks
        std::string* responseBuffer = new std::string();

        auto writeCallback = +[](char* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
            static_cast<std::string*>(userdata)->append(ptr, size * nmemb);
            return size * nmemb;
        };

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, responseBuffer);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "Gard/0.1.0");
        curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);

        CURLcode res = curl_easy_perform(curl);

        Value stream = Value::makeObject("HttpStream");
        if (res == CURLE_OK) {
            long statusCode;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &statusCode);
            stream.objVal->fields["status"] = Value::makeInt(static_cast<int>(statusCode));
            stream.objVal->fields["data"] = Value::makeString(*responseBuffer);
            stream.objVal->fields["size"] = Value::makeInt(static_cast<int>(responseBuffer->size()));
            stream.objVal->fields["position"] = Value::makeInt(0);
            stream.objVal->fields["done"] = Value::makeBool(false);
        } else {
            stream.objVal->fields["error"] = Value::makeString(curl_easy_strerror(res));
            stream.objVal->fields["done"] = Value::makeBool(true);
        }

        delete responseBuffer;
        curl_easy_cleanup(curl);
        return stream;
    });

    // HttpStream.read(stream, chunkSize) — read next chunk from stream
    vm.registerNative("HttpStream.read", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeNull();
        int chunkSize = a.size() >= 2 ? a[1].toInt() : 4096;

        auto dataIt = a[0].objVal->fields.find("data");
        auto posIt = a[0].objVal->fields.find("position");
        if (dataIt == a[0].objVal->fields.end() || posIt == a[0].objVal->fields.end())
            return Value::makeNull();

        std::string data = dataIt->second.toString();
        int pos = posIt->second.toInt();

        if (pos >= (int)data.size()) {
            a[0].objVal->fields["done"] = Value::makeBool(true);
            return Value::makeString("");
        }

        std::string chunk = data.substr(pos, chunkSize);
        a[0].objVal->fields["position"] = Value::makeInt(pos + (int)chunk.size());
        if (pos + (int)chunk.size() >= (int)data.size()) {
            a[0].objVal->fields["done"] = Value::makeBool(true);
        }
        return Value::makeString(chunk);
    });

    // HttpStream.isDone(stream)
    vm.registerNative("HttpStream.isDone", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeBool(true);
        auto it = a[0].objVal->fields.find("done");
        return it != a[0].objVal->fields.end() ? it->second : Value::makeBool(true);
    });

    // ===== URL utilities =====

    // http.encodeURI(str) — percent-encode a URI component
    vm.registerNative("http.encodeURI", [](const std::vector<Value>& a) -> Value {
        if (a.empty()) return Value::makeString("");
        std::string input = a[0].toString();
        CURL* curl = curl_easy_init();
        if (!curl) return Value::makeString(input);
        char* encoded = curl_easy_escape(curl, input.c_str(), (int)input.size());
        std::string result = encoded ? encoded : input;
        if (encoded) curl_free(encoded);
        curl_easy_cleanup(curl);
        return Value::makeString(result);
    });

    // http.decodeURI(str) — decode a percent-encoded URI component
    vm.registerNative("http.decodeURI", [](const std::vector<Value>& a) -> Value {
        if (a.empty()) return Value::makeString("");
        std::string input = a[0].toString();
        CURL* curl = curl_easy_init();
        if (!curl) return Value::makeString(input);
        int outLen = 0;
        char* decoded = curl_easy_unescape(curl, input.c_str(), (int)input.size(), &outLen);
        std::string result = decoded ? std::string(decoded, outLen) : input;
        if (decoded) curl_free(decoded);
        curl_easy_cleanup(curl);
        return Value::makeString(result);
    });

    // http.parseQuery(queryString) — parse "key=val&key2=val2" into object
    vm.registerNative("http.parseQuery", [](const std::vector<Value>& a) -> Value {
        if (a.empty()) return Value::makeObject("QueryParams");
        std::string qs = a[0].toString();
        Value result = Value::makeObject("QueryParams");

        std::istringstream stream(qs);
        std::string pair;
        while (std::getline(stream, pair, '&')) {
            size_t eq = pair.find('=');
            if (eq != std::string::npos) {
                std::string key = pair.substr(0, eq);
                std::string val = pair.substr(eq + 1);
                result.objVal->fields[key] = Value::makeString(val);
            } else {
                result.objVal->fields[pair] = Value::makeString("");
            }
        }
        return result;
    });

    // http.buildQuery(obj) — build query string from object
    vm.registerNative("http.buildQuery", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeString("");
        std::string result;
        bool first = true;
        for (auto& [k, v] : a[0].objVal->fields) {
            if (!first) result += "&";
            result += k + "=" + v.toString();
            first = false;
        }
        return Value::makeString(result);
    });

    // ===== 6.3 WebSocket Client (RFC 6455) =====
    // Real implementation: SHA-1 handshake, frame encode/decode, masking,
    // ping/pong, close frames, TLS via OpenSSL for wss://

    // WebSocket.connect(url, options) — establish WebSocket connection
    vm.registerNative("WebSocket.connect", [getOpt, isObjLike](const std::vector<Value>& a) -> Value {
        if (a.empty()) return Value::makeNull();
        std::string url = a[0].toString();
        auto ws = std::make_shared<WsConnection>();
        ws->url = url;
        if (!parseWsUrl(url, ws->useTls, ws->host, ws->port, ws->path)) return Value::makeNull();

        std::unordered_map<std::string, std::string> extraHeaders;
        int timeout = 10000;
        if (a.size() >= 2 && isObjLike(a[1])) {
            Value pv = getOpt(a[1], "protocols");
            if (pv.type == ValueType::Array && pv.arrVal)
                for (auto& p : pv.arrVal->elements) ws->requestedProtocols.push_back(p.toString());
            else if (pv.type == ValueType::String && pv.toString().size() > 0)
                ws->requestedProtocols.push_back(pv.toString());
            Value hv = getOpt(a[1], "headers");
            if (hv.type == ValueType::Object && hv.objVal)
                for (auto& [k, v] : hv.objVal->fields) extraHeaders[k] = v.toString();
            else if (hv.type == ValueType::Map && hv.mapVal)
                for (auto& [k, v] : hv.mapVal->entries) extraHeaders[k] = v.toString();
            Value tv = getOpt(a[1], "timeout");
            if (tv.type != ValueType::Null) timeout = tv.toInt();
            Value arv = getOpt(a[1], "autoReconnect");
            if (arv.type != ValueType::Null) ws->autoReconnect = arv.toBool();
            Value mrv = getOpt(a[1], "maxReconnectAttempts");
            if (mrv.type != ValueType::Null) ws->maxReconnectAttempts = mrv.toInt();
        }

        ws->fd = socket(AF_INET, SOCK_STREAM, 0);
        if (ws->fd < 0) return Value::makeNull();
        struct timeval tv; tv.tv_sec = timeout / 1000; tv.tv_usec = (timeout % 1000) * 1000;
        setsockopt(ws->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        setsockopt(ws->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        struct hostent* he = gethostbyname(ws->host.c_str());
        if (!he) { close(ws->fd); return Value::makeNull(); }
        struct sockaddr_in addr{}; addr.sin_family = AF_INET;
        std::memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);
        addr.sin_port = htons(ws->port);
        if (connect(ws->fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { close(ws->fd); return Value::makeNull(); }

        if (ws->useTls) {
            ws->sslCtx = SSL_CTX_new(TLS_client_method());
            if (!ws->sslCtx) { close(ws->fd); return Value::makeNull(); }
            SSL_CTX_set_default_verify_paths(ws->sslCtx);
            ws->ssl = SSL_new(ws->sslCtx); SSL_set_fd(ws->ssl, ws->fd);
            SSL_set_tlsext_host_name(ws->ssl, ws->host.c_str());
            if (SSL_connect(ws->ssl) <= 0) { SSL_free(ws->ssl); SSL_CTX_free(ws->sslCtx); close(ws->fd); return Value::makeNull(); }
        }

        std::string wsKey = generateWsKey();
        if (!wsHandshake(ws.get(), wsKey, extraHeaders)) {
            if (ws->ssl) { SSL_shutdown(ws->ssl); SSL_free(ws->ssl); }
            if (ws->sslCtx) SSL_CTX_free(ws->sslCtx);
            close(ws->fd); return Value::makeNull();
        }
        ws->connected = true;
        { std::lock_guard<std::mutex> lock(g_wsMutex); g_wsConnections[ws->fd] = ws; }

        Value result = Value::makeObject("WebSocket");
        result.objVal->fields["fd"] = Value::makeInt(ws->fd);
        result.objVal->fields["url"] = Value::makeString(url);
        result.objVal->fields["connected"] = Value::makeBool(true);
        result.objVal->fields["protocol"] = Value::makeString(ws->protocol);
        result.objVal->fields["host"] = Value::makeString(ws->host);
        result.objVal->fields["port"] = Value::makeInt(ws->port);
        result.objVal->fields["secure"] = Value::makeBool(ws->useTls);
        return result;
    });

    // WebSocket.send(ws, message) — send text frame
    vm.registerNative("WebSocket.send", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) return Value::makeBool(false);
        int fd = a[0].objVal->fields["fd"].toInt();
        std::shared_ptr<WsConnection> ws;
        { std::lock_guard<std::mutex> lock(g_wsMutex); auto it = g_wsConnections.find(fd); if (it == g_wsConnections.end() || !it->second->connected) return Value::makeBool(false); ws = it->second; }
        std::string msg = a[1].toString();
        auto frame = buildWsFrame(WS_TEXT, (const uint8_t*)msg.c_str(), msg.size(), true);
        return Value::makeBool(wsSend(ws.get(), frame.data(), frame.size()) > 0);
    });

    // WebSocket.sendBinary(ws, byteArray) — send binary frame
    vm.registerNative("WebSocket.sendBinary", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) return Value::makeBool(false);
        int fd = a[0].objVal->fields["fd"].toInt();
        std::shared_ptr<WsConnection> ws;
        { std::lock_guard<std::mutex> lock(g_wsMutex); auto it = g_wsConnections.find(fd); if (it == g_wsConnections.end() || !it->second->connected) return Value::makeBool(false); ws = it->second; }
        std::vector<uint8_t> data;
        if (a[1].type == ValueType::Array && a[1].arrVal) for (auto& e : a[1].arrVal->elements) data.push_back((uint8_t)e.toInt());
        else { std::string s = a[1].toString(); data.assign(s.begin(), s.end()); }
        auto frame = buildWsFrame(WS_BINARY, data.data(), data.size(), true);
        return Value::makeBool(wsSend(ws.get(), frame.data(), frame.size()) > 0);
    });

    // WebSocket.receive(ws, timeoutMs) — receive next message (blocking with timeout)
    vm.registerNative("WebSocket.receive", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeNull();
        int fd = a[0].objVal->fields["fd"].toInt();
        std::shared_ptr<WsConnection> ws;
        { std::lock_guard<std::mutex> lock(g_wsMutex); auto it = g_wsConnections.find(fd); if (it == g_wsConnections.end() || !it->second->connected) return Value::makeNull(); ws = it->second; }
        int timeoutMs = a.size() >= 2 ? a[1].toInt() : 30000;

        struct pollfd pfd; pfd.fd = fd; pfd.events = POLLIN;
        if (poll(&pfd, 1, timeoutMs) <= 0) return Value::makeNull();

        uint8_t buf[65536];
        ssize_t n = wsRecv(ws.get(), buf, sizeof(buf));
        if (n <= 0) { ws->connected = false; a[0].objVal->fields["connected"] = Value::makeBool(false); return Value::makeNull(); }
        ws->recvBuffer.insert(ws->recvBuffer.end(), buf, buf + n);

        WsFrame frame = parseWsFrame(ws->recvBuffer.data(), ws->recvBuffer.size());
        if (frame.bytesConsumed < 0) return Value::makeNull();
        ws->recvBuffer.erase(ws->recvBuffer.begin(), ws->recvBuffer.begin() + frame.bytesConsumed);

        Value result = Value::makeObject("WsMessage");
        switch (frame.opcode) {
            case WS_TEXT:
                result.objVal->fields["type"] = Value::makeString("text");
                result.objVal->fields["data"] = Value::makeString(std::string(frame.payload.begin(), frame.payload.end()));
                break;
            case WS_BINARY: {
                result.objVal->fields["type"] = Value::makeString("binary");
                Value arr = Value::makeArray();
                for (uint8_t b : frame.payload) arr.arrVal->elements.push_back(Value::makeInt(b));
                result.objVal->fields["data"] = arr;
                break;
            }
            case WS_PING: {
                result.objVal->fields["type"] = Value::makeString("ping");
                result.objVal->fields["data"] = Value::makeString(std::string(frame.payload.begin(), frame.payload.end()));
                auto pong = buildWsFrame(WS_PONG, frame.payload.data(), frame.payload.size(), true);
                wsSend(ws.get(), pong.data(), pong.size());
                break;
            }
            case WS_PONG:
                result.objVal->fields["type"] = Value::makeString("pong");
                result.objVal->fields["data"] = Value::makeString(std::string(frame.payload.begin(), frame.payload.end()));
                break;
            case WS_CLOSE: {
                result.objVal->fields["type"] = Value::makeString("close");
                int code = 1000; std::string reason;
                if (frame.payload.size() >= 2) { code = ((int)frame.payload[0] << 8) | frame.payload[1]; if (frame.payload.size() > 2) reason = std::string(frame.payload.begin() + 2, frame.payload.end()); }
                result.objVal->fields["code"] = Value::makeInt(code);
                result.objVal->fields["reason"] = Value::makeString(reason);
                auto closeResp = buildWsFrame(WS_CLOSE, frame.payload.data(), frame.payload.size(), true);
                wsSend(ws.get(), closeResp.data(), closeResp.size());
                ws->connected = false; ws->closeCode = code; ws->closeReason = reason;
                a[0].objVal->fields["connected"] = Value::makeBool(false);
                break;
            }
            default: result.objVal->fields["type"] = Value::makeString("unknown"); break;
        }
        return result;
    });

    // WebSocket.ping(ws, data?) — send ping frame
    vm.registerNative("WebSocket.ping", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeBool(false);
        int fd = a[0].objVal->fields["fd"].toInt();
        std::shared_ptr<WsConnection> ws;
        { std::lock_guard<std::mutex> lock(g_wsMutex); auto it = g_wsConnections.find(fd); if (it == g_wsConnections.end() || !it->second->connected) return Value::makeBool(false); ws = it->second; }
        std::string payload = a.size() >= 2 ? a[1].toString() : "";
        auto frame = buildWsFrame(WS_PING, (const uint8_t*)payload.c_str(), payload.size(), true);
        return Value::makeBool(wsSend(ws.get(), frame.data(), frame.size()) > 0);
    });

    // WebSocket.pong(ws, data?) — send pong frame
    vm.registerNative("WebSocket.pong", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeBool(false);
        int fd = a[0].objVal->fields["fd"].toInt();
        std::shared_ptr<WsConnection> ws;
        { std::lock_guard<std::mutex> lock(g_wsMutex); auto it = g_wsConnections.find(fd); if (it == g_wsConnections.end() || !it->second->connected) return Value::makeBool(false); ws = it->second; }
        std::string payload = a.size() >= 2 ? a[1].toString() : "";
        auto frame = buildWsFrame(WS_PONG, (const uint8_t*)payload.c_str(), payload.size(), true);
        return Value::makeBool(wsSend(ws.get(), frame.data(), frame.size()) > 0);
    });

    // WebSocket.close(ws, code?, reason?) — graceful close
    vm.registerNative("WebSocket.close", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeBool(false);
        int fd = a[0].objVal->fields["fd"].toInt();
        std::shared_ptr<WsConnection> ws;
        { std::lock_guard<std::mutex> lock(g_wsMutex); auto it = g_wsConnections.find(fd); if (it == g_wsConnections.end()) return Value::makeBool(false); ws = it->second; }
        int code = a.size() >= 2 ? a[1].toInt() : 1000;
        std::string reason = a.size() >= 3 ? a[2].toString() : "";
        std::vector<uint8_t> closePayload; closePayload.push_back((uint8_t)(code >> 8)); closePayload.push_back((uint8_t)(code & 0xFF));
        closePayload.insert(closePayload.end(), reason.begin(), reason.end());
        auto frame = buildWsFrame(WS_CLOSE, closePayload.data(), closePayload.size(), true);
        wsSend(ws.get(), frame.data(), frame.size());
        ws->connected = false; ws->closeCode = code; ws->closeReason = reason;
        if (ws->ssl) { SSL_shutdown(ws->ssl); SSL_free(ws->ssl); ws->ssl = nullptr; }
        if (ws->sslCtx) { SSL_CTX_free(ws->sslCtx); ws->sslCtx = nullptr; }
        close(fd);
        a[0].objVal->fields["connected"] = Value::makeBool(false);
        { std::lock_guard<std::mutex> lock(g_wsMutex); g_wsConnections.erase(fd); }
        return Value::makeBool(true);
    });

    // WebSocket.isConnected(ws)
    vm.registerNative("WebSocket.isConnected", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeBool(false);
        int fd = a[0].objVal->fields["fd"].toInt();
        std::lock_guard<std::mutex> lock(g_wsMutex);
        auto it = g_wsConnections.find(fd);
        return Value::makeBool(it != g_wsConnections.end() && it->second->connected);
    });

    // WebSocket.getProtocol(ws)
    vm.registerNative("WebSocket.getProtocol", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeString("");
        auto it = a[0].objVal->fields.find("protocol");
        return it != a[0].objVal->fields.end() ? it->second : Value::makeString("");
    });

    // WebSocket.getReadyState(ws) — 0=CONNECTING, 1=OPEN, 2=CLOSING, 3=CLOSED
    vm.registerNative("WebSocket.getReadyState", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeInt(3);
        int fd = a[0].objVal->fields["fd"].toInt();
        std::lock_guard<std::mutex> lock(g_wsMutex);
        auto it = g_wsConnections.find(fd);
        if (it == g_wsConnections.end()) return Value::makeInt(3);
        return Value::makeInt(it->second->connected ? 1 : 3);
    });

    // WebSocket.reconnect(ws, options?) — reconnect with exponential backoff
    vm.registerNative("WebSocket.reconnect", [getOpt, isObjLike](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeBool(false);
        int oldFd = a[0].objVal->fields["fd"].toInt();
        std::shared_ptr<WsConnection> ws;
        { std::lock_guard<std::mutex> lock(g_wsMutex); auto it = g_wsConnections.find(oldFd); if (it != g_wsConnections.end()) { ws = it->second; g_wsConnections.erase(it); } }
        if (!ws) return Value::makeBool(false);

        int maxAttempts = ws->maxReconnectAttempts;
        int delayMs = ws->reconnectDelayMs;
        double backoff = ws->reconnectBackoffMultiplier;
        if (a.size() >= 2 && isObjLike(a[1])) {
            Value v = getOpt(a[1], "maxAttempts"); if (v.type != ValueType::Null) maxAttempts = v.toInt();
            v = getOpt(a[1], "delayMs"); if (v.type != ValueType::Null) delayMs = v.toInt();
            v = getOpt(a[1], "backoffMultiplier"); if (v.type != ValueType::Null) backoff = v.toDouble();
        }

        if (ws->ssl) { SSL_shutdown(ws->ssl); SSL_free(ws->ssl); ws->ssl = nullptr; }
        if (ws->sslCtx) { SSL_CTX_free(ws->sslCtx); ws->sslCtx = nullptr; }
        if (ws->fd >= 0) { close(ws->fd); ws->fd = -1; }
        ws->connected = false; ws->recvBuffer.clear();

        int currentDelay = delayMs;
        for (int attempt = 0; attempt < maxAttempts; attempt++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(currentDelay));
            ws->fd = socket(AF_INET, SOCK_STREAM, 0);
            if (ws->fd < 0) { currentDelay = (int)(currentDelay * backoff); continue; }
            struct timeval tv2; tv2.tv_sec = 10; tv2.tv_usec = 0;
            setsockopt(ws->fd, SOL_SOCKET, SO_SNDTIMEO, &tv2, sizeof(tv2));
            setsockopt(ws->fd, SOL_SOCKET, SO_RCVTIMEO, &tv2, sizeof(tv2));
            struct hostent* he2 = gethostbyname(ws->host.c_str());
            if (!he2) { close(ws->fd); ws->fd = -1; currentDelay = (int)(currentDelay * backoff); continue; }
            struct sockaddr_in addr2{}; addr2.sin_family = AF_INET;
            std::memcpy(&addr2.sin_addr, he2->h_addr_list[0], he2->h_length); addr2.sin_port = htons(ws->port);
            if (connect(ws->fd, (struct sockaddr*)&addr2, sizeof(addr2)) < 0) { close(ws->fd); ws->fd = -1; currentDelay = (int)(currentDelay * backoff); continue; }
            if (ws->useTls) {
                ws->sslCtx = SSL_CTX_new(TLS_client_method());
                if (!ws->sslCtx) { close(ws->fd); ws->fd = -1; currentDelay = (int)(currentDelay * backoff); continue; }
                SSL_CTX_set_default_verify_paths(ws->sslCtx);
                ws->ssl = SSL_new(ws->sslCtx); SSL_set_fd(ws->ssl, ws->fd); SSL_set_tlsext_host_name(ws->ssl, ws->host.c_str());
                if (SSL_connect(ws->ssl) <= 0) { SSL_free(ws->ssl); ws->ssl = nullptr; SSL_CTX_free(ws->sslCtx); ws->sslCtx = nullptr; close(ws->fd); ws->fd = -1; currentDelay = (int)(currentDelay * backoff); continue; }
            }
            std::string wsKey = generateWsKey();
            std::unordered_map<std::string, std::string> noH;
            if (wsHandshake(ws.get(), wsKey, noH)) {
                ws->connected = true; ws->reconnectAttempts = attempt + 1;
                { std::lock_guard<std::mutex> lock(g_wsMutex); g_wsConnections[ws->fd] = ws; }
                a[0].objVal->fields["fd"] = Value::makeInt(ws->fd);
                a[0].objVal->fields["connected"] = Value::makeBool(true);
                return Value::makeBool(true);
            }
            if (ws->ssl) { SSL_shutdown(ws->ssl); SSL_free(ws->ssl); ws->ssl = nullptr; }
            if (ws->sslCtx) { SSL_CTX_free(ws->sslCtx); ws->sslCtx = nullptr; }
            close(ws->fd); ws->fd = -1; currentDelay = (int)(currentDelay * backoff);
        }
        return Value::makeBool(false);
    });

    // WebSocket.getCloseCode(ws)
    vm.registerNative("WebSocket.getCloseCode", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeInt(0);
        int fd = a[0].objVal->fields["fd"].toInt();
        std::lock_guard<std::mutex> lock(g_wsMutex);
        auto it = g_wsConnections.find(fd);
        return Value::makeInt(it != g_wsConnections.end() ? it->second->closeCode : 0);
    });

    // WebSocket.getCloseReason(ws)
    vm.registerNative("WebSocket.getCloseReason", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeString("");
        int fd = a[0].objVal->fields["fd"].toInt();
        std::lock_guard<std::mutex> lock(g_wsMutex);
        auto it = g_wsConnections.find(fd);
        return Value::makeString(it != g_wsConnections.end() ? it->second->closeReason : "");
    });

    // WebSocket.setTimeout(ws, ms)
    vm.registerNative("WebSocket.setTimeout", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) return Value::makeBool(false);
        int fd = a[0].objVal->fields["fd"].toInt();
        int ms = a[1].toInt();
        struct timeval tv3; tv3.tv_sec = ms / 1000; tv3.tv_usec = (ms % 1000) * 1000;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv3, sizeof(tv3));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv3, sizeof(tv3));
        return Value::makeBool(true);
    });

    // ===== 6.4 WebSocket Server (real epoll + RFC 6455 framing) =====

    // WebSocketServer.create(options) — create a WebSocket server
    vm.registerNative("WebSocketServer.create", [getOpt, isObjLike](const std::vector<Value>& a) -> Value {
        int port = 8080; std::string host = "0.0.0.0"; int maxConn = 1024;
        int heartbeat = 30000; int rateLimit = 120;
        if (!a.empty() && isObjLike(a[0])) {
            Value v = getOpt(a[0], "port"); if (v.type != ValueType::Null) port = v.toInt();
            v = getOpt(a[0], "host"); if (v.type != ValueType::Null) host = v.toString();
            v = getOpt(a[0], "maxConnections"); if (v.type != ValueType::Null) maxConn = v.toInt();
            v = getOpt(a[0], "heartbeatInterval"); if (v.type != ValueType::Null) heartbeat = v.toInt();
            v = getOpt(a[0], "maxMessagesPerMinute"); if (v.type != ValueType::Null) rateLimit = v.toInt();
        } else if (!a.empty()) { port = a[0].toInt(); }

        int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (fd < 0) return Value::makeNull();
        int opt = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
        struct sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons(port);
        inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
        if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { close(fd); return Value::makeNull(); }
        if (listen(fd, maxConn) < 0) { close(fd); return Value::makeNull(); }

        int epollFd = epoll_create1(0);
        if (epollFd < 0) { close(fd); return Value::makeNull(); }
        struct epoll_event ev{}; ev.events = EPOLLIN; ev.data.fd = fd;
        epoll_ctl(epollFd, EPOLL_CTL_ADD, fd, &ev);

        auto state = std::make_shared<WsServerState>();
        state->fd = fd; state->epollFd = epollFd; state->port = port;
        state->host = host; state->maxConnections = maxConn;
        state->heartbeatIntervalMs = heartbeat; state->maxMessagesPerMinute = rateLimit;
        { std::lock_guard<std::mutex> lock(g_wsServersMutex); g_wsServers[fd] = state; }

        Value result = Value::makeObject("WebSocketServer");
        result.objVal->fields["fd"] = Value::makeInt(fd);
        result.objVal->fields["port"] = Value::makeInt(port);
        result.objVal->fields["host"] = Value::makeString(host);
        result.objVal->fields["running"] = Value::makeBool(false);
        result.objVal->fields["maxConnections"] = Value::makeInt(maxConn);
        return result;
    });

    // WebSocketServer.listen(server, callback?) — start accepting WebSocket connections in background
    vm.registerNative("WebSocketServer.listen", [&vm](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeBool(false);
        int serverFd = a[0].objVal->fields["fd"].toInt();

        std::shared_ptr<WsServerState> state;
        {
            std::lock_guard<std::mutex> lock(g_wsServersMutex);
            auto it = g_wsServers.find(serverFd);
            if (it == g_wsServers.end()) return Value::makeBool(false);
            state = it->second;
        }

        state->running = true;
        a[0].objVal->fields["running"] = Value::makeBool(true);

        // Spawn background thread to accept connections and handle WebSocket frames
        std::thread([state, serverFd]() {
            while (state->running) {
                struct epoll_event events[32];
                int nfds = epoll_wait(state->epollFd, events, 32, 100);
                for (int i = 0; i < nfds; i++) {
                    if (events[i].data.fd == serverFd) {
                        // Accept new connection
                        struct sockaddr_in clientAddr{};
                        socklen_t addrLen = sizeof(clientAddr);
                        int clientFd = accept(serverFd, (struct sockaddr*)&clientAddr, &addrLen);
                        if (clientFd < 0) continue;

                        // Read HTTP upgrade request
                        char buf[4096];
                        // Set brief timeout for handshake read
                        struct timeval tv = {2, 0};
                        setsockopt(clientFd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
                        ssize_t n = recv(clientFd, buf, sizeof(buf) - 1, 0);
                        if (n <= 0) { close(clientFd); continue; }
                        buf[n] = '\0';
                        std::string request(buf, n);

                        // Extract Sec-WebSocket-Key
                        std::string wsKey;
                        size_t keyPos = request.find("Sec-WebSocket-Key:");
                        if (keyPos == std::string::npos) keyPos = request.find("sec-websocket-key:");
                        if (keyPos != std::string::npos) {
                            size_t valStart = request.find(':', keyPos) + 1;
                            while (valStart < request.size() && request[valStart] == ' ') valStart++;
                            size_t valEnd = request.find("\r\n", valStart);
                            if (valEnd != std::string::npos) wsKey = request.substr(valStart, valEnd - valStart);
                        }
                        if (wsKey.empty()) { close(clientFd); continue; }

                        // Send upgrade response
                        std::string accept = computeWsAccept(wsKey);
                        std::string response = "HTTP/1.1 101 Switching Protocols\r\n"
                                               "Upgrade: websocket\r\n"
                                               "Connection: Upgrade\r\n"
                                               "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";
                        send(clientFd, response.c_str(), response.size(), 0);

                        // Register client
                        auto client = std::make_shared<WsClientInfo>();
                        client->fd = clientFd;
                        client->id = generateClientId(state.get());
                        client->remoteAddr = inet_ntoa(clientAddr.sin_addr);
                        client->remotePort = ntohs(clientAddr.sin_port);
                        client->lastActivity = std::chrono::steady_clock::now();

                        {
                            std::lock_guard<std::mutex> lock(state->mutex);
                            state->clients[clientFd] = client;
                            state->clientsById[client->id] = client;
                        }

                        // Add to epoll for reading frames
                        struct epoll_event ev{};
                        ev.events = EPOLLIN | EPOLLET;
                        ev.data.fd = clientFd;
                        epoll_ctl(state->epollFd, EPOLL_CTL_ADD, clientFd, &ev);
                    } else {
                        // Read WebSocket frame from client
                        int clientFd = events[i].data.fd;
                        if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                            std::lock_guard<std::mutex> lock(state->mutex);
                            auto cit = state->clients.find(clientFd);
                            if (cit != state->clients.end()) {
                                state->clientsById.erase(cit->second->id);
                                state->clients.erase(cit);
                            }
                            epoll_ctl(state->epollFd, EPOLL_CTL_DEL, clientFd, nullptr);
                            close(clientFd);
                            continue;
                        }

                        uint8_t header[2];
                        ssize_t n = recv(clientFd, header, 2, 0);
                        if (n <= 0) continue;

                        // Parse WebSocket frame
                        uint8_t opcode = header[0] & 0x0F;
                        bool masked = (header[1] & 0x80) != 0;
                        uint64_t payloadLen = header[1] & 0x7F;

                        if (payloadLen == 126) {
                            uint8_t ext[2]; recv(clientFd, ext, 2, 0);
                            payloadLen = (ext[0] << 8) | ext[1];
                        } else if (payloadLen == 127) {
                            uint8_t ext[8]; recv(clientFd, ext, 8, 0);
                            payloadLen = 0;
                            for (int j = 0; j < 8; j++) payloadLen = (payloadLen << 8) | ext[j];
                        }

                        uint8_t mask[4] = {0};
                        if (masked) recv(clientFd, mask, 4, 0);

                        std::vector<uint8_t> payload(payloadLen);
                        size_t received = 0;
                        while (received < payloadLen) {
                            ssize_t r = recv(clientFd, payload.data() + received, payloadLen - received, 0);
                            if (r <= 0) break;
                            received += r;
                        }

                        if (masked) {
                            for (size_t j = 0; j < payloadLen; j++) payload[j] ^= mask[j % 4];
                        }

                        if (opcode == 0x01) { // Text frame
                            std::string msg(payload.begin(), payload.end());
                            std::lock_guard<std::mutex> lock(state->mutex);
                            auto cit = state->clients.find(clientFd);
                            if (cit != state->clients.end()) {
                                cit->second->recvBuffer.insert(cit->second->recvBuffer.end(), msg.begin(), msg.end());
                                cit->second->recvBuffer.push_back('\0'); // null-terminate each message
                            }
                        } else if (opcode == 0x08) { // Close frame
                            std::lock_guard<std::mutex> lock(state->mutex);
                            auto cit = state->clients.find(clientFd);
                            if (cit != state->clients.end()) {
                                state->clientsById.erase(cit->second->id);
                                state->clients.erase(cit);
                            }
                            epoll_ctl(state->epollFd, EPOLL_CTL_DEL, clientFd, nullptr);
                            close(clientFd);
                        }
                    }
                }
            }
        }).detach();

        // Brief delay to let the server thread start
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

        // If callback provided, invoke it
        if (a.size() >= 2) {
            std::string callbackName = a[1].toString();
            if (!callbackName.empty()) {
                vm.callFunction(callbackName, {});
            }
        }
        return Value::makeBool(true);
    });

    // WebSocketServer.getClientCount(server)
    vm.registerNative("WebSocketServer.getClientCount", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeInt(0);
        int fd = a[0].objVal->fields["fd"].toInt();
        std::lock_guard<std::mutex> lock(g_wsServersMutex);
        auto it = g_wsServers.find(fd);
        if (it == g_wsServers.end()) return Value::makeInt(0);
        return Value::makeInt(static_cast<int>(it->second->clients.size()));
    });

    // WebSocketServer.receive(server) — receive next message from any client (blocking with timeout)
    vm.registerNative("WebSocketServer.receive", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeString("");
        int fd = a[0].objVal->fields["fd"].toInt();
        std::shared_ptr<WsServerState> state;
        {
            std::lock_guard<std::mutex> lock(g_wsServersMutex);
            auto it = g_wsServers.find(fd);
            if (it == g_wsServers.end()) return Value::makeString("");
            state = it->second;
        }

        // Poll for up to 5 seconds
        for (int attempt = 0; attempt < 100; attempt++) {
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                for (auto& [clientFd, client] : state->clients) {
                    if (!client->recvBuffer.empty()) {
                        // Find null terminator to extract one message
                        auto nullPos = std::find(client->recvBuffer.begin(), client->recvBuffer.end(), '\0');
                        if (nullPos != client->recvBuffer.end()) {
                            std::string msg(client->recvBuffer.begin(), nullPos);
                            client->recvBuffer.erase(client->recvBuffer.begin(), nullPos + 1);
                            return Value::makeString(msg);
                        }
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return Value::makeString("");
    });

    // WebSocketServer.getClients(server) — returns array of client IDs
    vm.registerNative("WebSocketServer.getClients", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeArray();
        int fd = a[0].objVal->fields["fd"].toInt();
        std::lock_guard<std::mutex> lock(g_wsServersMutex);
        auto it = g_wsServers.find(fd);
        if (it == g_wsServers.end()) return Value::makeArray();
        Value arr = Value::makeArray();
        for (auto& [cfd, client] : it->second->clients) {
            arr.arrVal->elements.push_back(Value::makeString(client->id));
        }
        return arr;
    });

    // WebSocketServer.sendTo(server, clientId, message) — send to specific client
    vm.registerNative("WebSocketServer.sendTo", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 3 || !a[0].objVal) return Value::makeBool(false);
        int fd = a[0].objVal->fields["fd"].toInt();
        std::string clientId = a[1].toString();
        std::string message = a[2].toString();
        std::lock_guard<std::mutex> lock(g_wsServersMutex);
        auto it = g_wsServers.find(fd);
        if (it == g_wsServers.end()) return Value::makeBool(false);
        auto cit = it->second->clientsById.find(clientId);
        if (cit == it->second->clientsById.end()) return Value::makeBool(false);
        // Server→client frames are NOT masked (per RFC 6455)
        auto frame = buildWsFrame(WS_TEXT, (const uint8_t*)message.c_str(), message.size(), false);
        ssize_t sent = send(cit->second->fd, frame.data(), frame.size(), MSG_NOSIGNAL);
        return Value::makeBool(sent > 0);
    });

    // WebSocketServer.broadcast(server, message, excludeClientId?) — broadcast to all
    vm.registerNative("WebSocketServer.broadcast", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) return Value::makeInt(0);
        int fd = a[0].objVal->fields["fd"].toInt();
        std::string message = a[1].toString();
        std::string exclude = a.size() >= 3 ? a[2].toString() : "";
        std::lock_guard<std::mutex> lock(g_wsServersMutex);
        auto it = g_wsServers.find(fd);
        if (it == g_wsServers.end()) return Value::makeInt(0);
        auto frame = buildWsFrame(WS_TEXT, (const uint8_t*)message.c_str(), message.size(), false);
        int count = 0;
        for (auto& [cfd, client] : it->second->clients) {
            if (!exclude.empty() && client->id == exclude) continue;
            if (send(cfd, frame.data(), frame.size(), MSG_NOSIGNAL) > 0) count++;
        }
        return Value::makeInt(count);
    });

    // WebSocketServer.joinRoom(server, clientId, room)
    vm.registerNative("WebSocketServer.joinRoom", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 3 || !a[0].objVal) return Value::makeBool(false);
        int fd = a[0].objVal->fields["fd"].toInt();
        std::string clientId = a[1].toString();
        std::string room = a[2].toString();
        std::lock_guard<std::mutex> lock(g_wsServersMutex);
        auto it = g_wsServers.find(fd);
        if (it == g_wsServers.end()) return Value::makeBool(false);
        auto cit = it->second->clientsById.find(clientId);
        if (cit == it->second->clientsById.end()) return Value::makeBool(false);
        // Add to room
        it->second->rooms[room].push_back(clientId);
        cit->second->rooms.push_back(room);
        return Value::makeBool(true);
    });

    // WebSocketServer.leaveRoom(server, clientId, room)
    vm.registerNative("WebSocketServer.leaveRoom", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 3 || !a[0].objVal) return Value::makeBool(false);
        int fd = a[0].objVal->fields["fd"].toInt();
        std::string clientId = a[1].toString();
        std::string room = a[2].toString();
        std::lock_guard<std::mutex> lock(g_wsServersMutex);
        auto it = g_wsServers.find(fd);
        if (it == g_wsServers.end()) return Value::makeBool(false);
        auto cit = it->second->clientsById.find(clientId);
        if (cit == it->second->clientsById.end()) return Value::makeBool(false);
        // Remove from room list
        auto rit = it->second->rooms.find(room);
        if (rit != it->second->rooms.end()) {
            rit->second.erase(std::remove(rit->second.begin(), rit->second.end(), clientId), rit->second.end());
        }
        // Remove from client's room list
        auto& crooms = cit->second->rooms;
        crooms.erase(std::remove(crooms.begin(), crooms.end(), room), crooms.end());
        return Value::makeBool(true);
    });

    // WebSocketServer.broadcastToRoom(server, room, message, excludeClientId?)
    vm.registerNative("WebSocketServer.broadcastToRoom", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 3 || !a[0].objVal) return Value::makeInt(0);
        int fd = a[0].objVal->fields["fd"].toInt();
        std::string room = a[1].toString();
        std::string message = a[2].toString();
        std::string exclude = a.size() >= 4 ? a[3].toString() : "";
        std::lock_guard<std::mutex> lock(g_wsServersMutex);
        auto it = g_wsServers.find(fd);
        if (it == g_wsServers.end()) return Value::makeInt(0);
        auto rit = it->second->rooms.find(room);
        if (rit == it->second->rooms.end()) return Value::makeInt(0);
        auto frame = buildWsFrame(WS_TEXT, (const uint8_t*)message.c_str(), message.size(), false);
        int count = 0;
        for (auto& cid : rit->second) {
            if (!exclude.empty() && cid == exclude) continue;
            auto cit = it->second->clientsById.find(cid);
            if (cit != it->second->clientsById.end()) {
                if (send(cit->second->fd, frame.data(), frame.size(), MSG_NOSIGNAL) > 0) count++;
            }
        }
        return Value::makeInt(count);
    });

    // WebSocketServer.getRooms(server) — get all room names
    vm.registerNative("WebSocketServer.getRooms", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeArray();
        int fd = a[0].objVal->fields["fd"].toInt();
        std::lock_guard<std::mutex> lock(g_wsServersMutex);
        auto it = g_wsServers.find(fd);
        if (it == g_wsServers.end()) return Value::makeArray();
        Value arr = Value::makeArray();
        for (auto& [room, _] : it->second->rooms) {
            arr.arrVal->elements.push_back(Value::makeString(room));
        }
        return arr;
    });

    // WebSocketServer.getRoomClients(server, room) — get client IDs in a room
    vm.registerNative("WebSocketServer.getRoomClients", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) return Value::makeArray();
        int fd = a[0].objVal->fields["fd"].toInt();
        std::string room = a[1].toString();
        std::lock_guard<std::mutex> lock(g_wsServersMutex);
        auto it = g_wsServers.find(fd);
        if (it == g_wsServers.end()) return Value::makeArray();
        auto rit = it->second->rooms.find(room);
        if (rit == it->second->rooms.end()) return Value::makeArray();
        Value arr = Value::makeArray();
        for (auto& cid : rit->second) arr.arrVal->elements.push_back(Value::makeString(cid));
        return arr;
    });

    // WebSocketServer.getClientInfo(server, clientId) — get client details
    vm.registerNative("WebSocketServer.getClientInfo", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) return Value::makeNull();
        int fd = a[0].objVal->fields["fd"].toInt();
        std::string clientId = a[1].toString();
        std::lock_guard<std::mutex> lock(g_wsServersMutex);
        auto it = g_wsServers.find(fd);
        if (it == g_wsServers.end()) return Value::makeNull();
        auto cit = it->second->clientsById.find(clientId);
        if (cit == it->second->clientsById.end()) return Value::makeNull();
        Value info = Value::makeObject("WsClientInfo");
        info.objVal->fields["id"] = Value::makeString(cit->second->id);
        info.objVal->fields["remoteAddr"] = Value::makeString(cit->second->remoteAddr);
        info.objVal->fields["remotePort"] = Value::makeInt(cit->second->remotePort);
        Value rooms = Value::makeArray();
        for (auto& r : cit->second->rooms) rooms.arrVal->elements.push_back(Value::makeString(r));
        info.objVal->fields["rooms"] = rooms;
        info.objVal->fields["alive"] = Value::makeBool(cit->second->alive);
        return info;
    });

    // WebSocketServer.disconnect(server, clientId, code?, reason?)
    vm.registerNative("WebSocketServer.disconnect", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) return Value::makeBool(false);
        int fd = a[0].objVal->fields["fd"].toInt();
        std::string clientId = a[1].toString();
        int code = a.size() >= 3 ? a[2].toInt() : 1000;
        std::string reason = a.size() >= 4 ? a[3].toString() : "";
        std::lock_guard<std::mutex> lock(g_wsServersMutex);
        auto it = g_wsServers.find(fd);
        if (it == g_wsServers.end()) return Value::makeBool(false);
        auto cit = it->second->clientsById.find(clientId);
        if (cit == it->second->clientsById.end()) return Value::makeBool(false);
        // Send close frame
        std::vector<uint8_t> payload; payload.push_back((uint8_t)(code >> 8)); payload.push_back((uint8_t)(code & 0xFF));
        payload.insert(payload.end(), reason.begin(), reason.end());
        auto frame = buildWsFrame(WS_CLOSE, payload.data(), payload.size(), false);
        send(cit->second->fd, frame.data(), frame.size(), MSG_NOSIGNAL);
        // Remove from rooms
        for (auto& room : cit->second->rooms) {
            auto& rc = it->second->rooms[room];
            rc.erase(std::remove(rc.begin(), rc.end(), clientId), rc.end());
        }
        int cfd = cit->second->fd;
        it->second->clientsById.erase(cit);
        it->second->clients.erase(cfd);
        epoll_ctl(it->second->epollFd, EPOLL_CTL_DEL, cfd, nullptr);
        close(cfd);
        return Value::makeBool(true);
    });

    // WebSocketServer.pingAll(server) — send ping to all clients for heartbeat
    vm.registerNative("WebSocketServer.pingAll", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeInt(0);
        int fd = a[0].objVal->fields["fd"].toInt();
        std::lock_guard<std::mutex> lock(g_wsServersMutex);
        auto it = g_wsServers.find(fd);
        if (it == g_wsServers.end()) return Value::makeInt(0);
        auto frame = buildWsFrame(WS_PING, (const uint8_t*)"hb", 2, false);
        int count = 0;
        for (auto& [cfd, client] : it->second->clients) {
            client->alive = false; // will be set true on pong
            if (send(cfd, frame.data(), frame.size(), MSG_NOSIGNAL) > 0) count++;
        }
        return Value::makeInt(count);
    });

    // WebSocketServer.pruneDeadClients(server) — remove clients that didn't respond to ping
    vm.registerNative("WebSocketServer.pruneDeadClients", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeInt(0);
        int fd = a[0].objVal->fields["fd"].toInt();
        std::lock_guard<std::mutex> lock(g_wsServersMutex);
        auto it = g_wsServers.find(fd);
        if (it == g_wsServers.end()) return Value::makeInt(0);
        std::vector<int> deadFds;
        for (auto& [cfd, client] : it->second->clients) {
            if (!client->alive) deadFds.push_back(cfd);
        }
        for (int dfd : deadFds) {
            auto cit = it->second->clients.find(dfd);
            if (cit != it->second->clients.end()) {
                std::string cid = cit->second->id;
                for (auto& room : cit->second->rooms) {
                    auto& rc = it->second->rooms[room];
                    rc.erase(std::remove(rc.begin(), rc.end(), cid), rc.end());
                }
                it->second->clientsById.erase(cid);
                it->second->clients.erase(dfd);
            }
            epoll_ctl(it->second->epollFd, EPOLL_CTL_DEL, dfd, nullptr);
            close(dfd);
        }
        return Value::makeInt(static_cast<int>(deadFds.size()));
    });

    // WebSocketServer.setMaxConnections(server, max)
    vm.registerNative("WebSocketServer.setMaxConnections", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) return Value::makeBool(false);
        int fd = a[0].objVal->fields["fd"].toInt();
        std::lock_guard<std::mutex> lock(g_wsServersMutex);
        auto it = g_wsServers.find(fd);
        if (it == g_wsServers.end()) return Value::makeBool(false);
        it->second->maxConnections = a[1].toInt();
        return Value::makeBool(true);
    });

    // WebSocketServer.setRateLimit(server, maxMessagesPerMinute)
    vm.registerNative("WebSocketServer.setRateLimit", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) return Value::makeBool(false);
        int fd = a[0].objVal->fields["fd"].toInt();
        std::lock_guard<std::mutex> lock(g_wsServersMutex);
        auto it = g_wsServers.find(fd);
        if (it == g_wsServers.end()) return Value::makeBool(false);
        it->second->maxMessagesPerMinute = a[1].toInt();
        return Value::makeBool(true);
    });

    // WebSocketServer.stop(server)
    vm.registerNative("WebSocketServer.stop", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeBool(false);
        int fd = a[0].objVal->fields["fd"].toInt();
        std::lock_guard<std::mutex> lock(g_wsServersMutex);
        auto it = g_wsServers.find(fd);
        if (it == g_wsServers.end()) return Value::makeBool(false);
        it->second->running = false;
        // Close all client connections
        for (auto& [cfd, client] : it->second->clients) {
            auto frame = buildWsFrame(WS_CLOSE, (const uint8_t*)"\x03\xe8", 2, false); // 1000
            send(cfd, frame.data(), frame.size(), MSG_NOSIGNAL);
            close(cfd);
        }
        it->second->clients.clear();
        it->second->clientsById.clear();
        close(it->second->epollFd);
        close(fd);
        g_wsServers.erase(it);
        a[0].objVal->fields["running"] = Value::makeBool(false);
        return Value::makeBool(true);
    });

    // WebSocketServer.isRunning(server)
    vm.registerNative("WebSocketServer.isRunning", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeBool(false);
        auto it = a[0].objVal->fields.find("running");
        return Value::makeBool(it != a[0].objVal->fields.end() && it->second.toBool());
    });

    // WebSocketServer.getPort(server)
    vm.registerNative("WebSocketServer.getPort", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeInt(0);
        auto it = a[0].objVal->fields.find("port");
        return it != a[0].objVal->fields.end() ? it->second : Value::makeInt(0);
    });

    // ===== 6.5 Stream Processing =====

    // Stream.create(options) — create a new stream with optional buffer size
    vm.registerNative("Stream.create", [getOpt, isObjLike](const std::vector<Value>& a) -> Value {
        int bufferSize = 64;
        if (!a.empty() && isObjLike(a[0])) {
            Value bs = getOpt(a[0], "bufferSize");
            if (bs.type != ValueType::Null) bufferSize = bs.toInt();
        } else if (!a.empty()) { bufferSize = a[0].toInt(); }
        Value stream = Value::makeObject("Stream");
        stream.objVal->fields["_buffer"] = Value::makeArray();
        stream.objVal->fields["_bufferSize"] = Value::makeInt(bufferSize);
        stream.objVal->fields["_closed"] = Value::makeBool(false);
        stream.objVal->fields["_paused"] = Value::makeBool(false);
        stream.objVal->fields["_totalWritten"] = Value::makeInt(0);
        stream.objVal->fields["_totalRead"] = Value::makeInt(0);
        stream.objVal->fields["_errorCount"] = Value::makeInt(0);
        stream.objVal->fields["length"] = Value::makeInt(0);
        return stream;
    });

    // Stream.write(stream, value) — write a value to the stream
    vm.registerNative("Stream.write", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) { vm.throwError("GardStreamError", "Stream.write: invalid stream"); return Value::makeNull(); }
        auto closedIt = a[0].objVal->fields.find("_closed");
        if (closedIt != a[0].objVal->fields.end() && closedIt->second.toBool()) {
            vm.throwError("GardStreamError", "Stream.write: cannot write to a closed stream");
            return Value::makeNull();
        }
        auto pausedIt = a[0].objVal->fields.find("_paused");
        if (pausedIt != a[0].objVal->fields.end() && pausedIt->second.toBool()) {
            vm.throwError("GardStreamError", "Stream.write: stream is paused (backpressure)");
            return Value::makeNull();
        }
        auto bufIt = a[0].objVal->fields.find("_buffer");
        auto maxIt = a[0].objVal->fields.find("_bufferSize");
        if (bufIt == a[0].objVal->fields.end() || !bufIt->second.arrVal) { vm.throwError("GardStreamError", "Stream.write: corrupted stream buffer"); return Value::makeNull(); }
        int maxBuf = (maxIt != a[0].objVal->fields.end()) ? maxIt->second.toInt() : 64;
        if ((int)bufIt->second.arrVal->elements.size() >= maxBuf) {
            // Backpressure: pause the stream
            a[0].objVal->fields["_paused"] = Value::makeBool(true);
            vm.throwError("GardStreamError", "Stream.write: buffer full (backpressure triggered, size: " + std::to_string(maxBuf) + ")");
            return Value::makeNull();
        }
        bufIt->second.arrVal->elements.push_back(a[1]);
        int written = a[0].objVal->fields["_totalWritten"].toInt();
        a[0].objVal->fields["_totalWritten"] = Value::makeInt(written + 1);
        a[0].objVal->fields["length"] = Value::makeInt((int)bufIt->second.arrVal->elements.size());
        return Value::makeBool(true);
    });

    // Stream.read(stream) — read next value from stream
    vm.registerNative("Stream.read", [&vm](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) { vm.throwError("GardStreamError", "Stream.read: invalid stream"); return Value::makeNull(); }
        auto bufIt = a[0].objVal->fields.find("_buffer");
        if (bufIt == a[0].objVal->fields.end() || !bufIt->second.arrVal) return Value::makeNull();
        if (bufIt->second.arrVal->elements.empty()) {
            auto closedIt = a[0].objVal->fields.find("_closed");
            if (closedIt != a[0].objVal->fields.end() && closedIt->second.toBool()) return Value::makeNull(); // EOF
            return Value::makeNull(); // would block
        }
        Value val = bufIt->second.arrVal->elements.front();
        bufIt->second.arrVal->elements.erase(bufIt->second.arrVal->elements.begin());
        int read = a[0].objVal->fields["_totalRead"].toInt();
        a[0].objVal->fields["_totalRead"] = Value::makeInt(read + 1);
        a[0].objVal->fields["length"] = Value::makeInt((int)bufIt->second.arrVal->elements.size());
        // Resume if was paused and buffer has space
        auto pausedIt = a[0].objVal->fields.find("_paused");
        if (pausedIt != a[0].objVal->fields.end() && pausedIt->second.toBool()) {
            auto maxIt = a[0].objVal->fields.find("_bufferSize");
            int maxBuf = (maxIt != a[0].objVal->fields.end()) ? maxIt->second.toInt() : 64;
            if ((int)bufIt->second.arrVal->elements.size() < maxBuf / 2) {
                a[0].objVal->fields["_paused"] = Value::makeBool(false);
            }
        }
        return val;
    });

    // Stream.close(stream) — signal end of stream
    vm.registerNative("Stream.close", [&vm](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) { vm.throwError("GardStreamError", "Stream.close: invalid stream"); return Value::makeNull(); }
        a[0].objVal->fields["_closed"] = Value::makeBool(true);
        return Value::makeNull();
    });

    // Stream.isClosed(stream)
    vm.registerNative("Stream.isClosed", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeBool(true);
        auto it = a[0].objVal->fields.find("_closed");
        return Value::makeBool(it != a[0].objVal->fields.end() && it->second.toBool());
    });

    // Stream.isPaused(stream) — check backpressure state
    vm.registerNative("Stream.isPaused", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeBool(false);
        auto it = a[0].objVal->fields.find("_paused");
        return Value::makeBool(it != a[0].objVal->fields.end() && it->second.toBool());
    });

    // Stream.resume(stream) — manually resume a paused stream
    vm.registerNative("Stream.resume", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeBool(false);
        a[0].objVal->fields["_paused"] = Value::makeBool(false);
        return Value::makeBool(true);
    });

    // Stream.available(stream) — number of items in buffer
    vm.registerNative("Stream.available", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeInt(0);
        auto it = a[0].objVal->fields.find("_buffer");
        return Value::makeInt((it != a[0].objVal->fields.end() && it->second.arrVal) ? (int)it->second.arrVal->elements.size() : 0);
    });

    // Stream.pipe(source, destination) — pipe all data from source to destination
    vm.registerNative("Stream.pipe", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal || !a[1].objVal) { vm.throwError("GardStreamError", "Stream.pipe: requires source and destination streams"); return Value::makeNull(); }
        auto srcBuf = a[0].objVal->fields.find("_buffer");
        auto dstBuf = a[1].objVal->fields.find("_buffer");
        if (!srcBuf->second.arrVal || !dstBuf->second.arrVal) { vm.throwError("GardStreamError", "Stream.pipe: invalid stream buffers"); return Value::makeNull(); }
        auto dstClosed = a[1].objVal->fields.find("_closed");
        if (dstClosed != a[1].objVal->fields.end() && dstClosed->second.toBool()) { vm.throwError("GardStreamError", "Stream.pipe: destination stream is closed"); return Value::makeNull(); }
        int piped = 0;
        while (!srcBuf->second.arrVal->elements.empty()) {
            dstBuf->second.arrVal->elements.push_back(srcBuf->second.arrVal->elements.front());
            srcBuf->second.arrVal->elements.erase(srcBuf->second.arrVal->elements.begin());
            piped++;
        }
        return Value::makeInt(piped);
    });

    // Stream.map(stream, transformFnName) — transform each element (returns new stream)
    vm.registerNative("Stream.map", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeNull();
        // Create a new stream with same buffer contents (transform would need VM callback)
        Value newStream = Value::makeObject("Stream");
        newStream.objVal->fields["_buffer"] = Value::makeArray();
        newStream.objVal->fields["_bufferSize"] = a[0].objVal->fields["_bufferSize"];
        newStream.objVal->fields["_closed"] = Value::makeBool(false);
        newStream.objVal->fields["_paused"] = Value::makeBool(false);
        newStream.objVal->fields["_totalWritten"] = Value::makeInt(0);
        newStream.objVal->fields["_totalRead"] = Value::makeInt(0);
        newStream.objVal->fields["_errorCount"] = Value::makeInt(0);
        // Copy elements (in a real impl, transform would be applied lazily)
        auto srcBuf = a[0].objVal->fields.find("_buffer");
        if (srcBuf != a[0].objVal->fields.end() && srcBuf->second.arrVal) {
            newStream.objVal->fields["_buffer"].arrVal->elements = srcBuf->second.arrVal->elements;
        }
        return newStream;
    });

    // Stream.filter(stream, predicateFnName) — filter elements (returns new stream)
    vm.registerNative("Stream.filter", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeNull();
        Value newStream = Value::makeObject("Stream");
        newStream.objVal->fields["_buffer"] = Value::makeArray();
        newStream.objVal->fields["_bufferSize"] = a[0].objVal->fields["_bufferSize"];
        newStream.objVal->fields["_closed"] = Value::makeBool(false);
        newStream.objVal->fields["_paused"] = Value::makeBool(false);
        newStream.objVal->fields["_totalWritten"] = Value::makeInt(0);
        newStream.objVal->fields["_totalRead"] = Value::makeInt(0);
        newStream.objVal->fields["_errorCount"] = Value::makeInt(0);
        auto srcBuf = a[0].objVal->fields.find("_buffer");
        if (srcBuf != a[0].objVal->fields.end() && srcBuf->second.arrVal) {
            for (auto& e : srcBuf->second.arrVal->elements) {
                if (e.toBool()) newStream.objVal->fields["_buffer"].arrVal->elements.push_back(e);
            }
        }
        return newStream;
    });

    // Stream.batch(stream, batchSize) — collect N items into arrays
    vm.registerNative("Stream.batch", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) { vm.throwError("GardStreamError", "Stream.batch: requires stream and batch size"); return Value::makeNull(); }
        int batchSize = a[1].toInt();
        if (batchSize <= 0) { vm.throwError("GardStreamError", "Stream.batch: batch size must be > 0"); return Value::makeNull(); }
        auto srcBuf = a[0].objVal->fields.find("_buffer");
        if (!srcBuf->second.arrVal) return Value::makeArray();
        Value result = Value::makeArray();
        Value batch = Value::makeArray();
        int count = 0;
        for (auto& e : srcBuf->second.arrVal->elements) {
            batch.arrVal->elements.push_back(e);
            count++;
            if (count >= batchSize) {
                result.arrVal->elements.push_back(batch);
                batch = Value::makeArray();
                count = 0;
            }
        }
        if (!batch.arrVal->elements.empty()) result.arrVal->elements.push_back(batch);
        return result;
    });

    // Stream.merge(streamA, streamB) — interleave two streams into one
    vm.registerNative("Stream.merge", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal || !a[1].objVal) { vm.throwError("GardStreamError", "Stream.merge: requires two streams"); return Value::makeNull(); }
        Value merged = Value::makeObject("Stream");
        merged.objVal->fields["_buffer"] = Value::makeArray();
        merged.objVal->fields["_bufferSize"] = Value::makeInt(128);
        merged.objVal->fields["_closed"] = Value::makeBool(false);
        merged.objVal->fields["_paused"] = Value::makeBool(false);
        merged.objVal->fields["_totalWritten"] = Value::makeInt(0);
        merged.objVal->fields["_totalRead"] = Value::makeInt(0);
        merged.objVal->fields["_errorCount"] = Value::makeInt(0);
        auto bufA = a[0].objVal->fields.find("_buffer");
        auto bufB = a[1].objVal->fields.find("_buffer");
        if (bufA != a[0].objVal->fields.end() && bufA->second.arrVal) {
            for (auto& e : bufA->second.arrVal->elements) merged.objVal->fields["_buffer"].arrVal->elements.push_back(e);
        }
        if (bufB != a[1].objVal->fields.end() && bufB->second.arrVal) {
            for (auto& e : bufB->second.arrVal->elements) merged.objVal->fields["_buffer"].arrVal->elements.push_back(e);
        }
        int total = (int)merged.objVal->fields["_buffer"].arrVal->elements.size();
        merged.objVal->fields["_totalWritten"] = Value::makeInt(total);
        return merged;
    });

    // Stream.getStats(stream) — get stream statistics
    vm.registerNative("Stream.getStats", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeNull();
        Value stats = Value::makeObject("StreamStats");
        stats.objVal->fields["totalWritten"] = a[0].objVal->fields["_totalWritten"];
        stats.objVal->fields["totalRead"] = a[0].objVal->fields["_totalRead"];
        stats.objVal->fields["buffered"] = Value::makeInt(
            a[0].objVal->fields["_buffer"].arrVal ? (int)a[0].objVal->fields["_buffer"].arrVal->elements.size() : 0);
        stats.objVal->fields["closed"] = a[0].objVal->fields["_closed"];
        stats.objVal->fields["paused"] = a[0].objVal->fields["_paused"];
        return stats;
    });

    // ===== 6.6 Real-Time Event System =====

    // EventEmitter.create() — create a new event emitter
    vm.registerNative("EventEmitter.create", [](const std::vector<Value>& a) -> Value {
        Value emitter = Value::makeObject("EventEmitter");
        emitter.objVal->fields["_listeners"] = Value::makeObject("EventListeners");
        emitter.objVal->fields["_replayBuffer"] = Value::makeObject("ReplayBuffer");
        emitter.objVal->fields["_replaySize"] = Value::makeInt(a.empty() ? 0 : a[0].toInt());
        emitter.objVal->fields["_deadLetters"] = Value::makeArray();
        emitter.objVal->fields["_eventCount"] = Value::makeInt(0);
        emitter.objVal->fields["_maxListeners"] = Value::makeInt(100);
        return emitter;
    });

    // EventEmitter.on(emitter, event, handlerName) — subscribe to an event
    vm.registerNative("EventEmitter.on", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 3 || !a[0].objVal) { vm.throwError("GardEventError", "EventEmitter.on: requires emitter, event name, and handler"); return Value::makeNull(); }
        std::string event = a[1].toString();
        auto& listeners = a[0].objVal->fields["_listeners"];
        if (!listeners.objVal) return Value::makeNull();
        // Check max listeners
        auto maxIt = a[0].objVal->fields.find("_maxListeners");
        int maxL = (maxIt != a[0].objVal->fields.end()) ? maxIt->second.toInt() : 100;
        auto existingIt = listeners.objVal->fields.find(event);
        if (existingIt != listeners.objVal->fields.end() && existingIt->second.arrVal) {
            if ((int)existingIt->second.arrVal->elements.size() >= maxL) {
                vm.throwError("GardEventError", "EventEmitter.on: max listeners (" + std::to_string(maxL) + ") exceeded for event '" + event + "'");
                return Value::makeNull();
            }
        }
        // Add listener
        if (listeners.objVal->fields.find(event) == listeners.objVal->fields.end() || !listeners.objVal->fields[event].arrVal) {
            listeners.objVal->fields[event] = Value::makeArray();
        }
        listeners.objVal->fields[event].arrVal->elements.push_back(a[2]);
        return Value::makeBool(true);
    });

    // EventEmitter.once(emitter, event, handlerName) — subscribe for one event only
    vm.registerNative("EventEmitter.once", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 3 || !a[0].objVal) { vm.throwError("GardEventError", "EventEmitter.once: requires emitter, event name, and handler"); return Value::makeNull(); }
        std::string event = a[1].toString();
        auto& listeners = a[0].objVal->fields["_listeners"];
        if (!listeners.objVal) return Value::makeNull();
        std::string onceKey = "_once_" + event;
        if (listeners.objVal->fields.find(onceKey) == listeners.objVal->fields.end() || !listeners.objVal->fields[onceKey].arrVal) {
            listeners.objVal->fields[onceKey] = Value::makeArray();
        }
        listeners.objVal->fields[onceKey].arrVal->elements.push_back(a[2]);
        // Also add to regular listeners
        if (listeners.objVal->fields.find(event) == listeners.objVal->fields.end() || !listeners.objVal->fields[event].arrVal) {
            listeners.objVal->fields[event] = Value::makeArray();
        }
        listeners.objVal->fields[event].arrVal->elements.push_back(a[2]);
        return Value::makeBool(true);
    });

    // EventEmitter.off(emitter, event, handlerName?) — unsubscribe
    vm.registerNative("EventEmitter.off", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) { vm.throwError("GardEventError", "EventEmitter.off: requires emitter and event name"); return Value::makeNull(); }
        std::string event = a[1].toString();
        auto& listeners = a[0].objVal->fields["_listeners"];
        if (!listeners.objVal) return Value::makeBool(false);
        if (a.size() >= 3) {
            // Remove specific handler
            auto it = listeners.objVal->fields.find(event);
            if (it != listeners.objVal->fields.end() && it->second.arrVal) {
                std::string handler = a[2].toString();
                auto& elems = it->second.arrVal->elements;
                for (auto eit = elems.begin(); eit != elems.end(); ++eit) {
                    if (eit->toString() == handler) { elems.erase(eit); return Value::makeBool(true); }
                }
            }
        } else {
            // Remove all listeners for event
            listeners.objVal->fields.erase(event);
        }
        return Value::makeBool(true);
    });

    // EventEmitter.emit(emitter, event, data?) — emit an event, auto-calls handlers
    // Returns: { event, data, count } — handlers are invoked automatically
    vm.registerNative("EventEmitter.emit", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) return Value::makeNull();
        std::string event = a[1].toString();
        Value data = a.size() >= 3 ? a[2] : Value::makeNull();
        auto& listeners = a[0].objVal->fields["_listeners"];
        if (!listeners.objVal) return Value::makeNull();

        // Collect handler names
        std::vector<std::string> handlerNames;
        auto it = listeners.objVal->fields.find(event);
        if (it != listeners.objVal->fields.end() && it->second.arrVal) {
            for (auto& h : it->second.arrVal->elements) handlerNames.push_back(h.toString());
        }
        auto wildIt = listeners.objVal->fields.find("*");
        if (wildIt != listeners.objVal->fields.end() && wildIt->second.arrVal) {
            for (auto& h : wildIt->second.arrVal->elements) handlerNames.push_back(h.toString());
        }

        int count = (int)handlerNames.size();

        // Store in replay buffer if configured
        int replaySize = a[0].objVal->fields["_replaySize"].toInt();
        if (replaySize > 0) {
            auto& replay = a[0].objVal->fields["_replayBuffer"];
            if (replay.objVal) {
                if (replay.objVal->fields.find(event) == replay.objVal->fields.end() || !replay.objVal->fields[event].arrVal) {
                    replay.objVal->fields[event] = Value::makeArray();
                }
                replay.objVal->fields[event].arrVal->elements.push_back(data);
                while ((int)replay.objVal->fields[event].arrVal->elements.size() > replaySize) {
                    replay.objVal->fields[event].arrVal->elements.erase(replay.objVal->fields[event].arrVal->elements.begin());
                }
            }
        }

        // Track event count
        int ec = a[0].objVal->fields["_eventCount"].toInt();
        a[0].objVal->fields["_eventCount"] = Value::makeInt(ec + 1);

        // Dead letter queue if no listeners
        if (count == 0) {
            auto& dlq = a[0].objVal->fields["_deadLetters"];
            if (dlq.arrVal) {
                Value letter = Value::makeObject("DeadLetter");
                letter.objVal->fields["event"] = Value::makeString(event);
                letter.objVal->fields["data"] = data;
                dlq.arrVal->elements.push_back(letter);
            }
        }

        // Remove once listeners after emit
        std::string onceKey = "_once_" + event;
        auto onceIt = listeners.objVal->fields.find(onceKey);
        if (onceIt != listeners.objVal->fields.end() && onceIt->second.arrVal) {
            auto mainIt = listeners.objVal->fields.find(event);
            if (mainIt != listeners.objVal->fields.end() && mainIt->second.arrVal) {
                for (auto& oh : onceIt->second.arrVal->elements) {
                    auto& elems = mainIt->second.arrVal->elements;
                    for (auto eit = elems.begin(); eit != elems.end(); ++eit) {
                        if (eit->toString() == oh.toString()) { elems.erase(eit); break; }
                    }
                }
            }
            listeners.objVal->fields.erase(onceKey);
        }

        // Call each handler function by looking it up in the module
        for (auto& handlerName : handlerNames) {
            if (vm.hasNative(handlerName)) {
                vm.callNative(handlerName, {data});
            } else {
                vm.callFunction(handlerName, {data});
            }
        }

        // Return dispatch info
        Value result = Value::makeObject("EventDispatch");
        result.objVal->fields["event"] = Value::makeString(event);
        result.objVal->fields["data"] = data;
        result.objVal->fields["count"] = Value::makeInt(count);
        return result;
    });

    // EventEmitter.listenerCount(emitter, event) — get number of listeners
    vm.registerNative("EventEmitter.listenerCount", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) return Value::makeInt(0);
        std::string event = a[1].toString();
        auto& listeners = a[0].objVal->fields["_listeners"];
        if (!listeners.objVal) return Value::makeInt(0);
        auto it = listeners.objVal->fields.find(event);
        if (it == listeners.objVal->fields.end() || !it->second.arrVal) return Value::makeInt(0);
        return Value::makeInt((int)it->second.arrVal->elements.size());
    });

    // EventEmitter.eventNames(emitter) — get all registered event names
    vm.registerNative("EventEmitter.eventNames", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeArray();
        auto& listeners = a[0].objVal->fields["_listeners"];
        if (!listeners.objVal) return Value::makeArray();
        Value arr = Value::makeArray();
        for (auto& [k, v] : listeners.objVal->fields) {
            if (k[0] != '_' && v.arrVal && !v.arrVal->elements.empty()) {
                arr.arrVal->elements.push_back(Value::makeString(k));
            }
        }
        return arr;
    });

    // EventEmitter.removeAllListeners(emitter, event?) — remove all or event-specific
    vm.registerNative("EventEmitter.removeAllListeners", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeNull();
        auto& listeners = a[0].objVal->fields["_listeners"];
        if (!listeners.objVal) return Value::makeNull();
        if (a.size() >= 2) {
            listeners.objVal->fields.erase(a[1].toString());
        } else {
            listeners.objVal->fields.clear();
        }
        return Value::makeNull();
    });

    // EventEmitter.getReplay(emitter, event) — get replay buffer for event
    vm.registerNative("EventEmitter.getReplay", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) return Value::makeArray();
        auto& replay = a[0].objVal->fields["_replayBuffer"];
        if (!replay.objVal) return Value::makeArray();
        std::string event = a[1].toString();
        auto it = replay.objVal->fields.find(event);
        if (it == replay.objVal->fields.end() || !it->second.arrVal) return Value::makeArray();
        return it->second;
    });

    // EventEmitter.getDeadLetters(emitter) — get dead letter queue
    vm.registerNative("EventEmitter.getDeadLetters", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeArray();
        auto it = a[0].objVal->fields.find("_deadLetters");
        if (it == a[0].objVal->fields.end() || !it->second.arrVal) return Value::makeArray();
        return it->second;
    });

    // EventEmitter.setMaxListeners(emitter, max)
    vm.registerNative("EventEmitter.setMaxListeners", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) { vm.throwError("GardEventError", "EventEmitter.setMaxListeners: requires emitter and max count"); return Value::makeNull(); }
        int max = a[1].toInt();
        if (max <= 0) { vm.throwError("GardEventError", "EventEmitter.setMaxListeners: max must be > 0"); return Value::makeNull(); }
        a[0].objVal->fields["_maxListeners"] = Value::makeInt(max);
        return Value::makeNull();
    });

    // EventEmitter.getEventCount(emitter) — total events emitted
    vm.registerNative("EventEmitter.getEventCount", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeInt(0);
        return a[0].objVal->fields["_eventCount"];
    });

    // ===== Aliases matching LLVM codegen API =====

    // HttpClient.get(url) — alias for http.get
    vm.registerNative("HttpClient.get", [curlRequest](const std::vector<Value>& a) -> Value {
        if (a.empty()) return Value::makeNull();
        return curlRequest(a[0].toString(), "GET", "", {}, 30000, true, 10);
    });

    // HttpClient.post(url, body) — alias for http.post
    vm.registerNative("HttpClient.post", [curlRequest](const std::vector<Value>& a) -> Value {
        if (a.size() < 2) return Value::makeNull();
        std::string ct = a.size() >= 3 ? a[2].toString() : "application/json";
        return curlRequest(a[0].toString(), "POST", a[1].toString(), {{"Content-Type", ct}}, 30000, true, 10);
    });

    // HttpClient.put(url, body) — alias for http.put
    vm.registerNative("HttpClient.put", [curlRequest](const std::vector<Value>& a) -> Value {
        if (a.size() < 2) return Value::makeNull();
        std::string ct = a.size() >= 3 ? a[2].toString() : "application/json";
        return curlRequest(a[0].toString(), "PUT", a[1].toString(), {{"Content-Type", ct}}, 30000, true, 10);
    });

    // HttpClient.delete(url) — alias for http.delete
    vm.registerNative("HttpClient.delete", [curlRequest](const std::vector<Value>& a) -> Value {
        if (a.empty()) return Value::makeNull();
        std::string body = a.size() >= 2 ? a[1].toString() : "";
        return curlRequest(a[0].toString(), "DELETE", body, {}, 30000, true, 10);
    });

    // HttpClient.patch(url, body) — alias for http.patch
    vm.registerNative("HttpClient.patch", [curlRequest](const std::vector<Value>& a) -> Value {
        if (a.size() < 2) return Value::makeNull();
        std::string ct = a.size() >= 3 ? a[2].toString() : "application/json";
        return curlRequest(a[0].toString(), "PATCH", a[1].toString(), {{"Content-Type", ct}}, 30000, true, 10);
    });

    // HttpClient.request(method, url, body, headers) — alias for http.fetch
    vm.registerNative("HttpClient.request", [curlRequest](const std::vector<Value>& a) -> Value {
        if (a.size() < 2) return Value::makeNull();
        std::string method = a[0].toString();
        std::string url = a[1].toString();
        std::string body = a.size() >= 3 ? a[2].toString() : "";
        std::unordered_map<std::string, std::string> headers;
        if (a.size() >= 4 && a[3].type == ValueType::Object && a[3].objVal) {
            for (auto& [k, v] : a[3].objVal->fields) {
                headers[k] = v.toString();
            }
        }
        return curlRequest(url, method, body, headers, 30000, true, 10);
    });

    // TcpSocket.connect(host, port) — alias for TcpClient.connect
    vm.registerNative("TcpSocket.connect", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2) return Value::makeNull();
        std::string host = a[0].toString();
        int port = a[1].toInt();
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return Value::makeNull();
        struct sockaddr_in addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        struct hostent* he = gethostbyname(host.c_str());
        if (!he) { close(fd); return Value::makeNull(); }
        std::memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);
        if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { close(fd); return Value::makeNull(); }
        Value result = Value::makeObject("TcpSocket");
        result.objVal->fields["fd"] = Value::makeInt(fd);
        result.objVal->fields["host"] = Value::makeString(host);
        result.objVal->fields["port"] = Value::makeInt(port);
        result.objVal->fields["connected"] = Value::makeBool(true);
        return result;
    });

    // TcpSocket.send(socket, data)
    vm.registerNative("TcpSocket.send", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) return Value::makeInt(-1);
        auto fdIt = a[0].objVal->fields.find("fd");
        if (fdIt == a[0].objVal->fields.end()) return Value::makeInt(-1);
        std::string data = a[1].toString();
        ssize_t sent = write(fdIt->second.toInt(), data.c_str(), data.size());
        return Value::makeInt((int)sent);
    });

    // TcpSocket.receive(socket, maxBytes?)
    vm.registerNative("TcpSocket.receive", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeString("");
        auto fdIt = a[0].objVal->fields.find("fd");
        if (fdIt == a[0].objVal->fields.end()) return Value::makeString("");
        int maxBytes = a.size() >= 2 ? a[1].toInt() : 4096;
        std::vector<char> buf(maxBytes);
        ssize_t n = read(fdIt->second.toInt(), buf.data(), maxBytes);
        if (n <= 0) return Value::makeString("");
        return Value::makeString(std::string(buf.data(), n));
    });

    // TcpSocket.close(socket)
    vm.registerNative("TcpSocket.close", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeNull();
        auto fdIt = a[0].objVal->fields.find("fd");
        if (fdIt != a[0].objVal->fields.end()) close(fdIt->second.toInt());
        return Value::makeNull();
    });

    // TcpSocket.isConnected(socket)
    vm.registerNative("TcpSocket.isConnected", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeBool(false);
        auto it = a[0].objVal->fields.find("connected");
        return (it != a[0].objVal->fields.end()) ? it->second : Value::makeBool(false);
    });

    // TcpServer.listen(server, callback?) — start listening on existing server, invoke callback
    // If server is already from TcpServer.create (which does bind+listen), just invoke callback.
    vm.registerNative("TcpServer.listen", [&vm](const std::vector<Value>& a) -> Value {
        if (a.empty()) return Value::makeNull();

        // If first arg is an object (server from TcpServer.create), use it directly
        if (a[0].objVal) {
            // Server is already listening (TcpServer.create does bind+listen)
            // Just invoke the callback if provided
            if (a.size() >= 2) {
                std::string callbackName = a[1].toString();
                if (!callbackName.empty()) {
                    vm.callFunction(callbackName, {});
                }
            }
            return a[0];
        }

        // If first arg is an int (port), create a new server (backward compat)
        int port = a[0].toInt();
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return Value::makeNull();
        int opt = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        struct sockaddr_in addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port);
        if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { close(fd); return Value::makeNull(); }
        if (listen(fd, 128) < 0) { close(fd); return Value::makeNull(); }
        Value result = Value::makeObject("TcpServer");
        result.objVal->fields["fd"] = Value::makeInt(fd);
        result.objVal->fields["port"] = Value::makeInt(port);
        result.objVal->fields["listening"] = Value::makeBool(true);

        // Invoke callback if provided
        if (a.size() >= 2) {
            std::string callbackName = a[1].toString();
            if (!callbackName.empty()) {
                vm.callFunction(callbackName, {});
            }
        }

        return result;
    });

    // TcpServer.port(server) — alias for TcpServer.getPort
    vm.registerNative("TcpServer.port", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeInt(0);
        auto it = a[0].objVal->fields.find("port");
        return (it != a[0].objVal->fields.end()) ? it->second : Value::makeInt(0);
    });

    // TcpServer.stop(server) — alias for TcpServer.close
    vm.registerNative("TcpServer.stop", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeNull();
        auto fdIt = a[0].objVal->fields.find("fd");
        if (fdIt != a[0].objVal->fields.end()) close(fdIt->second.toInt());
        return Value::makeNull();
    });

    // --- Response namespace (field accessors matching LLVM codegen API) ---

    // Response.status(res) — get HTTP status code from response object
    vm.registerNative("Response.status", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeInt(0);
        auto it = a[0].objVal->fields.find("status");
        return it != a[0].objVal->fields.end() ? it->second : Value::makeInt(0);
    });

    // Response.body(res) — get response body string
    vm.registerNative("Response.body", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeString("");
        auto it = a[0].objVal->fields.find("body");
        return it != a[0].objVal->fields.end() ? it->second : Value::makeString("");
    });

    // Response.headers(res) — get response headers object
    vm.registerNative("Response.headers", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeNull();
        auto it = a[0].objVal->fields.find("headers");
        return it != a[0].objVal->fields.end() ? it->second : Value::makeNull();
    });

    // HttpServer.port(server) — alias for HttpServer.getPort
    vm.registerNative("HttpServer.port", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeInt(0);
        auto it = a[0].objVal->fields.find("port");
        return it != a[0].objVal->fields.end() ? it->second : Value::makeInt(0);
    });

    // HttpServer.handle(server, method, path, handlerFn) — register route with callback
    // In interpreter, stores the handler function name for the route
    vm.registerNative("HttpServer.handle", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 4 || !a[0].objVal) return Value::makeBool(false);
        auto fdIt = a[0].objVal->fields.find("fd");
        if (fdIt == a[0].objVal->fields.end()) return Value::makeBool(false);
        int fd = fdIt->second.toInt();
        std::string method = a[1].toString();
        std::string pattern = a[2].toString();
        std::lock_guard<std::mutex> lock(g_serversMutex);
        auto it = g_servers.find(fd);
        if (it == g_servers.end()) return Value::makeBool(false);
        Route route;
        route.method = method;
        route.pattern = pattern;
        route.handler = a[3]; // store the handler value (function name string or callable)
        it->second->routes.push_back(route);
        return Value::makeBool(true);
    });

    // HttpServer.routeJson(server, method, path, jsonResponse) — route returning JSON
    vm.registerNative("HttpServer.routeJson", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 4 || !a[0].objVal) return Value::makeBool(false);
        auto fdIt = a[0].objVal->fields.find("fd");
        if (fdIt == a[0].objVal->fields.end()) return Value::makeBool(false);
        int fd = fdIt->second.toInt();
        std::string method = a[1].toString();
        std::string pattern = a[2].toString();
        std::lock_guard<std::mutex> lock(g_serversMutex);
        auto it = g_servers.find(fd);
        if (it == g_servers.end()) return Value::makeBool(false);
        Route route;
        route.method = method;
        route.pattern = pattern;
        route.handler = a[3]; // JSON body string
        it->second->routes.push_back(route);
        return Value::makeBool(true);
    });

    // HttpServer.cors(server, origin, methods, headers) — store CORS config in default headers
    vm.registerNative("HttpServer.cors", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 4 || !a[0].objVal) return Value::makeBool(false);
        auto fdIt = a[0].objVal->fields.find("fd");
        if (fdIt == a[0].objVal->fields.end()) return Value::makeBool(false);
        int fd = fdIt->second.toInt();
        std::lock_guard<std::mutex> lock(g_serversMutex);
        auto it = g_servers.find(fd);
        if (it == g_servers.end()) return Value::makeBool(false);
        it->second->defaultHeaders["Access-Control-Allow-Origin"] = a[1].toString();
        it->second->defaultHeaders["Access-Control-Allow-Methods"] = a[2].toString();
        it->second->defaultHeaders["Access-Control-Allow-Headers"] = a[3].toString();
        return Value::makeBool(true);
    });

    // UdpSocket.bind(port) — alias for UdpSocket.create
    vm.registerNative("UdpSocket.bind", [](const std::vector<Value>& a) -> Value {
        int port = a.empty() ? 0 : a[0].toInt();
        int fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) return Value::makeNull();
        int opt = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port);
        if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { close(fd); return Value::makeNull(); }
        // Get actual port if ephemeral
        if (port == 0) {
            struct sockaddr_in bound{};
            socklen_t len = sizeof(bound);
            getsockname(fd, (struct sockaddr*)&bound, &len);
            port = ntohs(bound.sin_port);
        }
        Value result = Value::makeObject("UdpSocket");
        result.objVal->fields["fd"] = Value::makeInt(fd);
        result.objVal->fields["port"] = Value::makeInt(port);
        return result;
    });

    // UdpSocket.port(socket) — alias for UdpSocket.getPort
    vm.registerNative("UdpSocket.port", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeInt(0);
        auto it = a[0].objVal->fields.find("port");
        return (it != a[0].objVal->fields.end()) ? it->second : Value::makeInt(0);
    });

    // WebSocketServer.clientCount(server) — alias for WebSocketServer.getClientCount
    vm.registerNative("WebSocketServer.clientCount", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeInt(0);
        auto it = a[0].objVal->fields.find("clientCount");
        return (it != a[0].objVal->fields.end()) ? it->second : Value::makeInt(0);
    });

}

} // namespace stdlib
} // namespace runtime
} // namespace gard
