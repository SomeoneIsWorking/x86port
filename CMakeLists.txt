cmake_minimum_required(VERSION 3.20)
project(x86port C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Warnings are errors on every host. MSVC-compatible drivers use slash
# options; passing GCC's -Wall through clang-cl instead enables a different,
# excessively broad warning set and makes the Windows contract accidental.
if(MSVC)
  set(X86P_WARN_FLAGS /W4 /WX)
  set(X86P_OPTIMIZE_FLAG /O2)
else()
  set(X86P_WARN_FLAGS -Wall -Wextra -Werror)
  set(X86P_OPTIMIZE_FLAG -O2)
endif()

function(x86p_strict_target target)
  target_compile_options(${target} PRIVATE ${X86P_WARN_FLAGS})
endfunction()

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

add_library(x86port_runtime STATIC
  src/x86port/diagnostic.c
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
  src/x86port/flags.c
  src/x86port/alu.c
  src/x86port/cpu.c
  src/x86port/cond.c
  src/x86port/x87.c
  src/x86port/x87_transcendental.c
  src/x86port/emit_x64.c
  src/x86port/jit_x64.c
  src/x86port/jit_x64_x87.c
  src/x86port/jit_profile.c
  src/x86port/jit_engine.c)
add_library(x86port ALIAS x86port_runtime)
target_include_directories(x86port_runtime PUBLIC src/x86port)
target_link_libraries(x86port_runtime PUBLIC jitcommon PRIVATE Zydis)
if(NOT WIN32)
  target_link_libraries(x86port_runtime PRIVATE m)
endif()
x86p_strict_target(x86port_runtime)

# Tests and tools belong to THIS project, not to whoever consumes it. A port
# that pulls x86port in with add_subdirectory would otherwise inherit every
# test here into its own ctest run, where a failure says nothing about the
# port. Same reasoning, and same mechanism, as jit-common's own guard.
if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  return()
endif()

# The interpreter exists only as a separately built oracle for this
# repository's tests and diagnostics. A consuming game returns above and can
# neither name nor link this target.
add_library(x86port_test_oracle STATIC
  src/x86port/cpu_compare.c
  src/x86port/decode_cache.c
  src/x86port/exec.c
  src/x86port/x87_exec.c)
target_include_directories(x86port_test_oracle PUBLIC src/x86port)
target_link_libraries(x86port_test_oracle PUBLIC x86port_runtime)
x86p_strict_target(x86port_test_oracle)

# This fixture deliberately links only the shipping library. It proves both
# that translated guest code runs and that unsupported guest code is reported
# without acquiring the test oracle through a transitive dependency.
add_executable(test_product_runtime tests/test_product_runtime.c)
target_link_libraries(test_product_runtime PRIVATE x86port_runtime)
x86p_strict_target(test_product_runtime)
add_test(NAME test_product_runtime COMMAND test_product_runtime)

add_executable(test_diagnostic tests/test_diagnostic.c)
target_link_libraries(test_diagnostic PRIVATE x86port_runtime)
x86p_strict_target(test_diagnostic)
add_test(NAME test_diagnostic COMMAND test_diagnostic)

# The differential instrument. Not a test: it needs a title's Ghidra export,
# which is not in this repo and never will be.
add_executable(x86p_decode_diff tools/decode_diff.c)
target_link_libraries(x86p_decode_diff PRIVATE x86port)
x86p_strict_target(x86p_decode_diff)

add_executable(test_jit_engine tests/test_jit_engine.c)
target_link_libraries(test_jit_engine PRIVATE x86port_runtime x86port_test_oracle)
target_include_directories(test_jit_engine PRIVATE src)
x86p_strict_target(test_jit_engine)
add_test(NAME test_jit_engine COMMAND test_jit_engine)

add_executable(test_jit_profile tests/test_jit_profile.c)
target_link_libraries(test_jit_profile PRIVATE x86port)
target_include_directories(test_jit_profile PRIVATE src/x86port)
x86p_strict_target(test_jit_profile)
add_test(NAME test_jit_profile COMMAND test_jit_profile)

if(NOT WIN32)
  # These operator-only instruments currently depend on POSIX process and
  # timing facilities. They are not part of the runtime or test oracle.
  add_executable(x86p_jit_coverage tools/jit_coverage.c)
  target_link_libraries(x86p_jit_coverage PRIVATE x86port_runtime x86port_test_oracle)
  x86p_strict_target(x86p_jit_coverage)
  target_compile_options(x86p_jit_coverage PRIVATE ${X86P_OPTIMIZE_FLAG})

  add_executable(x86p_jit_bench tools/jit_bench.c)
  target_link_libraries(x86p_jit_bench PRIVATE x86port_runtime x86port_test_oracle)
  x86p_strict_target(x86p_jit_bench)
  target_compile_options(x86p_jit_bench PRIVATE ${X86P_OPTIMIZE_FLAG})
endif()

enable_testing()
add_executable(test_three_dnow tests/test_three_dnow.c)
target_link_libraries(test_three_dnow PRIVATE x86port)
x86p_strict_target(test_three_dnow)
add_test(NAME test_three_dnow COMMAND test_three_dnow)

if(NOT WIN32)
  # These suites execute host instructions from POSIX mappings. Windows still
  # builds and exercises the shipping JIT and the test-only interpreter below;
  # the silicon-oracle suites remain on Linux/macOS where their harness exists.
  add_executable(test_integer_tail tests/test_integer_tail.c)
  target_link_libraries(test_integer_tail PRIVATE x86port_runtime x86port_test_oracle)
  x86p_strict_target(test_integer_tail)
  add_test(NAME test_integer_tail COMMAND test_integer_tail)
  set_tests_properties(test_integer_tail PROPERTIES SKIP_RETURN_CODE 77)

  add_executable(test_x87_fn tests/test_x87_fn.c)
  target_link_libraries(test_x87_fn PRIVATE x86port_runtime x86port_test_oracle)
  x86p_strict_target(test_x87_fn)
  add_test(NAME test_x87_fn COMMAND test_x87_fn)

  add_executable(test_simd tests/test_simd.c)
  target_link_libraries(test_simd PRIVATE x86port)
  x86p_strict_target(test_simd)
  add_test(NAME test_simd COMMAND test_simd)

  add_executable(test_string_ops tests/test_string_ops.c)
  target_link_libraries(test_string_ops PRIVATE x86port)
  x86p_strict_target(test_string_ops)
  add_test(NAME test_string_ops COMMAND test_string_ops)
endif()

add_executable(test_decode tests/test_decode.c)
target_link_libraries(test_decode PRIVATE x86port)
x86p_strict_target(test_decode)
add_test(NAME test_decode COMMAND test_decode)

add_executable(test_decode_cache tests/test_decode_cache.c)
target_link_libraries(test_decode_cache PRIVATE x86port_runtime x86port_test_oracle)
x86p_strict_target(test_decode_cache)
add_test(NAME test_decode_cache COMMAND test_decode_cache)

add_executable(test_flags tests/test_flags.c)
target_link_libraries(test_flags PRIVATE x86port)
x86p_strict_target(test_flags)
add_test(NAME test_flags COMMAND test_flags)

add_executable(test_alu tests/test_alu.c)
target_link_libraries(test_alu PRIVATE x86port)
x86p_strict_target(test_alu)
add_test(NAME test_alu COMMAND test_alu)

add_executable(test_cpu tests/test_cpu.c)
target_link_libraries(test_cpu PRIVATE x86port)
x86p_strict_target(test_cpu)
add_test(NAME test_cpu COMMAND test_cpu)

add_executable(test_cond tests/test_cond.c)
target_link_libraries(test_cond PRIVATE x86port)
x86p_strict_target(test_cond)
add_test(NAME test_cond COMMAND test_cond)

add_executable(test_exec tests/test_exec.c)
target_link_libraries(test_exec PRIVATE x86port_runtime x86port_test_oracle)
x86p_strict_target(test_exec)
add_test(NAME test_exec COMMAND test_exec)

add_executable(test_x87 tests/test_x87.c)
target_link_libraries(test_x87 PRIVATE x86port)
if(NOT WIN32)
  target_link_libraries(test_x87 PRIVATE m)
endif()
x86p_strict_target(test_x87)
add_test(NAME test_x87 COMMAND test_x87)

add_executable(test_x87_exec tests/test_x87_exec.c)
target_link_libraries(test_x87_exec PRIVATE x86port_runtime x86port_test_oracle)
if(NOT WIN32)
  target_link_libraries(test_x87_exec PRIVATE m)
endif()
x86p_strict_target(test_x87_exec)
add_test(NAME test_x87_exec COMMAND test_x87_exec)

# The encoder's oracle is Zydis itself, so this test links it directly rather
# than through x86port's PRIVATE dependency.
add_executable(test_emit_x64 tests/test_emit_x64.c)
target_link_libraries(test_emit_x64 PRIVATE x86port Zydis)
x86p_strict_target(test_emit_x64)
add_test(NAME test_emit_x64 COMMAND test_emit_x64)

# The differential: translated code against the interpreter, whole machine.
add_executable(test_jit_x64 tests/test_jit_x64.c)
target_link_libraries(test_jit_x64 PRIVATE x86port_runtime x86port_test_oracle)
x86p_strict_target(test_jit_x64)
add_test(NAME test_jit_x64 COMMAND test_jit_x64)
set_tests_properties(test_jit_x64 PROPERTIES SKIP_RETURN_CODE 77)

add_executable(test_jit_control tests/test_jit_control.c)
target_link_libraries(test_jit_control PRIVATE x86port_runtime x86port_test_oracle)
x86p_strict_target(test_jit_control)
add_test(NAME test_jit_control COMMAND test_jit_control)
set_tests_properties(test_jit_control PROPERTIES SKIP_RETURN_CODE 77)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
if(WIN32)
  # CMake may select dumpbin for an MSVC-compatible driver. This boundary uses
  # nm's portable archive interface, which LLVM provides for COFF too.
  find_program(X86P_SYMBOL_TOOL NAMES llvm-nm REQUIRED)
else()
  set(X86P_SYMBOL_TOOL "${CMAKE_NM}")
endif()
add_test(
  NAME verify_product_boundary
  COMMAND
    "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/verify_product_boundary.py"
    --nm "${X86P_SYMBOL_TOOL}"
    --product "$<TARGET_FILE:x86port_runtime>"
    --oracle "$<TARGET_FILE:x86port_test_oracle>")
add_test(
  NAME verify_product_boundary_rejects_oracle
  COMMAND
    "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/verify_product_boundary.py"
    --nm "${X86P_SYMBOL_TOOL}"
    --product "$<TARGET_FILE:x86port_test_oracle>"
    --oracle "$<TARGET_FILE:x86port_test_oracle>"
    --expect-product-leak)
add_test(
  NAME test_product_boundary_symbols
  COMMAND
    "${CMAKE_COMMAND}" -E env "PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}/tools"
    "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_product_boundary.py")
add_test(
  NAME verify_source_policy
  COMMAND
    "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/verify_source_policy.py"
    --root "${CMAKE_CURRENT_SOURCE_DIR}")
add_test(
  NAME verify_source_policy_negative
  COMMAND
    "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/verify_source_policy.py"
    --selftest)
set_tests_properties(
  verify_product_boundary
  verify_product_boundary_rejects_oracle
  test_product_boundary_symbols
  verify_source_policy
  verify_source_policy_negative
  PROPERTIES ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
