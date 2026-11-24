# The Settings window: the pane layout over the schema and the controller that
# writes config.toml. A core library: the tests build its panes headless.
kt_module(kt_app_settings ${CMAKE_CURRENT_LIST_DIR}/settings_layout.cpp ${CMAKE_CURRENT_LIST_DIR}/settings_controller.cpp)
target_link_libraries(kt_app_settings PUBLIC kt_ui kt_config kt_commands kt_store kt_theme PRIVATE kt_agents)
set_target_properties(kt_app_settings PROPERTIES KT_CORE ON)
target_link_libraries(keepterm PRIVATE kt_app_settings)
target_sources(keepterm PRIVATE ${CMAKE_CURRENT_LIST_DIR}/settings_open.cpp ${CMAKE_CURRENT_LIST_DIR}/ui_window_host.cpp
    ${CMAKE_CURRENT_LIST_DIR}/limits_refresher.cpp)
if(WIN32)
    target_link_libraries(keepterm PRIVATE shell32)
endif()
