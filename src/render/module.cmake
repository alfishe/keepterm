# kt_render: the renderer interface, the draw list, the software backend and the GPU backends
#. GPU APIs are loaded at run time: Metal is weak-linked, d3d11/dxgi and the
# Vulkan loader are opened with LoadLibrary/dlopen.

find_package(Threads REQUIRED)

kt_module(kt_render
    ${CMAKE_CURRENT_LIST_DIR}/backend_factory.cpp
    ${CMAKE_CURRENT_LIST_DIR}/damage.cpp
    ${CMAKE_CURRENT_LIST_DIR}/gpu/gpu_common.cpp
    ${CMAKE_CURRENT_LIST_DIR}/draw_list.cpp
    ${CMAKE_CURRENT_LIST_DIR}/probe_scene.cpp
    ${CMAKE_CURRENT_LIST_DIR}/render_select.cpp
    ${CMAKE_CURRENT_LIST_DIR}/text_gamma.cpp
    ${CMAKE_CURRENT_LIST_DIR}/software/kernels.cpp
    ${CMAKE_CURRENT_LIST_DIR}/software/kernels_neon.cpp
    ${CMAKE_CURRENT_LIST_DIR}/software/kernels_sse2.cpp
    ${CMAKE_CURRENT_LIST_DIR}/software/presenter.cpp
    ${CMAKE_CURRENT_LIST_DIR}/software/software_backend.cpp)
target_link_libraries(kt_render PUBLIC kt_base PRIVATE Threads::Threads)
include(${CMAKE_CURRENT_LIST_DIR}/shaders/shaders.cmake)

# Window presenters of the software backend. They take a platform
# NativeSurface (header-only, src/platform_gui/native_surface.h) and do not link kt_platform_gui.
if(WIN32)
    target_sources(kt_render PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/software/window_presenter_win.cpp
        ${CMAKE_CURRENT_LIST_DIR}/backend_factory_win.cpp
        ${CMAKE_CURRENT_LIST_DIR}/d3d11/d3d11_backend_win.cpp
        ${CMAKE_CURRENT_LIST_DIR}/vulkan/vulkan_backend.cpp)
    target_include_directories(kt_render SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/third_party/vulkan-headers/include)
    target_link_libraries(kt_render PRIVATE gdi32 user32)
elseif(APPLE)
    target_sources(kt_render PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/software/window_presenter_macos.mm
        ${CMAKE_CURRENT_LIST_DIR}/backend_factory_macos.cpp
        ${CMAKE_CURRENT_LIST_DIR}/metal/metal_backend_macos.mm)
    set_source_files_properties(
        ${CMAKE_CURRENT_LIST_DIR}/software/window_presenter_macos.mm
        ${CMAKE_CURRENT_LIST_DIR}/metal/metal_backend_macos.mm
        PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    # Metal is weak: on a Mac without it the binary still starts and the software backend runs.
    target_link_libraries(kt_render PRIVATE "-framework Foundation" "-framework QuartzCore" "-framework IOSurface" "-framework CoreGraphics"
        "-weak_framework Metal")
else()
    # libX11 and libXext are dlopen'ed: only their vendored headers are used here.
    target_sources(kt_render PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/software/window_presenter_linux.cpp
        ${CMAKE_CURRENT_LIST_DIR}/software/window_presenter_x11_linux.cpp
        ${CMAKE_CURRENT_LIST_DIR}/backend_factory_linux.cpp
        ${CMAKE_CURRENT_LIST_DIR}/vulkan/vulkan_backend.cpp)
    target_include_directories(kt_render SYSTEM PRIVATE
        ${PROJECT_SOURCE_DIR}/third_party/vulkan-headers/include
        ${PROJECT_SOURCE_DIR}/third_party/libx11-headers/include
        ${PROJECT_SOURCE_DIR}/third_party/libxext-headers/include
        ${PROJECT_SOURCE_DIR}/third_party/xorgproto-headers/include)
    target_link_libraries(kt_render PRIVATE ${CMAKE_DL_LIBS})
endif()

# The probe child (`keepterm --probe-renderer`): kt_render plus a hidden window
# of kt_platform_gui (defined by platform_gui/module.cmake, included after this file).
kt_module(kt_render_probe ${CMAKE_CURRENT_LIST_DIR}/probe/probe_command.cpp)
target_link_libraries(kt_render_probe PUBLIC kt_render PRIVATE kt_platform_gui)
target_link_libraries(keepterm PRIVATE kt_render_probe)
target_compile_definitions(keepterm PRIVATE KT_HAVE_RENDER_PROBE=1)
