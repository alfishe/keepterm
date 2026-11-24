# Release packages: install rules and CPack. Built with `cpack -G <generator>`
# from a release build tree; cmake/VerifyPackage.cmake checks the result.
#
#   Linux    keepterm-Linux-<arch>.tar.gz   (TGZ)  bin/, share/ (desktop file, icons, completions)
#            keepterm-Linux-<arch>.deb      (DEB)  the same tree under /usr
#   macOS    keepterm-macOS-<arch>.dmg      (DragNDrop) keepterm.app, ad-hoc signed; one package
#                                                       per architecture (arm64, x86_64)
#   Windows  keepterm-Windows-<arch>.zip    (ZIP)  keepterm.exe, keepterm-mcp.exe, notices
#
# Every binary is stripped (CPACK_STRIP_FILES). Linux packages carry glibc as the only shared
# dependency; the X11, fontconfig, D-Bus and Vulkan libraries are opened at run time.

include(KeeptermNotices)

set(KT_PACKAGE_VERSION "${PROJECT_VERSION}" CACHE STRING
    "Package version (deb version, CFBundleVersion); snapshots pass a build stamp")
set(KT_BUNDLE_ID "keepterm" CACHE STRING "macOS bundle identifier (the platform app_id)")

set(kt_notices "${CMAKE_BINARY_DIR}/package/THIRD_PARTY_NOTICES.txt")
kt_generate_notices("${kt_notices}")

# Package architecture names: x86_64, arm64 (universal if both macOS slices are built).
if(APPLE)
    set(kt_pkg_os macOS)
    if(CMAKE_OSX_ARCHITECTURES MATCHES "arm64" AND CMAKE_OSX_ARCHITECTURES MATCHES "x86_64")
        set(kt_pkg_arch universal)
    elseif(CMAKE_OSX_ARCHITECTURES)
        set(kt_pkg_arch "${CMAKE_OSX_ARCHITECTURES}")
    else()
        set(kt_pkg_arch "${CMAKE_HOST_SYSTEM_PROCESSOR}")
    endif()
elseif(WIN32)
    set(kt_pkg_os Windows)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|arm64|aarch64")
        set(kt_pkg_arch arm64)
    else()
        set(kt_pkg_arch x86_64)
    endif()
else()
    set(kt_pkg_os Linux)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
        set(kt_pkg_arch arm64)
    else()
        set(kt_pkg_arch x86_64)
    endif()
endif()
set(KT_PACKAGE_NAME "keepterm-${kt_pkg_os}-${kt_pkg_arch}" CACHE INTERNAL "")

set(kt_license "${PROJECT_SOURCE_DIR}/LICENSE")

if(APPLE)
    # keepterm.app: both binaries in Contents/MacOS (keepterm finds keepterm-mcp beside itself).
    set(kt_app keepterm.app/Contents)
    configure_file("${PROJECT_SOURCE_DIR}/assets/macos/Info.plist.in"
        "${CMAKE_BINARY_DIR}/package/Info.plist" @ONLY)
    install(TARGETS keepterm keepterm-mcp RUNTIME DESTINATION ${kt_app}/MacOS)
    install(FILES "${CMAKE_BINARY_DIR}/package/Info.plist" DESTINATION ${kt_app})
    install(FILES "${PROJECT_SOURCE_DIR}/assets/icon/keepterm.icns" DESTINATION ${kt_app}/Resources)
    install(FILES "${kt_notices}" DESTINATION ${kt_app}/Resources)
    if(EXISTS "${kt_license}")
        install(FILES "${kt_license}" DESTINATION ${kt_app}/Resources)
    endif()
    # Sealed after stripping (install(CODE) runs after the targets are installed and stripped).
    # Ad-hoc until a Developer ID exists (code signing).
    install(CODE "execute_process(COMMAND codesign --force --sign - --timestamp=none
        \"\${CMAKE_INSTALL_PREFIX}/keepterm.app/Contents/MacOS/keepterm-mcp\" COMMAND_ERROR_IS_FATAL ANY)
        execute_process(COMMAND codesign --force --sign - --timestamp=none
        \"\${CMAKE_INSTALL_PREFIX}/keepterm.app\" COMMAND_ERROR_IS_FATAL ANY)")
elseif(WIN32)
    install(TARGETS keepterm keepterm-mcp RUNTIME DESTINATION .)
    install(FILES "${kt_notices}" DESTINATION .)
    if(EXISTS "${kt_license}")
        install(FILES "${kt_license}" DESTINATION . RENAME LICENSE.txt)
    endif()
else()
    install(TARGETS keepterm keepterm-mcp RUNTIME DESTINATION bin)
    install(FILES "${PROJECT_SOURCE_DIR}/assets/linux/keepterm.desktop" DESTINATION share/applications)
    foreach(px 16 32 48 64 128 256 512)
        install(FILES "${PROJECT_SOURCE_DIR}/assets/icon/png/keepterm-${px}.png"
            DESTINATION share/icons/hicolor/${px}x${px}/apps RENAME keepterm.png)
    endforeach()
    install(FILES "${PROJECT_SOURCE_DIR}/assets/icon/keepterm.svg" DESTINATION share/icons/hicolor/scalable/apps)
    install(FILES "${kt_notices}" DESTINATION share/doc/keepterm)
    if(EXISTS "${kt_license}")
        install(FILES "${kt_license}" DESTINATION share/doc/keepterm)
    endif()
    # Shell completions: what `keepterm completion <shell>` prints (src/app/cli_commands.cpp).
    # Not in sanitizer trees: the binary runs at build time there, and nothing is packaged.
    if(NOT KT_SANITIZE)
        # zsh's script is bashcompinit-based (an eval line in .zshrc), not a site-functions file.
        set(kt_comp "${CMAKE_BINARY_DIR}/package/completions")
        # A private home: the build touches nothing of the user's (and works where HOME is read-only).
        set(kt_comp_env ${CMAKE_COMMAND} -E env HOME=${kt_comp}/home XDG_CONFIG_HOME=${kt_comp}/home/.config
            XDG_STATE_HOME=${kt_comp}/home/.local/state XDG_DATA_HOME=${kt_comp}/home/.local/share
            XDG_CACHE_HOME=${kt_comp}/home/.cache XDG_RUNTIME_DIR=${kt_comp}/home/run)
        add_custom_command(OUTPUT ${kt_comp}/keepterm.bash ${kt_comp}/keepterm.fish
            COMMAND ${CMAKE_COMMAND} -E make_directory ${kt_comp}/home/run
            COMMAND ${kt_comp_env} $<TARGET_FILE:keepterm> completion bash > ${kt_comp}/keepterm.bash
            COMMAND ${kt_comp_env} $<TARGET_FILE:keepterm> completion fish > ${kt_comp}/keepterm.fish
            DEPENDS keepterm VERBATIM)
        add_custom_target(kt_completions ALL DEPENDS ${kt_comp}/keepterm.bash ${kt_comp}/keepterm.fish)
        install(FILES ${kt_comp}/keepterm.bash DESTINATION share/bash-completion/completions RENAME keepterm)
        install(FILES ${kt_comp}/keepterm.fish DESTINATION share/fish/vendor_completions.d)
    endif()
endif()

set(CPACK_PACKAGE_NAME keepterm)
set(CPACK_PACKAGE_VENDOR "keepterm")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_PACKAGE_FILE_NAME "${KT_PACKAGE_NAME}")
set(CPACK_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}/packages")
set(CPACK_STRIP_FILES TRUE)
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
set(CPACK_ARCHIVE_THREADS 0)
if(EXISTS "${kt_license}")
    set(CPACK_RESOURCE_FILE_LICENSE "${kt_license}")
endif()

# DEB: everything under /usr; the X11 & co. libraries are recommended, not required, because
# the headless commands (--session, --resources, the CLI) run without them.
set(CPACK_DEBIAN_PACKAGE_VERSION "${KT_PACKAGE_VERSION}")
set(CPACK_DEBIAN_PACKAGE_RELEASE 1)
set(CPACK_DEBIAN_FILE_NAME "${KT_PACKAGE_NAME}.deb")
if(kt_pkg_arch STREQUAL "arm64")
    set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE arm64)
else()
    set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE amd64)
endif()
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "keepterm project")
set(CPACK_DEBIAN_PACKAGE_SECTION x11)
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libc6 (>= 2.28)")
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "libx11-6, libxext6, libxrandr2, libfontconfig1, libdbus-1-3, libvulkan1")
set(CPACK_DEBIAN_COMPRESSION_TYPE xz)

# DMG: keepterm.app and a link to /Applications.
set(CPACK_DMG_VOLUME_NAME keepterm)
set(CPACK_DMG_FORMAT UDZO)

# The archive generators put the tree at the top of the archive; DEB installs it under /usr.
set(CPACK_PROJECT_CONFIG_FILE "${CMAKE_BINARY_DIR}/package/CPackProject.cmake")
file(WRITE "${CPACK_PROJECT_CONFIG_FILE}" [=[
if(CPACK_GENERATOR STREQUAL "DEB")
    set(CPACK_PACKAGING_INSTALL_PREFIX /usr)
    set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
elseif(CPACK_GENERATOR STREQUAL "DragNDrop")
    set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
endif()
]=])

if(APPLE)
    set(CPACK_GENERATOR DragNDrop)
elseif(WIN32)
    set(CPACK_GENERATOR ZIP)
else()
    set(CPACK_GENERATOR "TGZ;DEB")
endif()
include(CPack)
