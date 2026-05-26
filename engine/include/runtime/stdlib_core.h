#pragma once

#include "runtime/runtime.h"

namespace gard {
namespace runtime {
namespace stdlib {

// Core module — loaded when `import { ... } from "gard/core"` is present
// Provides: Database, db

void registerCoreModule(VM& vm);

} // namespace stdlib
} // namespace runtime
} // namespace gard
