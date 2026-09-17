cmake_minimum_required(VERSION 3.22)
project(luoli_engine CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_compile_options(-O2 -fvisibility=hidden -fdata-sections -ffunction-sections)
add_link_options(-Wl,--gc-sections)

add_executable(engine
    daemon/main.cpp
    daemon/server.cpp
    daemon/scanner.cpp
    daemon/freezer.cpp
    daemon/guard.cpp
    daemon/arm64_disasm.cpp
    daemon/rules.cpp
    daemon/il2cpp.cpp
    daemon/storage.cpp
    daemon/invoker.cpp
    daemon/crash.cpp
)

target_include_directories(engine PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)

# STL 通过 gradle 传入 -DANDROID_STL=c++_static，daemon 自包含运行

# 打包为 jniLibs: lib/arm64-v8a/libengine.so
set_target_properties(engine PROPERTIES
    OUTPUT_NAME "engine"
    PREFIX "lib"
    SUFFIX ".so"
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}
)
