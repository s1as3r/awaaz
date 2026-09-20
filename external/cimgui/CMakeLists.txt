cmake_minimum_required(VERSION 3.15)
project(cimgui_wrapper C CXX)

set(IMGUI_STATIC "yes" CACHE STRING "Build as a static library" FORCE)

add_subdirectory(cimgui)

target_sources(cimgui PRIVATE
    cimgui/imgui/backends/imgui_impl_sdl3.cpp
    cimgui/imgui/backends/imgui_impl_sdlrenderer3.cpp
)

target_include_directories(cimgui PRIVATE
    ../SDL/include
)
