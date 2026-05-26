#pragma once

#include "runtime/runtime.h"
#include <string>
#include <vector>

namespace gard {
namespace runtime {
namespace stdlib {

// Register all standard library native functions into the VM
void registerAll(VM& vm);

// Mark I/O-bound natives as always-async (fire-and-forget without await)
void registerAsyncNatives(VM& vm);

void registerStringFunctions(VM& vm);
void registerMathFunctions(VM& vm);
void registerDateTimeFunctions(VM& vm);
void registerJsonFunctions(VM& vm);
void registerFileFunctions(VM& vm);
void registerCryptoFunctions(VM& vm);
void registerProcessFunctions(VM& vm);
void registerCollectionFunctions(VM& vm);
void registerRegexFunctions(VM& vm);
void registerCompressionFunctions(VM& vm);
void registerXmlFunctions(VM& vm);
void registerReflectFunctions(VM& vm);
void registerConcurrencyFunctions(VM& vm);
void registerPerformanceFunctions(VM& vm);

} // namespace stdlib
} // namespace runtime
} // namespace gard
