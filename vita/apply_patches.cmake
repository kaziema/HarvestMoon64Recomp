# Applies the Vita patches in vita/patches to the submodules at configure time.
# A patch is skipped when it is already applied, so re-running configure is safe.
#
# To update a patch after editing files in a submodule:
#   git -C lib/N64ModernRuntime diff --ignore-submodules > vita/patches/N64ModernRuntime.patch
#   git -C lib/N64ModernRuntime/N64Recomp diff > vita/patches/N64Recomp.patch

find_program(HM64_PATCH_TOOL patch REQUIRED)

function(hm64_apply_patch patch_file target_dir)
    execute_process(
        COMMAND ${HM64_PATCH_TOOL} -p1 -R --dry-run -f -s -i ${patch_file}
        WORKING_DIRECTORY ${target_dir}
        RESULT_VARIABLE already_applied
        OUTPUT_QUIET ERROR_QUIET
    )
    if (already_applied EQUAL 0)
        message(STATUS "Vita patch already applied: ${patch_file}")
        return()
    endif()

    execute_process(
        COMMAND ${HM64_PATCH_TOOL} -p1 -N -f -s -i ${patch_file}
        WORKING_DIRECTORY ${target_dir}
        RESULT_VARIABLE apply_result
    )
    if (NOT apply_result EQUAL 0)
        message(FATAL_ERROR "Failed to apply Vita patch ${patch_file} in ${target_dir}. The submodule may have changed upstream.")
    endif()
    message(STATUS "Applied Vita patch: ${patch_file}")
endfunction()

hm64_apply_patch(${CMAKE_SOURCE_DIR}/vita/patches/N64ModernRuntime.patch ${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime)
hm64_apply_patch(${CMAKE_SOURCE_DIR}/vita/patches/N64Recomp.patch ${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/N64Recomp)
