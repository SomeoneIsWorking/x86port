cmake_minimum_required(VERSION 3.20)
project(x86port C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# The guest is x86-32 and this is a HOST library: nothing here is compiled for
# the guest architecture, so no -m32 anywhere. That is the whole point of a
# runtime engine.
# Zydis: decode only. Pinned at v4.1.1 in vendor/zydis, with pre-generated
# tables -- no build-time code generation, which is the whole point (G001).
set(ZYDIS_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_DOXYGEN OFF CACHE BOOL "" FORCE)
add_subdirectory(vendor/zydis EXCLUDE_FROM_ALL)

# jit-common is CONSUMED, not vendored: the code region and block cache have one
# implementation, in one repo, and a stale in-tree copy that silently won is the
# exact failure that split exists to end. So the resolver REFUSES by naming
# every path it tried rather than falling back to anything.
set(X86P_JITCOMMON_CANDIDATES
  "${X86PORT_JITCOMMON_DIR}"
  "$ENV{X86PORT_JITCOMMON_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/../jit-common")
set(X86P_JITCOMMON "")
foreach(cand IN LISTS X86P_JITCOMMON_CANDIDATES)
  if(cand AND EXISTS "${cand}/src/jitcommon/block_cache.h")
    set(X86P_JITCOMMON "${cand}")
    break()
  endif()
endforeach()
if(NOT X86P_JITCOMMON)
  message(FATAL_ERROR
    "jit-common not found. Tried, in order: ${X86P_JITCOMMON_CANDIDATES}\n"
    "Clone it beside this repo, or set -DX86PORT_JITCOMMON_DIR=<path>. "
    "There is deliberately no in-tree copy to fall back to.")
endif()
add_subdirectory("${X86P_JITCOMMON}" "${CMAKE_CURRENT_BINARY_DIR}/jitcommon" EXCLUDE_FROM_ALL)

add_library(x86port STATIC
  src/x86port/string_ops.c
  src/x86port/bcd.c
    src/x86port/cpuid.c
    src/x86port/x87_state.c
    src/x86port/privilege.c
    src/x86port/bit_ops.c
    src/x86port/simd.c
  src/x86port/simd_int.c
  src/x86port/simd_float.c
  src/x86port/three_dnow.c
  src/x86port/decode.c
  src/x86port/decode_cache.c
  src/x86port/engine.c
  src/x86port/flags.c
  src/x86port/alu.c
  src/x86port/cpu.c
  src/x86port/cond.c
  src/x86port/exec.c
  src/x86port/x87.c
  src/x86port/x87_exec.c
  src/x86port/x87_transcendental.c
  src/x86port/emit_x64.c
  src/x86port/jit_x64.c
  src/x86port/jit_engine.c)
target_include_directories(x86port PUBLIC src/x86port)
target_link_libraries(x86port PUBLIC jitcommon PRIVATE Zydis m)
target_compile_options(x86port PRIVATE -Wall -Wextra -Werror)

# Tests and tools belong to THIS project, not to whoever consumes it. A port
# that pulls x86port in with add_subdirectory would otherwise inherit every
# test here into its own ctest run, where a failure says nothing about the
# port. Same reasoning, and same mechanism, as jit-common's own guard.
if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  return()
endif()

# The differential instrument. Not a test: it needs a title's Ghidra export,
# which is not in this repo and never will be.
add_executable(x86p_decode_diff tools/decode_diff.c)
target_link_libraries(x86p_decode_diff PRIVATE x86port)
target_compile_options(x86p_decode_diff PRIVATE -Wall -Wextra -Werror)

add_executable(test_jit_engine tests/test_jit_engine.c)
target_link_libraries(test_jit_engine PRIVATE x86port)
target_include_directories(test_jit_engine PRIVATE src)
target_compile_options(test_jit_engine PRIVATE -Wall -Wextra -Werror)
add_test(NAME test_jit_engine COMMAND test_jit_engine)

add_executable(x86p_jit_coverage tools/jit_coverage.c)
target_link_libraries(x86p_jit_coverage PRIVATE x86port)
target_compile_options(x86p_jit_coverage PRIVATE -Wall -Wextra -Werror -O2)

add_executable(x86p_jit_bench tools/jit_bench.c)
target_link_libraries(x86p_jit_bench PRIVATE x86port)
target_compile_options(x86p_jit_bench PRIVATE -Wall -Wextra -Werror -O2)

enable_testing()
add_executable(test_three_dnow tests/test_three_dnow.c)
target_link_libraries(test_three_dnow PRIVATE x86port)
target_compile_options(test_three_dnow PRIVATE -Wall -Wextra -Werror)
add_test(NAME test_three_dnow COMMAND test_three_dnow)

add_executable(test_integer_tail tests/test_integer_tail.c)
target_link_libraries(test_integer_tail PRIVATE x86port)
target_compile_options(test_integer_tail PRIVATE -Wall -Wextra -Werror)
add_test(NAME test_integer_tail COMMAND test_integer_tail)

add_executable(test_x87_fn tests/test_x87_fn.c)
target_link_libraries(test_x87_fn PRIVATE x86port)
target_compile_options(test_x87_fn PRIVATE -Wall -Wextra -Werror)
add_test(NAME test_x87_fn COMMAND test_x87_fn)

add_executable(test_simd tests/test_simd.c)
target_link_libraries(test_simd PRIVATE x86port)
target_compile_options(test_simd PRIVATE -Wall -Wextra -Werror)
add_test(NAME test_simd COMMAND test_simd)

add_executable(test_string_ops tests/test_string_ops.c)
target_link_libraries(test_string_ops PRIVATE x86port)
target_compile_options(test_string_ops PRIVATE -Wall -Wextra -Werror)
add_test(NAME test_string_ops COMMAND test_string_ops)

add_executable(test_decode tests/test_decode.c)
target_link_libraries(test_decode PRIVATE x86port)
target_compile_options(test_decode PRIVATE -Wall -Wextra -Werror)
add_test(NAME test_decode COMMAND test_decode)

add_executable(test_decode_cache tests/test_decode_cache.c)
target_link_libraries(test_decode_cache PRIVATE x86port)
target_compile_options(test_decode_cache PRIVATE -Wall -Wextra -Werror)
add_test(NAME test_decode_cache COMMAND test_decode_cache)

add_executable(test_engine tests/test_engine.c)
target_link_libraries(test_engine PRIVATE x86port)
target_compile_options(test_engine PRIVATE -Wall -Wextra -Werror)
add_test(NAME test_engine COMMAND test_engine)

add_executable(test_flags tests/test_flags.c)
target_link_libraries(test_flags PRIVATE x86port)
target_compile_options(test_flags PRIVATE -Wall -Wextra -Werror)
add_test(NAME test_flags COMMAND test_flags)

add_executable(test_alu tests/test_alu.c)
target_link_libraries(test_alu PRIVATE x86port)
target_compile_options(test_alu PRIVATE -Wall -Wextra -Werror)
add_test(NAME test_alu COMMAND test_alu)

add_executable(test_cpu tests/test_cpu.c)
target_link_libraries(test_cpu PRIVATE x86port)
target_compile_options(test_cpu PRIVATE -Wall -Wextra -Werror)
add_test(NAME test_cpu COMMAND test_cpu)

add_executable(test_cond tests/test_cond.c)
target_link_libraries(test_cond PRIVATE x86port)
target_compile_options(test_cond PRIVATE -Wall -Wextra -Werror)
add_test(NAME test_cond COMMAND test_cond)

add_executable(test_exec tests/test_exec.c)
target_link_libraries(test_exec PRIVATE x86port)
target_compile_options(test_exec PRIVATE -Wall -Wextra -Werror)
add_test(NAME test_exec COMMAND test_exec)

add_executable(test_x87 tests/test_x87.c)
target_link_libraries(test_x87 PRIVATE x86port m)
target_compile_options(test_x87 PRIVATE -Wall -Wextra -Werror)
add_test(NAME test_x87 COMMAND test_x87)

add_executable(test_x87_exec tests/test_x87_exec.c)
target_link_libraries(test_x87_exec PRIVATE x86port m)
target_compile_options(test_x87_exec PRIVATE -Wall -Wextra -Werror)
add_test(NAME test_x87_exec COMMAND test_x87_exec)

# The encoder's oracle is Zydis itself, so this test links it directly rather
# than through x86port's PRIVATE dependency.
add_executable(test_emit_x64 tests/test_emit_x64.c)
target_link_libraries(test_emit_x64 PRIVATE x86port Zydis)
target_compile_options(test_emit_x64 PRIVATE -Wall -Wextra -Werror)
add_test(NAME test_emit_x64 COMMAND test_emit_x64)

# The differential: translated code against the interpreter, whole machine.
add_executable(test_jit_x64 tests/test_jit_x64.c)
target_link_libraries(test_jit_x64 PRIVATE x86port)
target_compile_options(test_jit_x64 PRIVATE -Wall -Wextra -Werror)
add_test(NAME test_jit_x64 COMMAND test_jit_x64)
set_tests_properties(test_jit_x64 PROPERTIES SKIP_RETURN_CODE 77)
