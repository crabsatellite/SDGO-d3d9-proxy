cmake_minimum_required(VERSION 3.20)
project(d3d9_proxy LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 32-bit build (SDGO is x86)
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "This project must be built as 32-bit (x86). Use -A Win32 with Visual Studio generator.")
endif()

add_library(d3d9 SHARED
    src/dllmain.cpp
    src/d3d9_proxy.cpp
    src/d3d9_device.cpp
    src/post_process.cpp
    src/texture_manager.cpp
    src/mod_manager.cpp
    src/hook.cpp
    src/config.cpp
    src/d3d9.def
)

target_include_directories(d3d9 PRIVATE src)
target_link_libraries(d3d9 PRIVATE d3dcompiler)

# Output directly to client directory for easy testing
set_target_properties(d3d9 PROPERTIES
    OUTPUT_NAME "d3d9"
    SUFFIX ".dll"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_SOURCE_DIR}/../../SDGO-client"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}/../../SDGO-client"
)
