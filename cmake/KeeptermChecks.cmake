# Post-build checks registered as CTest tests with the label "check".

set(kt_check_dir "${CMAKE_CURRENT_LIST_DIR}/checks")

if(APPLE)
    add_test(NAME check_macos_minos
        COMMAND ${CMAKE_COMMAND}
            -DBINARY=$<TARGET_FILE:keepterm>
            -DMINOS=${CMAKE_OSX_DEPLOYMENT_TARGET}
            -P ${kt_check_dir}/macos_minos.cmake)
    set_tests_properties(check_macos_minos PROPERTIES LABELS check)
elseif(UNIX)
    add_test(NAME check_linux_runtime
        COMMAND ${CMAKE_COMMAND}
            -DBINARY=$<TARGET_FILE:keepterm>
            -P ${kt_check_dir}/linux_runtime.cmake)
    set_tests_properties(check_linux_runtime PROPERTIES LABELS check)
endif()

# The icon files and the embedded pixels are what tools/gen_icon.py draws (assets/icon).
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME check_icon_generated
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/gen_icon.py --check)
    set_tests_properties(check_icon_generated PROPERTIES LABELS check TIMEOUT 300)
endif()

# Windows: both exes carry the application icon and the version (src/app/keepterm.rc.in).
if(WIN32)
    add_executable(kt_win_resources_check ${PROJECT_SOURCE_DIR}/tests/support/win_resources_check.cpp)
    target_link_libraries(kt_win_resources_check PRIVATE version)
    kt_set_warnings(kt_win_resources_check)
    foreach(kt_exe keepterm keepterm-mcp)
        add_test(NAME check_win_resources_${kt_exe}
            COMMAND kt_win_resources_check $<TARGET_FILE:${kt_exe}> ${PROJECT_VERSION} 8)
        set_tests_properties(check_win_resources_${kt_exe} PROPERTIES LABELS check)
    endforeach()
    # The check itself fails on an exe without them (its own).
    add_test(NAME check_win_resources_catches_missing
        COMMAND kt_win_resources_check $<TARGET_FILE:kt_win_resources_check> ${PROJECT_VERSION} 8)
    set_tests_properties(check_win_resources_catches_missing PROPERTIES LABELS check WILL_FAIL TRUE)
endif()

# Release packaging (cmake/KeeptermPackaging.cmake): the package of this tree passes
# cmake/VerifyPackage.cmake, and the verifier catches a package without keepterm-mcp.
# Not in sanitizer trees (their binaries need the sanitizer runtimes).
if(NOT KT_SANITIZE)
    if(APPLE)
        set(kt_pkg_gen TGZ)
        set(kt_pkg_kind macos)
    elseif(WIN32)
        set(kt_pkg_gen ZIP)
        set(kt_pkg_kind windows)
    else()
        set(kt_pkg_gen TGZ)
        set(kt_pkg_kind linux)
    endif()
    foreach(kt_break "" ON)
        set(kt_name check_package)
        if(kt_break)
            set(kt_name check_package_catches_missing)
        endif()
        add_test(NAME ${kt_name}
            COMMAND ${CMAKE_COMMAND} -DBUILD=${CMAKE_BINARY_DIR} -DSOURCE=${PROJECT_SOURCE_DIR}
                -DVERSION=${PROJECT_VERSION} -DGEN=${kt_pkg_gen} -DKIND=${kt_pkg_kind}
                -DNAME=${KT_PACKAGE_NAME} -DCONFIG=$<CONFIG> -DBREAK=${kt_break}
                -P ${kt_check_dir}/package_check.cmake)
        set_tests_properties(${kt_name} PROPERTIES LABELS check TIMEOUT 600 RESOURCE_LOCK kt_package)
    endforeach()
endif()
