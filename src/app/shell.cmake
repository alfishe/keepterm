# The main window: the workspace widgets of src/ui over the
# workspace model, a terminal session per pane, menus and the unified title bar.
#
# kt_app_shell is the core part the tests drive headless (the IWorkspaceView adapter).
kt_module(kt_app_shell ${CMAKE_CURRENT_LIST_DIR}/workspace_adapter.cpp ${CMAKE_CURRENT_LIST_DIR}/palette_controller.cpp)
target_link_libraries(kt_app_shell PUBLIC kt_ui kt_workspace kt_commands_ws)
set_target_properties(kt_app_shell PROPERTIES KT_CORE ON)

# What the monitor learns of a pane from its host (app/pane_sample.h), driven by real hosts in tests.
kt_module(kt_pane_sample ${CMAKE_CURRENT_LIST_DIR}/pane_sample.cpp)
target_link_libraries(kt_pane_sample PUBLIC kt_session kt_ui)

# The control channel to the running app: the GUI's server and the CLI.
kt_module(kt_control ${CMAKE_CURRENT_LIST_DIR}/control.cpp ${CMAKE_CURRENT_LIST_DIR}/launch.cpp ${CMAKE_CURRENT_LIST_DIR}/cli_commands.cpp
    ${CMAKE_CURRENT_LIST_DIR}/mcp.cpp)
target_link_libraries(kt_control PUBLIC kt_session PRIVATE kt_workspace kt_workspace_os kt_platform_core)

target_sources(keepterm PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/pane_session.cpp
    ${CMAKE_CURRENT_LIST_DIR}/main_window.cpp
    ${CMAKE_CURRENT_LIST_DIR}/desktop.cpp)
target_link_libraries(keepterm PRIVATE kt_app_shell kt_ui kt_workspace_os kt_commands_ws kt_theme kt_resources kt_store
    kt_agents kt_control kt_pane_sample)
if(WIN32)
    target_link_libraries(keepterm PRIVATE ole32 shell32 uuid)   # the folder chooser (desktop.cpp)
endif()
if(APPLE)
    target_sources(keepterm PRIVATE ${CMAKE_CURRENT_LIST_DIR}/mac_window.mm ${CMAKE_CURRENT_LIST_DIR}/desktop_mac.mm)
    set_source_files_properties(${CMAKE_CURRENT_LIST_DIR}/mac_window.mm ${CMAKE_CURRENT_LIST_DIR}/desktop_mac.mm
        PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(keepterm PRIVATE "-framework Cocoa")
endif()

# Settings (src/app/settings.cmake).
include(${CMAKE_CURRENT_LIST_DIR}/settings.cmake)
