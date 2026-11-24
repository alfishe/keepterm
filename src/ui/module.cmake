# kt_ui: the UI toolkit: widget tree, layout, display lists, compositor, events,
# theme tokens, animation, core and workspace widgets. Core code: no OS calls.

kt_module(kt_ui
    ${CMAKE_CURRENT_LIST_DIR}/icons.cpp
    ${CMAKE_CURRENT_LIST_DIR}/theme.cpp
    ${CMAKE_CURRENT_LIST_DIR}/text.cpp
    ${CMAKE_CURRENT_LIST_DIR}/text_service.cpp
    ${CMAKE_CURRENT_LIST_DIR}/widget.cpp
    ${CMAKE_CURRENT_LIST_DIR}/window.cpp
    ${CMAKE_CURRENT_LIST_DIR}/widgets.cpp
    ${CMAKE_CURRENT_LIST_DIR}/popups.cpp
    ${CMAKE_CURRENT_LIST_DIR}/palette_view.cpp
    ${CMAKE_CURRENT_LIST_DIR}/workspace_widgets.cpp
    ${CMAKE_CURRENT_LIST_DIR}/resource_panel.cpp
    ${CMAKE_CURRENT_LIST_DIR}/find_bar.cpp)
target_link_libraries(kt_ui PUBLIC kt_render kt_text kt_view kt_base PRIVATE kt_harfbuzz kt_freetype)
set_target_properties(kt_ui PROPERTIES KT_CORE ON)
# The Settings window's controls and chrome.
target_sources(kt_ui PRIVATE ${CMAKE_CURRENT_LIST_DIR}/form_widgets.cpp ${CMAKE_CURRENT_LIST_DIR}/settings_view.cpp)
