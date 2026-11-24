# kt_platform_gui: windows, the event loop, input, DPI and clipboard.
# Included from src/CMakeLists.txt. GUI role only; the session host and the CLI never link it.

set(kt_pg ${CMAKE_CURRENT_LIST_DIR})

kt_module(kt_platform_gui
    ${kt_pg}/dpi.cpp
    ${kt_pg}/event.cpp
    ${kt_pg}/keyboard.cpp
    ${kt_pg}/loop_parts.cpp
    ${kt_pg}/utf.cpp)
target_link_libraries(kt_platform_gui PUBLIC kt_base)

# The application icon as pixels (platform_gui/app_icon.h): X11 sets it on every window; the
# tests check it on every platform.
kt_module(kt_app_icon ${kt_pg}/app_icon.cpp)

if(WIN32)
    target_sources(kt_platform_gui PRIVATE
        ${kt_pg}/platform_win.cpp
        ${kt_pg}/window_win.cpp)
    # Import allow-list; newer user32/shcore functions are resolved at run time.
    target_link_libraries(kt_platform_gui PRIVATE user32 gdi32 imm32 advapi32 shell32 ole32 shlwapi windowscodecs)
elseif(APPLE)
    set(kt_pg_objc ${kt_pg}/platform_macos.mm ${kt_pg}/window_macos.mm)
    target_sources(kt_platform_gui PRIVATE ${kt_pg_objc})
    set_source_files_properties(${kt_pg_objc} PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(kt_platform_gui PRIVATE
        "-framework AppKit" "-framework Foundation" "-framework QuartzCore"
        "-framework CoreGraphics" "-framework Carbon")
else()
    # X11 is dlopen'ed; the build uses vendored headers only (third_party/MANIFEST.toml).
    target_sources(kt_platform_gui PRIVATE
        ${kt_pg}/platform_linux.cpp
        ${kt_pg}/wayland_linux.cpp
        ${kt_pg}/x11_api_linux.cpp
        ${kt_pg}/platform_x11_linux.cpp
        ${kt_pg}/window_x11_linux.cpp)
    target_include_directories(kt_platform_gui SYSTEM PRIVATE
        ${PROJECT_SOURCE_DIR}/third_party/libx11-headers/include
        ${PROJECT_SOURCE_DIR}/third_party/libxext-headers/include
        ${PROJECT_SOURCE_DIR}/third_party/xorgproto-headers/include)
    target_link_libraries(kt_platform_gui PRIVATE ${CMAKE_DL_LIBS} kt_app_icon)
endif()

# OS appearance, light or dark (platform_gui/appearance.h; theme.mode = "system", src/theme).
if(WIN32)
    target_sources(kt_platform_gui PRIVATE ${kt_pg}/appearance_win.cpp)
elseif(APPLE)
    target_sources(kt_platform_gui PRIVATE ${kt_pg}/appearance_macos.mm)
    set_source_files_properties(${kt_pg}/appearance_macos.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
else()
    target_sources(kt_platform_gui PRIVATE ${kt_pg}/appearance_linux.cpp)
endif()
