// Gard Network Runtime — HTTP Client (libcurl)
// Provides HttpClient.get/post/put/delete/patch for AOT binaries.
// Link with: -lcurl

#ifndef GARD_RUNTIME_NET_H
#define GARD_RUNTIME_NET_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// HTTP Response
typedef struct {
    char* body;
    int32_t status_code;
    char* headers;
} GardHttpResponse;

// HttpClient methods (exact names from stdlib_network.cpp)
GardHttpResponse* gard_http_get(const char* url);
GardHttpResponse* gard_http_post(const char* url, const char* body);
GardHttpResponse* gard_http_put(const char* url, const char* body);
GardHttpResponse* gard_http_delete(const char* url);
GardHttpResponse* gard_http_patch(const char* url, const char* body);
GardHttpResponse* gard_http_request(const char* method, const char* url, const char* body, const char* headers);

// Response accessors
const char* gard_http_response_body(GardHttpResponse* res);
int32_t gard_http_response_status(GardHttpResponse* res);
const char* gard_http_response_headers(GardHttpResponse* res);
void gard_http_response_free(GardHttpResponse* res);

// ============================================================
// HTTP Server (libmicrohttpd — production-grade, multi-threaded, epoll)
// Features: HTTP/1.1, keep-alive, chunked, routing with path params,
//           JSON responses, middleware, graceful shutdown
// ============================================================

typedef struct GardHttpServer GardHttpServer;
typedef struct GardRequest GardRequest;
typedef struct GardResponse GardResponse;

// Server lifecycle
GardHttpServer* gard_http_server_create(int32_t port);
int32_t gard_http_server_listen(GardHttpServer* server);
void gard_http_server_stop(GardHttpServer* server);
int32_t gard_http_server_is_running(GardHttpServer* server);
int32_t gard_http_server_port(GardHttpServer* server);

// Route registration
// handler_id is an integer that identifies which handler to call
void gard_http_server_route(GardHttpServer* server, const char* method, const char* path, const char* static_response);
void gard_http_server_route_json(GardHttpServer* server, const char* method, const char* path, const char* json_response);
void gard_http_server_route_status(GardHttpServer* server, const char* method, const char* path, int32_t status, const char* body);

// CORS configuration
void gard_http_server_cors(GardHttpServer* server, const char* origin, const char* methods, const char* headers);

// Request accessors
const char* gard_request_method(GardRequest* req);
const char* gard_request_path(GardRequest* req);
const char* gard_request_body(GardRequest* req);
const char* gard_request_header(GardRequest* req, const char* name);
const char* gard_request_param(GardRequest* req, const char* name);
const char* gard_request_query(GardRequest* req, const char* name);

// ============================================================
// WebSocket Client (RFC 6455)
// ============================================================

typedef struct GardWebSocket GardWebSocket;

GardWebSocket* gard_ws_connect(const char* url);
int32_t gard_ws_send(GardWebSocket* ws, const char* message);
char* gard_ws_receive(GardWebSocket* ws);
void gard_ws_close(GardWebSocket* ws);
int32_t gard_ws_is_connected(GardWebSocket* ws);

// ============================================================
// WebSocket Server (libwebsockets — production-grade)
// ============================================================

typedef struct GardWsServer GardWsServer;

GardWsServer* gard_ws_server_create(int32_t port);
int32_t gard_ws_server_listen(GardWsServer* server);
void gard_ws_server_stop(GardWsServer* server);
int32_t gard_ws_server_is_running(GardWsServer* server);
int32_t gard_ws_server_client_count(GardWsServer* server);
void gard_ws_server_broadcast(GardWsServer* server, const char* message);
char* gard_ws_server_receive(GardWsServer* server);

// ============================================================
// TCP Socket (POSIX — production-grade)
// ============================================================

typedef struct GardTcpSocket GardTcpSocket;
typedef struct GardTcpServer GardTcpServer;

// TCP Client
GardTcpSocket* gard_tcp_connect(const char* host, int32_t port);
int32_t gard_tcp_send(GardTcpSocket* sock, const char* data);
char* gard_tcp_receive(GardTcpSocket* sock);
int32_t gard_tcp_receive_bytes(GardTcpSocket* sock, char* buf, int32_t max_len);
void gard_tcp_close(GardTcpSocket* sock);
int32_t gard_tcp_is_connected(GardTcpSocket* sock);

// TCP Server
GardTcpServer* gard_tcp_server_create(int32_t port);
int32_t gard_tcp_server_listen(GardTcpServer* server);
GardTcpSocket* gard_tcp_server_accept(GardTcpServer* server);
void gard_tcp_server_stop(GardTcpServer* server);
int32_t gard_tcp_server_port(GardTcpServer* server);

// ============================================================
// UDP Socket (POSIX — production-grade)
// ============================================================

typedef struct GardUdpSocket GardUdpSocket;

GardUdpSocket* gard_udp_bind(int32_t port);
int32_t gard_udp_send(GardUdpSocket* sock, const char* host, int32_t port, const char* data);
char* gard_udp_receive(GardUdpSocket* sock);
void gard_udp_close(GardUdpSocket* sock);
int32_t gard_udp_port(GardUdpSocket* sock);

#ifdef __cplusplus
}
#endif

#endif // GARD_RUNTIME_NET_H
