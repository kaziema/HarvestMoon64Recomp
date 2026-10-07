# Post-link check: fails if any pthread or sched function is an undefined weak symbol in the ELF.
#
# libstdc++ references its threading functions weakly. An unresolved one is not a link error: the ARM linker
# turns calls to it into no-ops, and the program crashes later on hardware (TEST 1: pthread_cancel,
# TEST 3: pthread_once). The fix for a hit is another -Wl,-u,<symbol> in vita/vita.cmake.
#
# Inputs: NM (arm-vita-eabi-nm), ELF (linked executable).

execute_process(
    COMMAND "${NM}" "${ELF}"
    OUTPUT_VARIABLE nm_output
    RESULT_VARIABLE nm_result
)
if (NOT nm_result EQUAL 0)
    message(FATAL_ERROR "check_weak_symbols: ${NM} failed on ${ELF}")
endif()

string(REGEX MATCHALL "[ \t]+[wv][ \t]+(pthread_|sched_)[A-Za-z0-9_]+" weak_hits "${nm_output}")
if (weak_hits)
    list(TRANSFORM weak_hits REPLACE "^[ \t]+[wv][ \t]+" "")
    list(JOIN weak_hits ", " weak_list)
    message(FATAL_ERROR "Unresolved weak threading symbols (calls to these become no-ops): ${weak_list}. Add -Wl,-u,<symbol> for each in vita/vita.cmake.")
endif()
