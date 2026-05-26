#pragma once

#include "runtime/runtime.h"

namespace gard {
namespace runtime {
namespace stdlib {

// Web module — only loaded when `import gard/web` is present
// Provides: Wasm (WebAssembly code generation, compilation, and execution)

void registerWebModule(VM& vm);

} // namespace stdlib
} // namespace runtime
} // namespace gard
