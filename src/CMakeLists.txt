# One static library per module.

function(kt_module target)
    add_library(${target} STATIC ${ARGN})
    target_include_directories(${target} PUBLIC ${PROJECT_SOURCE_DIR}/src)
    kt_set_warnings(${target})
endfunction()

kt_module(kt_base
    base/error.cpp
    base/crc32c.cpp
    base/log.cpp
    base/fault.cpp)

if(WIN32)
    kt_module(kt_platform_core platform/random_win.cpp
        platform/paths_common.cpp platform/paths_win.cpp platform/file_watcher_win.cpp)
    target_link_libraries(kt_platform_core PUBLIC kt_base PRIVATE bcrypt shell32 ole32)
elseif(APPLE)
    kt_module(kt_platform_core platform/random_posix.cpp
        platform/paths_common.cpp platform/paths_posix.cpp platform/file_watcher_macos.cpp)
    target_link_libraries(kt_platform_core PUBLIC kt_base "-framework CoreServices")
else()
    kt_module(kt_platform_core platform/random_posix.cpp
        platform/paths_common.cpp platform/paths_posix.cpp platform/file_watcher_linux.cpp)
    find_package(Threads REQUIRED)
    target_link_libraries(kt_platform_core PUBLIC kt_base Threads::Threads)
endif()

kt_module(kt_vt
    vt/parser.cpp
    vt/width.cpp
    vt/width_tables.cpp
    vt/tables.cpp
    vt/screen.cpp
    vt/scrollback.cpp
    vt/frame.cpp
    vt/terminal.cpp
    vt/terminal_csi.cpp
    vt/terminal_osc.cpp
    vt/dump.cpp
    vt/input.cpp
    vt/snapshot.cpp)
target_link_libraries(kt_vt PUBLIC kt_base)
target_compile_definitions(kt_vt PRIVATE KT_VERSION="${PROJECT_VERSION}")

kt_module(kt_session_proto session/ids.cpp session/protocol.cpp session/json.cpp session/journal.cpp)
target_link_libraries(kt_session_proto PUBLIC kt_base PRIVATE kt_platform_core)

# The console agent's scraper and input decoder are portable, so their tests run everywhere.
if(WIN32)
    kt_module(kt_pty
        pty/pty_win.cpp
        pty/util_win.cpp
        pty/conpty_win.cpp
        pty/agent_pty_win.cpp
        pty/agent/console_agent_win.cpp
        pty/agent/scraper.cpp
        pty/agent/input_decoder.cpp
        pty/shell_integration.cpp)
    target_link_libraries(kt_pty PRIVATE kt_platform_core advapi32 user32 userenv winmm)
else()
    kt_module(kt_pty pty/pty_posix.cpp pty/agent/scraper.cpp pty/agent/input_decoder.cpp
        pty/shell_integration.cpp)
endif()
target_link_libraries(kt_pty PUBLIC kt_base)
target_compile_definitions(kt_pty PRIVATE KT_VERSION="${PROJECT_VERSION}")

# Working directory helpers of the session host: parsing, precedence, OS query.
if(WIN32)
    kt_module(kt_session_cwd session/cwd.cpp session/cwd_query_win.cpp)
else()
    kt_module(kt_session_cwd session/cwd.cpp session/cwd_query_posix.cpp)
endif()
target_link_libraries(kt_session_cwd PUBLIC kt_base)

# Sessions: system layer, discovery, history, the host role, the client and the CLI.
if(WIN32)
    set(kt_session_os session/os_win.cpp session/ipc_win.cpp)
else()
    set(kt_session_os session/os_posix.cpp session/ipc_posix.cpp)
endif()
kt_module(kt_session
    ${kt_session_os}
    session/registry.cpp
    session/suspend.cpp
    session/client.cpp
    session/history.cpp
    session/host.cpp
    session/gui_state.cpp
    session/cli.cpp)
target_link_libraries(kt_session PUBLIC kt_session_proto kt_session_cwd kt_vt kt_pty kt_base PRIVATE kt_platform_core)
target_compile_definitions(kt_session PRIVATE KT_VERSION="${PROJECT_VERSION}")
if(WIN32)
    target_link_libraries(kt_session PRIVATE advapi32 user32 shell32)
elseif(NOT APPLE)
    find_package(Threads REQUIRED)
    target_link_libraries(kt_session PRIVATE Threads::Threads)
endif()

add_executable(keepterm app/main.cpp)
target_compile_definitions(keepterm PRIVATE KT_VERSION="${PROJECT_VERSION}")
target_link_libraries(keepterm PRIVATE kt_vt kt_session_proto kt_session kt_pty)
if(WIN32)
    target_link_libraries(keepterm PRIVATE shell32)  # CommandLineToArgvW (UTF-8 argv)
endif()
kt_set_warnings(keepterm)

# Windows resources: the application icon and the version (app/keepterm.rc.in), on both exes.
function(kt_win_resources target description)
    if(NOT WIN32)
        return()
    endif()
    set(KT_RC_NAME ${target})
    set(KT_RC_DESCRIPTION "${description}")
    set(KT_RC_ICON "${PROJECT_SOURCE_DIR}/assets/icon/keepterm.ico")
    string(REPLACE "/" "\\\\" KT_RC_ICON "${KT_RC_ICON}")
    set(rc ${CMAKE_BINARY_DIR}/rc/${target}.rc)
    configure_file(${PROJECT_SOURCE_DIR}/src/app/keepterm.rc.in ${rc} @ONLY)
    target_sources(${target} PRIVATE ${rc})
    set_property(SOURCE ${rc} APPEND PROPERTY OBJECT_DEPENDS
        ${PROJECT_SOURCE_DIR}/assets/icon/keepterm.ico)
endfunction()
kt_win_resources(keepterm "keepterm terminal")

# keepterm-mcp: the stdio bridge an agent's MCP client starts; next to keepterm.
add_executable(keepterm-mcp mcp_bridge/main.cpp)
target_link_libraries(keepterm-mcp PRIVATE kt_session)
kt_set_warnings(keepterm-mcp)
kt_win_resources(keepterm-mcp "keepterm MCP bridge")
add_dependencies(keepterm keepterm-mcp)

# Modules that keep their own build file (added as they land).
foreach(kt_mod text render platform_gui view resources config)
    include(${CMAKE_CURRENT_SOURCE_DIR}/${kt_mod}/module.cmake OPTIONAL)
endforeach()

# AI agents: recognition, records, resume command, session files. Pure core plus the
# OS access it needs (file system, process liveness) in a separate library.
kt_module(kt_agents
    agents/registry.cpp
    agents/agent_resume.cpp
    agents/resume_command.cpp
    agents/json_lite.cpp
    agents/record_json.cpp
    agents/session_files.cpp
    agents/capture.cpp)
target_link_libraries(kt_agents PUBLIC kt_base)
if(WIN32)
    kt_module(kt_agents_os agents/os_fs.cpp agents/os_process_win.cpp)
else()
    kt_module(kt_agents_os agents/os_fs.cpp agents/os_process_posix.cpp)
endif()
target_link_libraries(kt_agents_os PUBLIC kt_agents)
# Agent context telemetry: probes, reducers, the tracker
# and the statusLine hook's text edits are core; the Codex index (SQLite, read-only) and the hook's
# file operations are OS side.
target_sources(kt_agents PRIVATE agents/json_scan.cpp agents/context_common.cpp agents/context_claude.cpp
    agents/context_codex.cpp agents/context_agy.cpp agents/context_tracker.cpp agents/context_statusline.cpp agents/context_hook.cpp
    agents/limits_probe.cpp)
target_sources(kt_agents_os PRIVATE agents/context_os.cpp agents/context_hook_os.cpp agents/limits_probe_os.cpp)
target_link_libraries(kt_agents_os PUBLIC kt_platform_core PRIVATE kt_sqlite)

# Wiring of the modules into the GUI and the session host: the config keys of new panes and of
# the agents (session/pane_config.h), the probe cache in the cache folder (render_select.cpp).
target_sources(kt_session PRIVATE session/pane_config.cpp)
target_link_libraries(kt_session PUBLIC kt_config kt_platform_core)
target_link_libraries(kt_render PRIVATE kt_platform_core)
# The foreground chain (session/proc_chain.h), the agent capture and restore in the host
# (session/agent_keeper.h) and the containment the host sets up (resources/containment_setup.h).
kt_module(kt_containment resources/containment_setup.cpp)
target_link_libraries(kt_containment PUBLIC kt_base)
target_sources(kt_session PRIVATE session/proc_chain.cpp session/agent_keeper.cpp session/agent_context.cpp
    session/agent_statusline.cpp)
if(WIN32)
    target_sources(kt_session PRIVATE session/proc_chain_win.cpp)
else()
    target_sources(kt_session PRIVATE session/proc_chain_posix.cpp)
endif()
target_link_libraries(kt_session PUBLIC kt_agents_os kt_containment)
# `keepterm --resources` lists the live sessions (app/resource_sessions.h).
if(TARGET kt_resources)
    target_sources(keepterm PRIVATE app/resource_sessions.cpp)
endif()

# Warm history store: segments, catalog, search, retention.
include(${CMAKE_CURRENT_SOURCE_DIR}/store/module.cmake)

# Commands, keymap, matcher and palette engine.
include(${CMAKE_CURRENT_SOURCE_DIR}/commands/module.cmake)

# Themes and fonts: theme files, built-in themes, apply to terminal and view.
include(${CMAKE_CURRENT_SOURCE_DIR}/theme/module.cmake)

# UI toolkit: widgets, layout, compositor, the main window's workspace widgets.
include(${CMAKE_CURRENT_SOURCE_DIR}/ui/module.cmake)

# Workspace model, state file and session binding.
include(${CMAKE_CURRENT_SOURCE_DIR}/workspace/module.cmake)

# The main window: sidebar, tabs, splits, status area over the workspace model.
include(${CMAKE_CURRENT_SOURCE_DIR}/app/shell.cmake)
