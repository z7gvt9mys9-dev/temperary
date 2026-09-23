cmake_minimum_required(VERSION 3.20)

project(cs2_internal_esp LANGUAGES C CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Build this DLL for x64")
endif()

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/minhook-1.3.4 third_party/minhook-build EXCLUDE_FROM_ALL)

set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui-1.92.9b")

add_library(cs2_offset_esp SHARED
    dllmain.cpp
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_dx11.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_win32.cpp"
)
target_compile_features(cs2_offset_esp PRIVATE cxx_std_17)
target_compile_definitions(cs2_offset_esp PRIVATE UNICODE _UNICODE)
target_include_directories(cs2_offset_esp PRIVATE
    "${IMGUI_DIR}"
    "${IMGUI_DIR}/backends"
)
target_link_libraries(cs2_offset_esp PRIVATE
    minhook
    user32
    d3d11
    dxgi
    d3dcompiler
    imm32
)

if(MSVC)
    target_compile_options(cs2_offset_esp PRIVATE /W4 /permissive- /utf-8)
endif()

set_target_properties(cs2_offset_esp PROPERTIES
    PREFIX ""
    OUTPUT_NAME "cs2_velocity_gui"
)
