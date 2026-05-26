cmake_minimum_required(VERSION 3.20)
project(gard VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Source files
file(GLOB_RECURSE SOURCES "src/*.cpp")
file(GLOB_RECURSE HEADERS "include/*.h")

# Main compiler executable
add_executable(gard ${SOURCES})
target_include_directories(gard PRIVATE include /usr/include/libmongoc-1.0 /usr/include/libbson-1.0 /usr/include/postgresql /usr/lib/llvm-20/include)

# Warnings
target_compile_options(gard PRIVATE
    -Wall -Wextra -Wno-unused-parameter -Wno-pedantic -O2
    -D_GNU_SOURCE -D__STDC_CONSTANT_MACROS -D__STDC_FORMAT_MACROS -D__STDC_LIMIT_MACROS
)

# Link OpenSSL for crypto stdlib
find_package(OpenSSL REQUIRED)
find_package(LibXml2 REQUIRED)
find_package(CURL REQUIRED)
target_link_libraries(gard PRIVATE OpenSSL::SSL OpenSSL::Crypto z LibXml2::LibXml2 CURL::libcurl pthread sqlite3 mysqlclient pq mongoc-1.0 bson-1.0 dl ffi LLVM-20 SDL2 SDL2_image SDL2_ttf SDL2_mixer)

# LSP Server (frontend-only, no LLVM/codegen/runtime dependencies)
file(GLOB LSP_FRONTEND_SOURCES
    "src/lexer/*.cpp"
    "src/parser/*.cpp"
    "src/sema/*.cpp"
    "src/types/*.cpp"
    "src/tools/*.cpp"
)
add_executable(gard-lsp lsp/gard_lsp.cpp ${LSP_FRONTEND_SOURCES})
target_include_directories(gard-lsp PRIVATE include)
target_compile_options(gard-lsp PRIVATE -Wall -Wextra -Wno-unused-parameter -O2)

# Test: async runtime
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_async.cpp")
add_executable(test_async tests/test_async.cpp
    src/runtime/async.cpp
    src/runtime/runtime.cpp
    src/bytecode/bytecode.cpp
)
target_include_directories(test_async PRIVATE include)
target_compile_options(test_async PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

# Test: event loop
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_event_loop.cpp")
add_executable(test_event_loop tests/test_event_loop.cpp
    src/runtime/event_loop.cpp
)
target_include_directories(test_event_loop PRIVATE include)
target_compile_options(test_event_loop PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(test_event_loop PRIVATE pthread)
endif()

# Test: VM extensions
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_vm_ext.cpp")
add_executable(test_vm_ext tests/test_vm_ext.cpp
    src/runtime/vm_extensions.cpp
    src/runtime/runtime.cpp
    src/bytecode/bytecode.cpp
)
target_include_directories(test_vm_ext PRIVATE include)
target_compile_options(test_vm_ext PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(test_vm_ext PRIVATE pthread)
endif()

# Test: JIT engine
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_jit.cpp")
file(GLOB JIT_TEST_SOURCES
    "tests/test_jit.cpp"
    "src/jit/*.cpp"
    "src/lexer/*.cpp"
    "src/parser/*.cpp"
    "src/ir/*.cpp"
    "src/bytecode/*.cpp"
    "src/runtime/runtime.cpp"
    "src/runtime/stdlib.cpp"
    "src/sema/*.cpp"
    "src/types/*.cpp"
)
add_executable(test_jit ${JIT_TEST_SOURCES})
target_include_directories(test_jit PRIVATE include /usr/lib/llvm-20/include /usr/include/libmongoc-1.0 /usr/include/libbson-1.0 /usr/include/postgresql)
target_compile_options(test_jit PRIVATE -O2 -Wno-unused-parameter -Wno-pedantic -D_GNU_SOURCE -D__STDC_CONSTANT_MACROS -D__STDC_FORMAT_MACROS -D__STDC_LIMIT_MACROS)
target_link_libraries(test_jit PRIVATE LLVM-20 OpenSSL::SSL OpenSSL::Crypto z LibXml2::LibXml2 CURL::libcurl pthread sqlite3 mysqlclient pq mongoc-1.0 bson-1.0 dl ffi)
endif()
