cmake_minimum_required(VERSION 3.20)
project(FuckPlaygroundGames LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(amd_ags_x64 SHARED
    src/proxy_main.cpp
    src/proxy_log.cpp
    src/proxy_vram.cpp
    src/proxy_guard.cpp
    src/proxy_particles.cpp
    src/proxy_sched.cpp
)

target_include_directories(amd_ags_x64 PRIVATE src)

target_link_libraries(amd_ags_x64 PRIVATE
    dxgi
    user32
)

set_target_properties(amd_ags_x64 PROPERTIES
    PREFIX ""
    OUTPUT_NAME "amd_ags_x64"
)
