# The workspace model: projects, tabs, splits, windows, state file, session binding.
# kt_workspace is the UI-independent core (no OS calls); kt_workspace_os wires it to the real
# file system, the session hosts and the old per-window files.

kt_module(kt_workspace
    workspace/ids.cpp
    workspace/json.cpp
    workspace/paths.cpp
    workspace/split_tree.cpp
    workspace/model.cpp
    workspace/workspace.cpp
    workspace/state.cpp
    workspace/fs.cpp
    workspace/persist.cpp
    workspace/binder.cpp
    workspace/usage.cpp
    workspace/presets.cpp
    workspace/preset_store.cpp
    workspace/yaml.cpp
    workspace/project_file.cpp)
find_package(Threads REQUIRED)
target_link_libraries(kt_workspace PUBLIC kt_base kt_session_proto kt_agents PRIVATE kt_platform_core Threads::Threads)

kt_module(kt_workspace_os
    workspace/fs_os.cpp
    workspace/host_manager.cpp
    workspace/legacy.cpp
    workspace/runtime.cpp)
target_link_libraries(kt_workspace_os PUBLIC kt_workspace kt_config PRIVATE kt_session)
