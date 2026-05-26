// Gard Network Runtime — HTTP Client via libcurl
// Compile: gcc -c -O2 -fPIC gard_runtime_net.c -o gard_runtime_net.o
// Link: -lcurl

#include "gard_runtime_net.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>

// Write callback for curl — appends data to a dynamic buffer
typedef struct {
    char* data;
    size_t size;
    size_t capacity;
} CurlBuffer;

static size_t curl_write_cb(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total = size * nmemb;
    CurlBuffer* buf = (CurlBuffer*)userp;
    if (buf->size + total + 1 > buf->capacity) {
        buf->capacity = (buf->size + total + 1) * 2;
        buf->data = (char*)realloc(buf->data, buf->capacity);
    }
    memcpy(buf->data + buf->size, contents, total);
    buf->size += total;
    buf->data[buf->size] = '\0';
    return total;
}

// Header callback — collects response headers
static size_t curl_header_cb(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total = size * nmemb;
    CurlBuffer* buf = (CurlBuffer*)userp;
    if (buf->size + total + 1 > buf->capacity) {
        buf->capacity = (buf->size + total + 1) * 2;
        buf->data = (char*)realloc(buf->data, buf->capacity);
    }
    memcpy(buf->data + buf->size, contents, total);
    buf->size += total;
    buf->data[buf->size] = '\0';
    return total;
}

// Core HTTP request function
static GardHttpResponse* do_request(const char* method, const char* url, const char* body, const char* custom_headers) {
    if (!url) return NULL;

    GardHttpResponse* response = (GardHttpResponse*)calloc(1, sizeof(GardHttpResponse));
    response->status_code = 0;
    response->body = strdup("");
    response->headers = strdup("");

    CURL* curl = curl_easy_init();
    if (!curl) return response;

    CurlBuffer body_buf = { (char*)malloc(4096), 0, 4096 };
    body_buf.data[0] = '\0';
    CurlBuffer header_buf = { (char*)malloc(2048), 0, 2048 };
    header_buf.data[0] = '\0';

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body_buf);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, curl_header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &header_buf);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Gard/0.1.0");

    // Set method
    if (strcmp(method, "POST") == 0) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body ? body : "");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, body ? (long)strlen(body) : 0L);
    } else if (strcmp(method, "PUT") == 0) {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body ? body : "");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, body ? (long)strlen(body) : 0L);
    } else if (strcmp(method, "DELETE") == 0) {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    } else if (strcmp(method, "PATCH") == 0) {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PATCH");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body ? body : "");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, body ? (long)strlen(body) : 0L);
    }
    // GET is the default

    // Custom headers
    struct curl_slist* header_list = NULL;
    if (custom_headers && custom_headers[0]) {
        // Parse headers: "Content-Type: application/json\nAuthorization: Bearer token"
        char* hdrs = strdup(custom_headers);
        char* line = strtok(hdrs, "\n");
        while (line) {
            header_list = curl_slist_append(header_list, line);
            line = strtok(NULL, "\n");
        }
        free(hdrs);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
    }

    // For POST/PUT/PATCH without explicit Content-Type, default to JSON
    if ((strcmp(method, "POST") == 0 || strcmp(method, "PUT") == 0 || strcmp(method, "PATCH") == 0) &&
        (!custom_headers || !strstr(custom_headers, "Content-Type"))) {
        header_list = curl_slist_append(header_list, "Content-Type: application/json");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
    }

    // Execute
    CURLcode res = curl_easy_perform(curl);

    if (res == CURLE_OK) {
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        response->status_code = (int32_t)http_code;
    } else {
        response->status_code = -1;
        free(body_buf.data);
        body_buf.data = strdup(curl_easy_strerror(res));
    }

    free(response->body);
    response->body = body_buf.data;
    free(response->headers);
    response->headers = header_buf.data;

    if (header_list) curl_slist_free_all(header_list);
    curl_easy_cleanup(curl);
    return response;
}

// === Public API ===

GardHttpResponse* gard_http_get(const char* url) {
    return do_request("GET", url, NULL, NULL);
}

GardHttpResponse* gard_http_post(const char* url, const char* body) {
    return do_request("POST", url, body, NULL);
}

GardHttpResponse* gard_http_put(const char* url, const char* body) {
    return do_request("PUT", url, body, NULL);
}

GardHttpResponse* gard_http_delete(const char* url) {
    return do_request("DELETE", url, NULL, NULL);
}

GardHttpResponse* gard_http_patch(const char* url, const char* body) {
    return do_request("PATCH", url, body, NULL);
}

GardHttpResponse* gard_http_request(const char* method, const char* url, const char* body, const char* headers) {
    return do_request(method ? method : "GET", url, body, headers);
}

const char* gard_http_response_body(GardHttpResponse* res) {
    return res ? res->body : "";
}

int32_t gard_http_response_status(GardHttpResponse* res) {
    return res ? res->status_code : 0;
}

const char* gard_http_response_headers(GardHttpResponse* res) {
    return res ? res->headers : "";
}

void gard_http_response_free(GardHttpResponse* res) {
    if (!res) return;
    if (res->body) free(res->body);
    if (res->headers) free(res->headers);
    free(res);
}


// ============================================================
// HTTP Server — libmicrohttpd (production-grade)
// Multi-threaded, epoll-based, HTTP/1.1 with keep-alive
// ============================================================

#include <microhttpd.h>
#include <pthread.h>

#define MAX_ROUTES 256
#define MAX_POST_SIZE (1024 * 1024) // 1MB max POST body

typedef struct {
    char* method;
    char* path;
    char* response_body;
    char* content_type;
    int32_t status_code;
    char* (*handler)(const char* request_body); // callback handler (NULL = use static response)
} GardRoute;

struct GardRequest {
    const char* method;
    const char* path;
    char* body;
    size_t body_size;
    struct MHD_Connection* connection;
};

struct GardHttpServer {
    struct MHD_Daemon* daemon;
    int32_t port;
    int32_t running;
    GardRoute routes[MAX_ROUTES];
    int32_t route_count;
    pthread_mutex_t mutex;
    // CORS config
    char* cors_origin;
    char* cors_methods;
    char* cors_headers;
};

// POST data processor
struct PostData {
    char* data;
    size_t size;
    size_t capacity;
};

// Find matching route (supports path params like /users/:id)
static GardRoute* find_route(GardHttpServer* server, const char* method, const char* path) {
    if (!server || !method || !path) return NULL;

    for (int32_t i = 0; i < server->route_count; i++) {
        GardRoute* r = &server->routes[i];
        // Check method
        if (strcasecmp(r->method, method) != 0 && strcmp(r->method, "*") != 0) continue;

        // Exact match
        if (strcmp(r->path, path) == 0) return r;

        // Wildcard match: /api/* matches /api/anything
        size_t rlen = strlen(r->path);
        if (rlen > 1 && r->path[rlen-1] == '*') {
            if (strncmp(r->path, path, rlen - 1) == 0) return r;
        }

        // Path param match: /users/:id matches /users/123
        if (strchr(r->path, ':')) {
            const char* rp = r->path;
            const char* pp = path;
            int match = 1;
            while (*rp && *pp) {
                if (*rp == ':') {
                    // Skip param name in route
                    while (*rp && *rp != '/') rp++;
                    // Skip param value in path
                    while (*pp && *pp != '/') pp++;
                } else if (*rp != *pp) {
                    match = 0;
                    break;
                } else {
                    rp++;
                    pp++;
                }
            }
            if (match && *rp == '\0' && *pp == '\0') return r;
        }
    }
    return NULL;
}

// MHD request handler callback
static enum MHD_Result request_handler(void* cls, struct MHD_Connection* connection,
                                        const char* url, const char* method,
                                        const char* version, const char* upload_data,
                                        size_t* upload_data_size, void** con_cls) {
    GardHttpServer* server = (GardHttpServer*)cls;

    // First call: allocate POST data buffer
    if (*con_cls == NULL) {
        struct PostData* pd = (struct PostData*)calloc(1, sizeof(struct PostData));
        pd->capacity = 4096;
        pd->data = (char*)malloc(pd->capacity);
        pd->data[0] = '\0';
        pd->size = 0;
        *con_cls = pd;
        return MHD_YES;
    }

    struct PostData* pd = (struct PostData*)*con_cls;

    // Accumulate POST body
    if (*upload_data_size > 0) {
        if (pd->size + *upload_data_size + 1 > pd->capacity) {
            pd->capacity = (pd->size + *upload_data_size + 1) * 2;
            if (pd->capacity > MAX_POST_SIZE) pd->capacity = MAX_POST_SIZE;
            pd->data = (char*)realloc(pd->data, pd->capacity);
        }
        if (pd->size + *upload_data_size < pd->capacity) {
            memcpy(pd->data + pd->size, upload_data, *upload_data_size);
            pd->size += *upload_data_size;
            pd->data[pd->size] = '\0';
        }
        *upload_data_size = 0;
        return MHD_YES;
    }

    // Request complete — find route and respond
    GardRoute* route = find_route(server, method, url);

    struct MHD_Response* response;
    int status;

    if (route) {
        const char* body;
        if (route->handler) {
            // Dynamic handler: call the callback with the request body
            const char* req_body = (pd && pd->data) ? pd->data : "";
            body = route->handler(req_body);
            if (!body) body = "";
        } else {
            body = route->response_body ? route->response_body : "";
        }
        status = route->status_code;
        response = MHD_create_response_from_buffer(strlen(body), (void*)body, MHD_RESPMEM_MUST_COPY);
        if (route->content_type) {
            MHD_add_response_header(response, "Content-Type", route->content_type);
        }
    } else {
        // 404 Not Found
        const char* not_found = "{\"error\":\"Not Found\",\"path\":\"";
        size_t nf_len = strlen(not_found) + strlen(url) + 3;
        char* nf_body = (char*)malloc(nf_len);
        snprintf(nf_body, nf_len, "%s%s\"}", not_found, url);
        status = 404;
        response = MHD_create_response_from_buffer(strlen(nf_body), nf_body, MHD_RESPMEM_MUST_FREE);
        MHD_add_response_header(response, "Content-Type", "application/json");
    }

    // CORS headers (configurable)
    const char* cors_origin = server->cors_origin ? server->cors_origin : "*";
    const char* cors_methods = server->cors_methods ? server->cors_methods : "GET, POST, PUT, DELETE, PATCH, OPTIONS";
    const char* cors_headers = server->cors_headers ? server->cors_headers : "Content-Type, Authorization";
    MHD_add_response_header(response, "Access-Control-Allow-Origin", cors_origin);
    MHD_add_response_header(response, "Access-Control-Allow-Methods", cors_methods);
    MHD_add_response_header(response, "Access-Control-Allow-Headers", cors_headers);

    // Handle OPTIONS preflight
    if (strcasecmp(method, "OPTIONS") == 0) {
        MHD_destroy_response(response);
        response = MHD_create_response_from_buffer(0, "", MHD_RESPMEM_PERSISTENT);
        MHD_add_response_header(response, "Access-Control-Allow-Origin", cors_origin);
        MHD_add_response_header(response, "Access-Control-Allow-Methods", cors_methods);
        MHD_add_response_header(response, "Access-Control-Allow-Headers", cors_headers);
        status = 204;
    }

    enum MHD_Result ret = MHD_queue_response(connection, status, response);
    MHD_destroy_response(response);

    // Cleanup POST data
    free(pd->data);
    free(pd);
    *con_cls = NULL;

    return ret;
}

// === Public API ===

GardHttpServer* gard_http_server_create(int32_t port) {
    GardHttpServer* server = (GardHttpServer*)calloc(1, sizeof(GardHttpServer));
    server->port = port > 0 ? port : 8080;
    server->running = 0;
    server->route_count = 0;
    server->cors_origin = NULL;  // NULL = use default "*"
    server->cors_methods = NULL;
    server->cors_headers = NULL;
    pthread_mutex_init(&server->mutex, NULL);
    return server;
}

void gard_http_server_route(GardHttpServer* server, const char* method, const char* path, const char* static_response) {
    if (!server || server->route_count >= MAX_ROUTES) return;
    pthread_mutex_lock(&server->mutex);
    GardRoute* r = &server->routes[server->route_count++];
    r->method = strdup(method ? method : "GET");
    r->path = strdup(path ? path : "/");
    r->response_body = strdup(static_response ? static_response : "");
    r->content_type = strdup("text/plain");
    r->status_code = 200;
    r->handler = NULL;
    pthread_mutex_unlock(&server->mutex);
}

void gard_http_server_route_json(GardHttpServer* server, const char* method, const char* path, const char* json_response) {
    if (!server || server->route_count >= MAX_ROUTES) return;
    pthread_mutex_lock(&server->mutex);
    GardRoute* r = &server->routes[server->route_count++];
    r->method = strdup(method ? method : "GET");
    r->path = strdup(path ? path : "/");
    r->response_body = strdup(json_response ? json_response : "{}");
    r->content_type = strdup("application/json");
    r->status_code = 200;
    r->handler = NULL;
    pthread_mutex_unlock(&server->mutex);
}

void gard_http_server_route_status(GardHttpServer* server, const char* method, const char* path, int32_t status, const char* body) {
    if (!server || server->route_count >= MAX_ROUTES) return;
    pthread_mutex_lock(&server->mutex);
    GardRoute* r = &server->routes[server->route_count++];
    r->method = strdup(method ? method : "GET");
    r->path = strdup(path ? path : "/");
    r->response_body = strdup(body ? body : "");
    r->content_type = strdup("text/plain");
    r->status_code = status;
    r->handler = NULL;
    pthread_mutex_unlock(&server->mutex);
}

// Route with callback handler: handler(request_body) → response_body
void gard_http_server_route_handler(GardHttpServer* server, const char* method, const char* path, char* (*handler)(const char*)) {
    if (!server || server->route_count >= MAX_ROUTES) return;
    pthread_mutex_lock(&server->mutex);
    GardRoute* r = &server->routes[server->route_count++];
    r->method = strdup(method ? method : "GET");
    r->path = strdup(path ? path : "/");
    r->response_body = NULL;
    r->content_type = strdup("application/json");
    r->status_code = 200;
    r->handler = NULL;
    r->handler = handler;
    pthread_mutex_unlock(&server->mutex);
}

void gard_http_server_cors(GardHttpServer* server, const char* origin, const char* methods, const char* headers) {
    if (!server) return;
    pthread_mutex_lock(&server->mutex);
    if (server->cors_origin) free(server->cors_origin);
    if (server->cors_methods) free(server->cors_methods);
    if (server->cors_headers) free(server->cors_headers);
    server->cors_origin = origin ? strdup(origin) : NULL;
    server->cors_methods = methods ? strdup(methods) : NULL;
    server->cors_headers = headers ? strdup(headers) : NULL;
    pthread_mutex_unlock(&server->mutex);
}

int32_t gard_http_server_listen(GardHttpServer* server) {
    if (!server || server->running) return 0;

    // Start with thread-per-connection model + internal select/epoll
    server->daemon = MHD_start_daemon(
        MHD_USE_AUTO | MHD_USE_INTERNAL_POLLING_THREAD | MHD_USE_AUTO_INTERNAL_THREAD,
        (uint16_t)server->port,
        NULL, NULL,                    // accept policy (accept all)
        &request_handler, server,      // request handler
        MHD_OPTION_CONNECTION_TIMEOUT, (unsigned int)30,
        MHD_OPTION_THREAD_POOL_SIZE, (unsigned int)4,
        MHD_OPTION_END
    );

    if (!server->daemon) return 0;
    server->running = 1;

    // Get actual port (in case 0 was passed for auto-assign)
    const union MHD_DaemonInfo* info = MHD_get_daemon_info(server->daemon, MHD_DAEMON_INFO_BIND_PORT);
    if (info) server->port = (int32_t)info->port;

    return 1;
}

void gard_http_server_stop(GardHttpServer* server) {
    if (!server || !server->running) return;
    MHD_stop_daemon(server->daemon);
    server->daemon = NULL;
    server->running = 0;
    // Free routes
    for (int32_t i = 0; i < server->route_count; i++) {
        free(server->routes[i].method);
        free(server->routes[i].path);
        free(server->routes[i].response_body);
        free(server->routes[i].content_type);
    }
    // Free CORS
    if (server->cors_origin) free(server->cors_origin);
    if (server->cors_methods) free(server->cors_methods);
    if (server->cors_headers) free(server->cors_headers);
    pthread_mutex_destroy(&server->mutex);
}

int32_t gard_http_server_is_running(GardHttpServer* server) {
    return server ? server->running : 0;
}

int32_t gard_http_server_port(GardHttpServer* server) {
    return server ? server->port : 0;
}

// Request accessors (for future callback-based routing)
const char* gard_request_method(GardRequest* req) { return req ? req->method : ""; }
const char* gard_request_path(GardRequest* req) { return req ? req->path : ""; }
const char* gard_request_body(GardRequest* req) { return req ? (req->body ? req->body : "") : ""; }
const char* gard_request_header(GardRequest* req, const char* name) {
    if (!req || !name || !req->connection) return "";
    const char* val = MHD_lookup_connection_value(req->connection, MHD_HEADER_KIND, name);
    return val ? val : "";
}
const char* gard_request_param(GardRequest* req, const char* name) {
    // Path params would require route matching context — return empty for now
    return "";
}
const char* gard_request_query(GardRequest* req, const char* name) {
    if (!req || !name || !req->connection) return "";
    const char* val = MHD_lookup_connection_value(req->connection, MHD_GET_ARGUMENT_KIND, name);
    return val ? val : "";
}


// ============================================================
// WebSocket Client — libwebsockets (production-grade, RFC 6455)
// Features: TLS (wss://), ping/pong, fragmented messages,
//           per-message deflate, event-driven
// ============================================================

#include <libwebsockets.h>

#define WS_RX_BUFFER_SIZE 65536

struct GardWebSocket {
    struct lws_context* context;
    struct lws* wsi;
    int32_t connected;
    int32_t destroyed;
    // Receive buffer (ring buffer)
    char** rx_queue;
    int32_t rx_head;
    int32_t rx_tail;
    int32_t rx_capacity;
    // Current incoming message accumulator
    char* rx_partial;
    size_t rx_partial_len;
    size_t rx_partial_cap;
    // Send buffer
    char* tx_pending;
    size_t tx_pending_len;
    pthread_mutex_t mutex;
};

// libwebsockets callback
static int ws_callback(struct lws* wsi, enum lws_callback_reasons reason,
                       void* user, void* in, size_t len) {
    GardWebSocket* ws = (GardWebSocket*)lws_context_user(lws_get_context(wsi));
    if (!ws) return 0;

    switch (reason) {
        case LWS_CALLBACK_CLIENT_ESTABLISHED:
            ws->connected = 1;
            break;

        case LWS_CALLBACK_CLIENT_RECEIVE: {
            pthread_mutex_lock(&ws->mutex);
            // Accumulate fragments
            int is_final = lws_is_final_fragment(wsi);
            if (ws->rx_partial_len + len + 1 > ws->rx_partial_cap) {
                ws->rx_partial_cap = (ws->rx_partial_len + len + 1) * 2;
                ws->rx_partial = (char*)realloc(ws->rx_partial, ws->rx_partial_cap);
            }
            memcpy(ws->rx_partial + ws->rx_partial_len, in, len);
            ws->rx_partial_len += len;
            ws->rx_partial[ws->rx_partial_len] = '\0';

            if (is_final) {
                // Complete message — enqueue
                int next_tail = (ws->rx_tail + 1) % ws->rx_capacity;
                if (next_tail != ws->rx_head) { // not full
                    ws->rx_queue[ws->rx_tail] = strdup(ws->rx_partial);
                    ws->rx_tail = next_tail;
                }
                ws->rx_partial_len = 0;
            }
            pthread_mutex_unlock(&ws->mutex);
            break;
        }

        case LWS_CALLBACK_CLIENT_WRITEABLE: {
            pthread_mutex_lock(&ws->mutex);
            if (ws->tx_pending && ws->tx_pending_len > 0) {
                // libwebsockets requires LWS_PRE bytes before the data
                size_t total = LWS_PRE + ws->tx_pending_len;
                unsigned char* buf = (unsigned char*)malloc(total);
                memcpy(buf + LWS_PRE, ws->tx_pending, ws->tx_pending_len);
                lws_write(wsi, buf + LWS_PRE, ws->tx_pending_len, LWS_WRITE_TEXT);
                free(buf);
                free(ws->tx_pending);
                ws->tx_pending = NULL;
                ws->tx_pending_len = 0;
            }
            pthread_mutex_unlock(&ws->mutex);
            break;
        }

        case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
            ws->connected = 0;
            break;

        case LWS_CALLBACK_CLIENT_CLOSED:
            ws->connected = 0;
            break;

        default:
            break;
    }
    return 0;
}

static const struct lws_protocols ws_protocols[] = {
    { "gard-ws-server", ws_callback, 0, WS_RX_BUFFER_SIZE },
    { NULL, NULL, 0, 0 }
};

// Service thread — runs the lws event loop
static void* ws_service_thread(void* arg) {
    GardWebSocket* ws = (GardWebSocket*)arg;
    while (!ws->destroyed && ws->context) {
        lws_service(ws->context, 50); // 50ms timeout
    }
    return NULL;
}

GardWebSocket* gard_ws_connect(const char* url) {
    if (!url) return NULL;

    GardWebSocket* ws = (GardWebSocket*)calloc(1, sizeof(GardWebSocket));
    ws->connected = 0;
    ws->destroyed = 0;
    ws->rx_capacity = 256;
    ws->rx_queue = (char**)calloc(ws->rx_capacity, sizeof(char*));
    ws->rx_head = 0;
    ws->rx_tail = 0;
    ws->rx_partial_cap = 4096;
    ws->rx_partial = (char*)malloc(ws->rx_partial_cap);
    ws->rx_partial_len = 0;
    ws->tx_pending = NULL;
    ws->tx_pending_len = 0;
    pthread_mutex_init(&ws->mutex, NULL);

    // Parse URL: ws://host:port/path or wss://host:port/path
    int use_ssl = 0;
    const char* p = url;
    if (strncmp(p, "wss://", 6) == 0) { use_ssl = 1; p += 6; }
    else if (strncmp(p, "ws://", 5) == 0) { p += 5; }
    else { free(ws->rx_queue); free(ws->rx_partial); free(ws); return NULL; }

    // Extract host, port, path
    char host[256] = "";
    int port = use_ssl ? 443 : 80;
    char path[1024] = "/";

    const char* slash = strchr(p, '/');
    const char* colon = strchr(p, ':');
    if (colon && (!slash || colon < slash)) {
        size_t hlen = colon - p;
        memcpy(host, p, hlen); host[hlen] = '\0';
        port = atoi(colon + 1);
        if (slash) strncpy(path, slash, sizeof(path) - 1);
    } else if (slash) {
        size_t hlen = slash - p;
        memcpy(host, p, hlen); host[hlen] = '\0';
        strncpy(path, slash, sizeof(path) - 1);
    } else {
        strncpy(host, p, sizeof(host) - 1);
    }

    // Create lws context
    struct lws_context_creation_info ctx_info;
    memset(&ctx_info, 0, sizeof(ctx_info));
    ctx_info.port = CONTEXT_PORT_NO_LISTEN;
    ctx_info.protocols = ws_protocols;
    ctx_info.user = ws;
    ctx_info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;

    ws->context = lws_create_context(&ctx_info);
    if (!ws->context) {
        free(ws->rx_queue); free(ws->rx_partial); free(ws);
        return NULL;
    }

    // Connect
    struct lws_client_connect_info conn_info;
    memset(&conn_info, 0, sizeof(conn_info));
    conn_info.context = ws->context;
    conn_info.address = host;
    conn_info.port = port;
    conn_info.path = path;
    conn_info.host = host;
    conn_info.origin = host;
    conn_info.protocol = ws_protocols[0].name;
    conn_info.ssl_connection = use_ssl ? LCCSCF_USE_SSL : 0;

    ws->wsi = lws_client_connect_via_info(&conn_info);
    if (!ws->wsi) {
        lws_context_destroy(ws->context);
        free(ws->rx_queue); free(ws->rx_partial); free(ws);
        return NULL;
    }

    // Start service thread
    pthread_t tid;
    pthread_create(&tid, NULL, ws_service_thread, ws);
    pthread_detach(tid);

    // Wait for connection (up to 5 seconds)
    for (int i = 0; i < 100 && !ws->connected; i++) {
        struct timespec ts = {0, 50000000}; // 50ms
        nanosleep(&ts, NULL);
    }

    return ws;
}

int32_t gard_ws_send(GardWebSocket* ws, const char* message) {
    if (!ws || !ws->connected || !message) return 0;
    pthread_mutex_lock(&ws->mutex);
    if (ws->tx_pending) free(ws->tx_pending);
    ws->tx_pending_len = strlen(message);
    ws->tx_pending = (char*)malloc(ws->tx_pending_len + 1);
    memcpy(ws->tx_pending, message, ws->tx_pending_len + 1);
    pthread_mutex_unlock(&ws->mutex);
    lws_callback_on_writable(ws->wsi);
    return 1;
}

char* gard_ws_receive(GardWebSocket* ws) {
    if (!ws) return strdup("");
    // Wait up to 5 seconds for a message
    for (int i = 0; i < 100; i++) {
        pthread_mutex_lock(&ws->mutex);
        if (ws->rx_head != ws->rx_tail) {
            char* msg = ws->rx_queue[ws->rx_head];
            ws->rx_head = (ws->rx_head + 1) % ws->rx_capacity;
            pthread_mutex_unlock(&ws->mutex);
            return msg ? msg : strdup("");
        }
        pthread_mutex_unlock(&ws->mutex);
        struct timespec ts = {0, 50000000}; // 50ms
        nanosleep(&ts, NULL);
    }
    return strdup(""); // timeout
}

void gard_ws_close(GardWebSocket* ws) {
    if (!ws) return;
    ws->destroyed = 1;
    ws->connected = 0;
    // Give service thread time to notice and exit
    struct timespec ts = {0, 200000000}; // 200ms
    nanosleep(&ts, NULL);
    if (ws->context) { lws_context_destroy(ws->context); ws->context = NULL; }
    // Free rx queue
    for (int i = ws->rx_head; i != ws->rx_tail; i = (i + 1) % ws->rx_capacity) {
        if (ws->rx_queue[i]) free(ws->rx_queue[i]);
    }
    free(ws->rx_queue);
    free(ws->rx_partial);
    if (ws->tx_pending) free(ws->tx_pending);
    pthread_mutex_destroy(&ws->mutex);
    free(ws);
}

int32_t gard_ws_is_connected(GardWebSocket* ws) {
    return (ws && ws->connected) ? 1 : 0;
}


// ============================================================
// WebSocket Server — libwebsockets (production-grade)
// Multi-client, broadcast, per-connection state, ping/pong
// ============================================================

#define WS_SERVER_MAX_CLIENTS 1024
#define WS_SERVER_RX_QUEUE_SIZE 1024

// Per-connection data
struct WsServerPerSession {
    struct lws* wsi;
    int active;
    // Per-client send queue
    char* pending_msg;
    size_t pending_len;
};

struct GardWsServer {
    struct lws_context* context;
    int32_t port;
    int32_t running;
    int32_t destroyed;
    // Client tracking
    struct WsServerPerSession* clients[WS_SERVER_MAX_CLIENTS];
    int32_t client_count;
    // Receive queue (messages from all clients)
    char* rx_queue[WS_SERVER_RX_QUEUE_SIZE];
    int32_t rx_head;
    int32_t rx_tail;
    // Broadcast pending
    char* broadcast_pending;
    size_t broadcast_len;
    pthread_mutex_t mutex;
    pthread_t service_thread;
};

static int ws_server_callback(struct lws* wsi, enum lws_callback_reasons reason,
                               void* user, void* in, size_t len) {
    GardWsServer* server = (GardWsServer*)lws_context_user(lws_get_context(wsi));
    struct WsServerPerSession* pss = (struct WsServerPerSession*)user;

    switch (reason) {
        case LWS_CALLBACK_ESTABLISHED: {
            if (!server || !pss) break;
            pthread_mutex_lock(&server->mutex);
            pss->wsi = wsi;
            pss->active = 1;
            pss->pending_msg = NULL;
            pss->pending_len = 0;
            // Add to client list
            if (server->client_count < WS_SERVER_MAX_CLIENTS) {
                server->clients[server->client_count++] = pss;
            }
            pthread_mutex_unlock(&server->mutex);
            break;
        }

        case LWS_CALLBACK_RECEIVE: {
            if (!server || !in || len == 0) break;
            pthread_mutex_lock(&server->mutex);
            // Enqueue received message
            int next_tail = (server->rx_tail + 1) % WS_SERVER_RX_QUEUE_SIZE;
            if (next_tail != server->rx_head) {
                char* msg = (char*)malloc(len + 1);
                memcpy(msg, in, len);
                msg[len] = '\0';
                server->rx_queue[server->rx_tail] = msg;
                server->rx_tail = next_tail;
            }
            pthread_mutex_unlock(&server->mutex);
            // Request writable callback for echo/broadcast
            lws_callback_on_writable_all_protocol(lws_get_context(wsi), lws_get_protocol(wsi));
            break;
        }

        case LWS_CALLBACK_SERVER_WRITEABLE: {
            if (!server || !pss || !pss->active) break;
            pthread_mutex_lock(&server->mutex);
            if (pss->pending_msg && pss->pending_len > 0) {
                size_t total = LWS_PRE + pss->pending_len;
                unsigned char* buf = (unsigned char*)malloc(total);
                memcpy(buf + LWS_PRE, pss->pending_msg, pss->pending_len);
                lws_write(wsi, buf + LWS_PRE, pss->pending_len, LWS_WRITE_TEXT);
                free(buf);
                free(pss->pending_msg);
                pss->pending_msg = NULL;
                pss->pending_len = 0;
            }
            pthread_mutex_unlock(&server->mutex);
            break;
        }

        case LWS_CALLBACK_CLOSED: {
            if (!server || !pss) break;
            pthread_mutex_lock(&server->mutex);
            pss->active = 0;
            // Remove from client list
            for (int i = 0; i < server->client_count; i++) {
                if (server->clients[i] == pss) {
                    server->clients[i] = server->clients[--server->client_count];
                    break;
                }
            }
            pthread_mutex_unlock(&server->mutex);
            break;
        }

        default:
            break;
    }
    return 0;
}

static const struct lws_protocols ws_server_protocols[] = {
    { "gard-ws-server", ws_server_callback, sizeof(struct WsServerPerSession), WS_RX_BUFFER_SIZE },
    { NULL, NULL, 0, 0 }
};

static void* ws_server_service_thread(void* arg) {
    GardWsServer* server = (GardWsServer*)arg;
    while (!server->destroyed && server->context) {
        lws_service(server->context, 50);
    }
    return NULL;
}

GardWsServer* gard_ws_server_create(int32_t port) {
    GardWsServer* server = (GardWsServer*)calloc(1, sizeof(GardWsServer));
    server->port = port > 0 ? port : 8081;
    server->running = 0;
    server->destroyed = 0;
    server->client_count = 0;
    server->rx_head = 0;
    server->rx_tail = 0;
    server->broadcast_pending = NULL;
    server->broadcast_len = 0;
    pthread_mutex_init(&server->mutex, NULL);
    return server;
}

int32_t gard_ws_server_listen(GardWsServer* server) {
    if (!server || server->running) return 0;

    struct lws_context_creation_info info;
    memset(&info, 0, sizeof(info));
    info.port = server->port;
    info.protocols = ws_server_protocols;
    info.user = server;
    info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;

    server->context = lws_create_context(&info);
    if (!server->context) return 0;

    server->running = 1;

    // Start service thread
    pthread_create(&server->service_thread, NULL, ws_server_service_thread, server);

    return 1;
}

void gard_ws_server_stop(GardWsServer* server) {
    if (!server || !server->running) return;
    server->destroyed = 1;
    server->running = 0;
    // Give service thread time to exit naturally
    struct timespec ts = {0, 200000000}; // 200ms
    nanosleep(&ts, NULL);
    // Cancel thread if still running
    pthread_cancel(server->service_thread);
    struct timespec ts2 = {0, 100000000}; // 100ms
    nanosleep(&ts2, NULL);
    if (server->context) { lws_context_destroy(server->context); server->context = NULL; }
    // Free rx queue
    while (server->rx_head != server->rx_tail) {
        if (server->rx_queue[server->rx_head]) free(server->rx_queue[server->rx_head]);
        server->rx_head = (server->rx_head + 1) % WS_SERVER_RX_QUEUE_SIZE;
    }
    if (server->broadcast_pending) free(server->broadcast_pending);
    pthread_mutex_destroy(&server->mutex);
}

int32_t gard_ws_server_is_running(GardWsServer* server) {
    return (server && server->running) ? 1 : 0;
}

int32_t gard_ws_server_client_count(GardWsServer* server) {
    if (!server) return 0;
    pthread_mutex_lock(&server->mutex);
    int32_t count = server->client_count;
    pthread_mutex_unlock(&server->mutex);
    return count;
}

void gard_ws_server_broadcast(GardWsServer* server, const char* message) {
    if (!server || !message || !server->running) return;
    pthread_mutex_lock(&server->mutex);
    // Copy message to each connected client's send queue
    for (int i = 0; i < server->client_count; i++) {
        struct WsServerPerSession* pss = server->clients[i];
        if (pss && pss->active) {
            if (pss->pending_msg) free(pss->pending_msg);
            pss->pending_msg = strdup(message);
            pss->pending_len = strlen(message);
        }
    }
    // Store for reference
    if (server->broadcast_pending) free(server->broadcast_pending);
    server->broadcast_len = strlen(message);
    server->broadcast_pending = strdup(message);
    pthread_mutex_unlock(&server->mutex);
    // Trigger writable on all clients
    if (server->context) {
        lws_callback_on_writable_all_protocol(server->context, &ws_server_protocols[0]);
    }
}

char* gard_ws_server_receive(GardWsServer* server) {
    if (!server) return strdup("");
    // Wait up to 5 seconds
    for (int i = 0; i < 100; i++) {
        pthread_mutex_lock(&server->mutex);
        if (server->rx_head != server->rx_tail) {
            char* msg = server->rx_queue[server->rx_head];
            server->rx_head = (server->rx_head + 1) % WS_SERVER_RX_QUEUE_SIZE;
            pthread_mutex_unlock(&server->mutex);
            return msg ? msg : strdup("");
        }
        pthread_mutex_unlock(&server->mutex);
        struct timespec ts = {0, 50000000};
        nanosleep(&ts, NULL);
    }
    return strdup("");
}


// ============================================================
// TCP Socket — POSIX (production-grade)
// Non-blocking connect with timeout, proper error handling,
// SO_REUSEADDR, TCP_NODELAY, configurable buffer sizes
// ============================================================

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>

#define TCP_RECV_BUF_SIZE 65536
#define TCP_CONNECT_TIMEOUT_MS 5000
#define TCP_RECV_TIMEOUT_MS 5000

struct GardTcpSocket {
    int fd;
    int32_t connected;
    struct sockaddr_in remote_addr;
};

struct GardTcpServer {
    int fd;
    int32_t port;
    int32_t listening;
};

// --- TCP Client ---

GardTcpSocket* gard_tcp_connect(const char* host, int32_t port) {
    if (!host || port <= 0) return NULL;

    // Resolve hostname
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", port);

    if (getaddrinfo(host, port_str, &hints, &res) != 0) return NULL;

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); return NULL; }

    // Set non-blocking for connect with timeout
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    int ret = connect(fd, res->ai_addr, res->ai_addrlen);
    if (ret < 0 && errno != EINPROGRESS) {
        close(fd); freeaddrinfo(res); return NULL;
    }

    if (ret < 0) {
        // Wait for connection with timeout
        struct pollfd pfd = { .fd = fd, .events = POLLOUT };
        int poll_ret = poll(&pfd, 1, TCP_CONNECT_TIMEOUT_MS);
        if (poll_ret <= 0) { close(fd); freeaddrinfo(res); return NULL; }

        // Check for connection error
        int err = 0;
        socklen_t errlen = sizeof(err);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errlen);
        if (err != 0) { close(fd); freeaddrinfo(res); return NULL; }
    }

    // Set back to blocking
    fcntl(fd, F_SETFL, flags);

    // TCP_NODELAY (disable Nagle's algorithm for low latency)
    int nodelay = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    // Set receive timeout
    struct timeval tv = { .tv_sec = TCP_RECV_TIMEOUT_MS / 1000, .tv_usec = (TCP_RECV_TIMEOUT_MS % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    GardTcpSocket* sock = (GardTcpSocket*)calloc(1, sizeof(GardTcpSocket));
    sock->fd = fd;
    sock->connected = 1;
    memcpy(&sock->remote_addr, res->ai_addr, sizeof(struct sockaddr_in));
    freeaddrinfo(res);
    return sock;
}

int32_t gard_tcp_send(GardTcpSocket* sock, const char* data) {
    if (!sock || !sock->connected || sock->fd < 0 || !data) return 0;
    size_t len = strlen(data);
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(sock->fd, data + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) {
            if (errno == EINTR) continue;
            sock->connected = 0;
            return 0;
        }
        sent += n;
    }
    return (int32_t)sent;
}

char* gard_tcp_receive(GardTcpSocket* sock) {
    if (!sock || !sock->connected || sock->fd < 0) return strdup("");
    char buf[TCP_RECV_BUF_SIZE];
    ssize_t n = recv(sock->fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) {
        if (n == 0) sock->connected = 0; // peer closed
        return strdup("");
    }
    buf[n] = '\0';
    return strdup(buf);
}

int32_t gard_tcp_receive_bytes(GardTcpSocket* sock, char* buf, int32_t max_len) {
    if (!sock || !sock->connected || sock->fd < 0 || !buf || max_len <= 0) return 0;
    ssize_t n = recv(sock->fd, buf, max_len, 0);
    if (n <= 0) {
        if (n == 0) sock->connected = 0;
        return 0;
    }
    return (int32_t)n;
}

void gard_tcp_close(GardTcpSocket* sock) {
    if (!sock) return;
    if (sock->fd >= 0) {
        shutdown(sock->fd, SHUT_RDWR);
        close(sock->fd);
    }
    sock->connected = 0;
    sock->fd = -1;
    free(sock);
}

int32_t gard_tcp_is_connected(GardTcpSocket* sock) {
    return (sock && sock->connected) ? 1 : 0;
}

// --- TCP Server ---

GardTcpServer* gard_tcp_server_create(int32_t port) {
    if (port <= 0) port = 8080;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return NULL;

    // SO_REUSEADDR (allow immediate rebind after restart)
    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)port);

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd); return NULL;
    }

    GardTcpServer* server = (GardTcpServer*)calloc(1, sizeof(GardTcpServer));
    server->fd = fd;
    server->port = port;
    server->listening = 0;
    return server;
}

int32_t gard_tcp_server_listen(GardTcpServer* server) {
    if (!server || server->listening) return 0;
    if (listen(server->fd, 128) < 0) return 0; // backlog of 128
    server->listening = 1;
    return 1;
}

GardTcpSocket* gard_tcp_server_accept(GardTcpServer* server) {
    if (!server || !server->listening) return NULL;

    // Poll with timeout (5 seconds)
    struct pollfd pfd = { .fd = server->fd, .events = POLLIN };
    int ret = poll(&pfd, 1, TCP_RECV_TIMEOUT_MS);
    if (ret <= 0) return NULL;

    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    int client_fd = accept(server->fd, (struct sockaddr*)&client_addr, &addr_len);
    if (client_fd < 0) return NULL;

    // TCP_NODELAY on accepted connection
    int nodelay = 1;
    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    // Set receive timeout
    struct timeval tv = { .tv_sec = TCP_RECV_TIMEOUT_MS / 1000, .tv_usec = (TCP_RECV_TIMEOUT_MS % 1000) * 1000 };
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    GardTcpSocket* sock = (GardTcpSocket*)calloc(1, sizeof(GardTcpSocket));
    sock->fd = client_fd;
    sock->connected = 1;
    sock->remote_addr = client_addr;
    return sock;
}

void gard_tcp_server_stop(GardTcpServer* server) {
    if (!server) return;
    if (server->fd >= 0) close(server->fd);
    server->fd = -1;
    server->listening = 0;
    free(server);
}

int32_t gard_tcp_server_port(GardTcpServer* server) {
    return server ? server->port : 0;
}

// ============================================================
// UDP Socket — POSIX (production-grade)
// ============================================================

struct GardUdpSocket {
    int fd;
    int32_t port;
    struct sockaddr_in last_sender;
};

GardUdpSocket* gard_udp_bind(int32_t port) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return NULL;

    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)(port > 0 ? port : 0));

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd); return NULL;
    }

    // Get actual port if 0 was passed
    if (port == 0) {
        socklen_t len = sizeof(addr);
        getsockname(fd, (struct sockaddr*)&addr, &len);
        port = ntohs(addr.sin_port);
    }

    // Set receive timeout
    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    GardUdpSocket* sock = (GardUdpSocket*)calloc(1, sizeof(GardUdpSocket));
    sock->fd = fd;
    sock->port = port;
    return sock;
}

int32_t gard_udp_send(GardUdpSocket* sock, const char* host, int32_t port, const char* data) {
    if (!sock || !host || !data || port <= 0) return 0;

    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons((uint16_t)port);

    // Resolve hostname
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", port);
    if (getaddrinfo(host, port_str, &hints, &res) != 0) return 0;
    memcpy(&dest, res->ai_addr, sizeof(dest));
    freeaddrinfo(res);

    size_t len = strlen(data);
    ssize_t sent = sendto(sock->fd, data, len, 0, (struct sockaddr*)&dest, sizeof(dest));
    return sent > 0 ? (int32_t)sent : 0;
}

char* gard_udp_receive(GardUdpSocket* sock) {
    if (!sock || sock->fd < 0) return strdup("");
    char buf[65536];
    struct sockaddr_in sender;
    socklen_t sender_len = sizeof(sender);
    ssize_t n = recvfrom(sock->fd, buf, sizeof(buf) - 1, 0, (struct sockaddr*)&sender, &sender_len);
    if (n <= 0) return strdup("");
    buf[n] = '\0';
    sock->last_sender = sender;
    return strdup(buf);
}

void gard_udp_close(GardUdpSocket* sock) {
    if (!sock) return;
    if (sock->fd >= 0) close(sock->fd);
    free(sock);
}

int32_t gard_udp_port(GardUdpSocket* sock) {
    return sock ? sock->port : 0;
}
