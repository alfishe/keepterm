# CTest check of the release packaging (cmake/KeeptermPackaging.cmake): packs this build tree
# into an archive in a private folder and runs cmake/VerifyPackage.cmake on it. The archive is
# .tar.gz (Linux, macOS: the bundle without the dmg) or .zip (Windows); CI packs the real
# DEB / DMG / ZIP and verifies those (.github/workflows/release.yml).
#
# BREAK=ON: removes keepterm-mcp from the package first; the check passes only if the verifier
# reports exactly that (it must not wave a broken package through).
#
#   cmake -DBUILD=<tree> -DSOURCE=<repo> -DVERSION=<x.y.z> -DGEN=TGZ|ZIP -DKIND=linux|macos|windows
#         -DNAME=<package name> [-DCONFIG=Debug] [-DBREAK=ON] -P package_check.cmake

set(dir "${BUILD}/package-check${BREAK}")
file(REMOVE_RECURSE "${dir}")
file(MAKE_DIRECTORY "${dir}")
set(cfg_args "")
if(CONFIG)
    set(cfg_args -C "${CONFIG}")
endif()
execute_process(COMMAND ${CMAKE_CPACK_COMMAND} -G ${GEN} ${cfg_args} --config "${BUILD}/CPackConfig.cmake" -B "${dir}"
    WORKING_DIRECTORY "${BUILD}" OUTPUT_VARIABLE out ERROR_VARIABLE out RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "cpack failed:\n${out}")
endif()
if(GEN STREQUAL "ZIP")
    set(pkg "${dir}/${NAME}.zip")
else()
    set(pkg "${dir}/${NAME}.tar.gz")
endif()
if(NOT EXISTS "${pkg}")
    message(FATAL_ERROR "cpack wrote no ${pkg}:\n${out}")
endif()

set(allow_pdb OFF)
if(NOT CONFIG STREQUAL "Release")
    set(allow_pdb ON)
endif()

if(BREAK)
    set(x "${dir}/x")
    file(ARCHIVE_EXTRACT INPUT "${pkg}" DESTINATION "${x}")
    file(GLOB_RECURSE mcp "${x}/*keepterm-mcp" "${x}/*keepterm-mcp.exe")
    if(NOT mcp)
        message(FATAL_ERROR "the package has no keepterm-mcp to remove")
    endif()
    file(REMOVE ${mcp})
    file(REMOVE "${pkg}")
    if(GEN STREQUAL "ZIP")
        execute_process(COMMAND ${CMAKE_COMMAND} -E tar cf "${pkg}" --format=zip "${NAME}" WORKING_DIRECTORY "${x}")
    else()
        execute_process(COMMAND ${CMAKE_COMMAND} -E tar czf "${pkg}" "${NAME}" WORKING_DIRECTORY "${x}")
    endif()
endif()

execute_process(COMMAND ${CMAKE_COMMAND} -DPACKAGE=${pkg} -DVERSION=${VERSION} -DKIND=${KIND}
        -DALLOW_PDB=${allow_pdb} -P "${SOURCE}/cmake/VerifyPackage.cmake"
    OUTPUT_VARIABLE vout ERROR_VARIABLE vout RESULT_VARIABLE vrc)
message("${vout}")
if(BREAK)
    if(vrc EQUAL 0)
        message(FATAL_ERROR "the verifier passed a package without keepterm-mcp")
    endif()
    if(NOT vout MATCHES "FAIL: missing [^\n]*keepterm-mcp")
        message(FATAL_ERROR "the verifier failed, but not on the missing keepterm-mcp")
    endif()
    message(STATUS "the verifier caught the missing keepterm-mcp")
elseif(NOT vrc EQUAL 0)
    message(FATAL_ERROR "package check failed")
endif()
file(REMOVE_RECURSE "${dir}")
