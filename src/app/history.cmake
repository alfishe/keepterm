# The Session History window: the list model and the window's controller. A core
# library: the tests build them headless.
kt_module(kt_app_history ${CMAKE_CURRENT_LIST_DIR}/history_model.cpp ${CMAKE_CURRENT_LIST_DIR}/history_controller.cpp)
target_link_libraries(kt_app_history PUBLIC kt_session kt_commands kt_ui)
set_target_properties(kt_app_history PROPERTIES KT_CORE ON)
target_link_libraries(keepterm PRIVATE kt_app_history)
# The output viewer window (app/output_viewer.h).
target_sources(keepterm PRIVATE ${CMAKE_CURRENT_LIST_DIR}/output_viewer.cpp ${CMAKE_CURRENT_LIST_DIR}/output_viewer_text.cpp)
