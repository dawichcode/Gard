#pragma once

#include "runtime/runtime.h"

namespace gard {
namespace runtime {
namespace stdlib {

// Network module — only loaded when `import gard/network` is present
// Provides: TcpServer, TcpClient, UdpSocket, HttpClient, HttpServer, WebSocket

void registerNetworkModule(VM& vm);

} // namespace stdlib
} // namespace runtime
} // namespace gard
