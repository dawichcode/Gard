#pragma once

#include "runtime/runtime.h"

namespace gard {
namespace runtime {
namespace stdlib {

// Native/FFI module — only loaded when `import gard/native` is present
// Provides: FFI (dlopen, dlsym, native calls), Memory (pointer ops), Unsafe block support

void registerNativeModule(VM& vm);

} // namespace stdlib
} // namespace runtime
} // namespace gard
