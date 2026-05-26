#pragma once

#include "runtime/runtime.h"

namespace gard {
namespace runtime {
namespace stdlib {

// Graphics module — only loaded when `import gard/graphics` is present
// Provides: Window, Graphics2D, Graphics3D, Image, Shader, Font, Input, Audio, Color
// Dependencies: SDL2, OpenGL 4.5+, GLEW, SDL2_image, SDL2_ttf, SDL2_mixer

void registerGraphicsModule(VM& vm);

} // namespace stdlib
} // namespace runtime
} // namespace gard
