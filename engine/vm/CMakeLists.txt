cmake_minimum_required(VERSION 3.16)
project(gardvm LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(gardvm
    main.cpp
    interpreter.cpp
    dispatch.cpp
)

target_include_directories(gardvm PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(gardvm PRIVATE pthread)
target_compile_options(gardvm PRIVATE -O2 -Wall)
