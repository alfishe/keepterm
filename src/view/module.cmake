# kt_view: terminal view. Core code: no OS calls.

kt_module(kt_view
    ${CMAKE_CURRENT_LIST_DIR}/terminal/theme.cpp
    ${CMAKE_CURRENT_LIST_DIR}/terminal/atlas_textures.cpp
    ${CMAKE_CURRENT_LIST_DIR}/terminal/terminal_view.cpp
    ${CMAKE_CURRENT_LIST_DIR}/terminal/text_ops.cpp)
target_link_libraries(kt_view PUBLIC kt_vt kt_text kt_render PRIVATE kt_pcre2)
set_target_properties(kt_view PROPERTIES KT_CORE ON)

# Headless rendering of the app (`keepterm --pty-render`).
target_sources(keepterm PRIVATE ${PROJECT_SOURCE_DIR}/src/app/pty_render.cpp ${PROJECT_SOURCE_DIR}/src/app/png_writer.cpp)
target_link_libraries(keepterm PRIVATE kt_view kt_text_platform kt_render)
target_compile_definitions(keepterm PRIVATE KT_HAVE_PTY_RENDER=1)

# The terminal window (`keepterm`, `keepterm --gui`).
target_sources(keepterm PRIVATE
    ${PROJECT_SOURCE_DIR}/src/app/gui_app.cpp
    ${PROJECT_SOURCE_DIR}/src/app/gui_input.cpp
    ${PROJECT_SOURCE_DIR}/src/app/clip_image.cpp
    ${PROJECT_SOURCE_DIR}/src/app/session_link.cpp
    ${PROJECT_SOURCE_DIR}/src/app/session_link_client.cpp)
target_link_libraries(keepterm PRIVATE kt_platform_gui kt_session)
target_compile_definitions(keepterm PRIVATE KT_HAVE_SESSIONS=1)
target_compile_definitions(keepterm PRIVATE KT_HAVE_GUI=1)
# keepterm lists kt_vt itself and again through kt_view; let CMake drop the repeat (the Apple
# linker warns about duplicate static libraries).
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.31)
    set_property(TARGET keepterm PROPERTY LINK_LIBRARIES_STRATEGY REORDER_FREELY)
endif()
