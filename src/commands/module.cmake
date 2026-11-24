# kt_commands: the command registry, the keymap and its resolver, the fuzzy matcher
# and the palette engine. UI-independent core: no OS headers; platform key events come in
# through the header-only platform_gui/input.h. Included from src/CMakeLists.txt.

set(kt_cmd ${CMAKE_CURRENT_LIST_DIR})

kt_module(kt_commands
    ${kt_cmd}/keys.cpp
    ${kt_cmd}/context.cpp
    ${kt_cmd}/registry.cpp
    ${kt_cmd}/catalog.cpp
    ${kt_cmd}/keymap.cpp
    ${kt_cmd}/layout.cpp
    ${kt_cmd}/resolver.cpp
    ${kt_cmd}/matcher.cpp
    ${kt_cmd}/palette.cpp
    ${kt_cmd}/keymap_doc.cpp
    ${kt_cmd}/cli.cpp)
target_link_libraries(kt_commands PUBLIC kt_base kt_config PRIVATE kt_platform_core)

# keepterm --command list | run | keymap
target_link_libraries(keepterm PRIVATE kt_commands)
target_compile_definitions(keepterm PRIVATE KT_HAVE_COMMANDS=1)

# The adapter to the workspace model: WorkspaceTarget, palette usage and data.
kt_module(kt_commands_ws ${kt_cmd}/workspace_target.cpp)
target_link_libraries(kt_commands_ws PUBLIC kt_commands kt_workspace)
