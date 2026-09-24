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
  # The Microsoft CRT marks ISO C functions such as fopen, getenv and sscanf
  # deprecated in favour of its non-portable *_s forms; under /WX that makes
  # standard C an error. The portable functions are the ones used here.
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
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
if(EMSCRIPTEN)
  set(ZYAN_NO_LIBC ON CACHE BOOL "" FORCE)
endif()
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

# The x64, ARM64 and WebAssembly backend modules implement the
# same externally-visible x86p_jit_* contract declared once in jit_x64.h. At
# most one backend can ever be linked into x86port_runtime, or their identically
# named symbols collide -- so the backend is chosen once, here, by host
# architecture. emit_x64.c/emit_arm64.c/emit_wasm.c have no such clash
# (differently prefixed symbols) and stay unconditionally built for their own
# dedicated encoder tests -- which also means every host compiles all three, so
# an encoder cannot rot on a platform nobody builds it for.
#
# The WebAssembly backend is split further, and for the same reason: only
# jit_wasm.c assumes the host it runs on is the host it emits for, so it is the
# only conditional part. The lowering (jit_wasm_lower.c and its per-family
# units), the module builder and the module-lifetime arena are x86p_wasm_*
# symbols with no clash, built everywhere, and tested everywhere -- a module
# can be handed to any engine, so the differential against the interpreter runs
# on a developer machine with no Emscripten toolchain at all.
#
# The selection NAMES an unsupported host rather than assuming one. It used to
# be a two-way branch whose else-arm meant "x86-64", so any host that was not
# ARM64 got the x86-64 emitter -- including Emscripten, which reports its
# processor as `x86` and cannot execute emitted machine code at all. That
# configure succeeded, that link succeeded, and the product would have died on
# its first translated block with nothing in the build having said a word.
set(X86P_JIT_STORAGE_SOURCES src/x86port/jit_storage_native.c)
set(X86P_JIT_BACKEND_SOURCES "")
if(EMSCRIPTEN)
  # Asked BEFORE the processor is looked at, because Emscripten reports its
  # processor as `x86` -- which is how it once got the x86-64 emitter.
  set(X86P_JIT_BACKEND_SOURCES
    src/x86port/jit_wasm.c
    src/x86port/jit_wasm_host.c)
  set(X86P_JIT_STORAGE_SOURCES src/x86port/jit_storage_wasm.c)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64|ARM64)$")
  set(X86P_JIT_BACKEND_SOURCES
    src/x86port/jit_arm64.c
    src/x86port/jit_arm64_cond.c
    src/x86port/jit_arm64_integer.c
    src/x86port/jit_arm64_alu.c
    src/x86port/jit_arm64_branch.c
    src/x86port/jit_arm64_simd.c
    src/x86port/jit_arm64_x87_register.c
    src/x86port/jit_arm64_x87.c)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64|x64)$")
  set(X86P_JIT_BACKEND_SOURCES
    src/x86port/jit_x64.c
    src/x86port/jit_x64_mem.c
    src/x86port/jit_x64_cond.c
    src/x86port/jit_x64_alu.c
    src/x86port/jit_x64_gpr.c
    src/x86port/jit_x64_branch.c
    src/x86port/jit_x64_simd.c
    src/x86port/jit_x64_x87_register.c
    src/x86port/jit_x64_x87.c
    src/x86port/jit_x64_x87_inline.c)
endif()

# A host with no backend is a refusal, not a default. The one exception is
# explicit and named: a portability MEASUREMENT wants to compile everything
# that can be compiled for a host whose backend does not exist yet, and it says
# so on the command line. It still gets told, every configure, that the
# resulting library cannot execute a guest instruction.
if(X86P_JIT_BACKEND_SOURCES STREQUAL "")
  string(CONCAT X86P_NO_BACKEND_REASON
    "no JIT backend exists for CMAKE_SYSTEM_PROCESSOR='${CMAKE_SYSTEM_PROCESSOR}'. "
    "The supported hosts are arm64/aarch64, x86_64/amd64, and Emscripten (WebAssembly).")
  if(X86P_MEASURE_UNRUNNABLE_BACKEND)
    string(CONCAT X86P_NO_BACKEND_WARNING
      "x86port: ${X86P_NO_BACKEND_REASON} "
      "X86P_MEASURE_UNRUNNABLE_BACKEND is set, so the x86-64 emitter is linked to let a portability "
      "measurement compile the rest. THE RESULTING LIBRARY CANNOT EXECUTE A GUEST INSTRUCTION ON THIS "
      "HOST. Never set this for anything a person runs.")
    message(WARNING "${X86P_NO_BACKEND_WARNING}")
    set(X86P_JIT_BACKEND_SOURCES
      src/x86port/jit_x64.c
      src/x86port/jit_x64_mem.c
      src/x86port/jit_x64_cond.c
      src/x86port/jit_x64_alu.c
      src/x86port/jit_x64_gpr.c
      src/x86port/jit_x64_branch.c
      src/x86port/jit_x64_simd.c
      src/x86port/jit_x64_x87_register.c
      src/x86port/jit_x64_x87.c
      src/x86port/jit_x64_x87_inline.c)
  else()
    string(CONCAT X86P_NO_BACKEND_REFUSAL
      "x86port: ${X86P_NO_BACKEND_REASON} "
      "Configuring anyway would link an emitter this host cannot run and fail at the first translated "
      "block, so this refuses instead. A portability measurement may pass "
      "-DX86P_MEASURE_UNRUNNABLE_BACKEND=ON, which produces a library that cannot execute guest code.")
    message(FATAL_ERROR "${X86P_NO_BACKEND_REFUSAL}")
  endif()
endif()

include(cmake/softfloat.cmake)

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
  src/x86port/simd_packed.c
  src/x86port/three_dnow.c
  src/x86port/decode.c
  src/x86port/flags.c
  src/x86port/alu.c
  src/x86port/cpu.c
  src/x86port/memory.c
  src/x86port/memory_sparse.c
  src/x86port/cond.c
  src/x86port/x87.c
  src/x86port/x87_binary128.c
  src/x86port/x87_double_arith.c
  src/x86port/x87_double_fn.c
  src/x86port/x87_ext80_arith.c
  src/x86port/x87_op_census.c
  src/x86port/x87_stack.c
  src/x86port/x87_ext80_narrow.c
  src/x86port/x87_ext80_widen.c
  src/x86port/x87_transcendental.c
  src/x86port/jit_x87_predicates.c
  src/x86port/x87_softfloat.cpp
  src/x86port/x87_fn_stack.c
  src/x86port/x87_integer.c
  src/x86port/x87_register.c
  src/x86port/emit_x64.c
  src/x86port/emit_arm64.c
  src/x86port/emit_wasm.c
  src/x86port/jit_status.c
  src/x86port/jit_wasm_imports.c
  src/x86port/jit_wasm_bitops.c
  src/x86port/jit_wasm_control.c
  src/x86port/jit_wasm_cond.c
  src/x86port/jit_wasm_x87.c
  src/x86port/jit_wasm_x87_arith.c
  src/x86port/jit_wasm_x87_load.c
  src/x86port/jit_wasm_x87_store.c
  src/x86port/jit_wasm_simd.c
  src/x86port/jit_wasm_simd_inline.c
  src/x86port/x87_memory.c
  src/x86port/stack_ops.c
  src/x86port/jit_wasm_module.c
  src/x86port/jit_wasm_memory.c
  src/x86port/jit_wasm_integer.c
  src/x86port/jit_wasm_chain.c
  src/x86port/jit_wasm_state.c
  src/x86port/jit_wasm_lower.c
  src/x86port/jit_wasm_alu.c
  src/x86port/jit_wasm_move.c
  src/x86port/jit_wasm_stack.c
  src/x86port/jit_wasm_branch.c
  src/x86port/jit_wasm_arena.c
  src/x86port/jit_wasm_compact.c
  ${X86P_JIT_BACKEND_SOURCES}
  ${X86P_JIT_STORAGE_SOURCES}
  src/x86port/jit_chain.c
  src/x86port/jit_leaf_sites.c
  src/x86port/jit_chain_census.c
  src/x86port/jit_profile.c
  src/x86port/jit_engine.c)
target_link_libraries(x86port_runtime PRIVATE x86p_softfloat)
add_library(x86port ALIAS x86port_runtime)
target_include_directories(x86port_runtime PUBLIC src/x86port)
if(EMSCRIPTEN)
  target_link_libraries(x86port_runtime PUBLIC jitcommon_cache PRIVATE Zydis)
  target_link_options(x86port_runtime INTERFACE "-sALLOW_TABLE_GROWTH=1")
else()
  target_link_libraries(x86port_runtime PUBLIC jitcommon PRIVATE Zydis)
endif()
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

if(EMSCRIPTEN)
  # These are bounded command-line tests/tools: returning from main must
  # dispose worker threads and report the result to CTest.
  add_link_options("-sEXIT_RUNTIME=1")
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

add_executable(test_memory_sparse tests/test_memory_sparse.c)
target_link_libraries(test_memory_sparse PRIVATE x86port_runtime)
x86p_strict_target(test_memory_sparse)
add_test(NAME test_memory_sparse COMMAND test_memory_sparse)

add_executable(test_memory_perms tests/test_memory_perms.c)
target_link_libraries(test_memory_perms PRIVATE x86port_runtime)
x86p_strict_target(test_memory_perms)
add_test(NAME test_memory_perms COMMAND test_memory_perms)

add_executable(test_x87_state tests/test_x87_state.c)
target_link_libraries(test_x87_state PRIVATE x86port_runtime)
x86p_strict_target(test_x87_state)
add_test(NAME test_x87_state COMMAND test_x87_state)

add_executable(test_x87_memory tests/test_x87_memory.c)
target_link_libraries(test_x87_memory PRIVATE x86port_runtime)
x86p_strict_target(test_x87_memory)
add_test(NAME test_x87_memory COMMAND test_x87_memory)

if(NOT EMSCRIPTEN)
  # Probes native executable-memory allocation and protection transitions.
  add_executable(test_jit_engine tests/test_jit_engine.c)
  target_link_libraries(test_jit_engine PRIVATE x86port_runtime x86port_test_oracle)
  target_include_directories(test_jit_engine PRIVATE src)
  x86p_strict_target(test_jit_engine)
  add_test(NAME test_jit_engine COMMAND test_jit_engine)
endif()

add_executable(test_jit_chain_census tests/test_jit_chain_census.c)
target_link_libraries(test_jit_chain_census PRIVATE x86port)
target_include_directories(test_jit_chain_census PRIVATE src/x86port)
x86p_strict_target(test_jit_chain_census)
add_test(NAME test_jit_chain_census COMMAND test_jit_chain_census)

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

if(EMSCRIPTEN)
  add_executable(test_wasm_runtime tests/test_wasm_runtime.c)
  target_link_libraries(test_wasm_runtime PRIVATE x86port_runtime)
  # The lifetime test holds a 16 MiB translation arena beside the program and
  # a second engine, deliberately exercising growth of the imported memory.
  target_link_options(test_wasm_runtime PRIVATE "-sALLOW_MEMORY_GROWTH=1")
  x86p_strict_target(test_wasm_runtime)
  add_test(NAME test_wasm_runtime COMMAND test_wasm_runtime)
  add_executable(test_wasm_threads tests/test_wasm_threads.c)
  target_link_libraries(test_wasm_threads PRIVATE x86port_runtime)
  target_compile_options(test_wasm_threads PRIVATE -pthread)
  target_link_options(test_wasm_threads PRIVATE -pthread "-sPROXY_TO_PTHREAD=1" "-sPTHREAD_POOL_SIZE=2")
  x86p_strict_target(test_wasm_threads)
  add_test(NAME test_wasm_threads COMMAND test_wasm_threads)
  add_executable(test_wasm_sparse tests/test_wasm_sparse.c)
  target_link_libraries(test_wasm_sparse PRIVATE x86port_runtime)
  x86p_strict_target(test_wasm_sparse)
  add_test(NAME test_wasm_sparse COMMAND test_wasm_sparse)
  add_executable(test_wasm_perms tests/test_wasm_perms.c)
  target_link_libraries(test_wasm_perms PRIVATE x86port_runtime)
  x86p_strict_target(test_wasm_perms)
  add_test(NAME test_wasm_perms COMMAND test_wasm_perms)
  add_executable(test_wasm_integer tests/test_wasm_integer.c tests/wasm_differential.c)
  target_link_libraries(test_wasm_integer PRIVATE x86port_runtime x86port_test_oracle)
  x86p_strict_target(test_wasm_integer)
  add_test(NAME test_wasm_integer COMMAND test_wasm_integer)
  foreach(family x87 simd integer_tail cond exits)
    add_executable(test_wasm_${family} tests/test_wasm_${family}.c)
    target_link_libraries(test_wasm_${family} PRIVATE x86port_runtime x86port_test_oracle)
    x86p_strict_target(test_wasm_${family})
    add_test(NAME test_wasm_${family} COMMAND test_wasm_${family})
  endforeach()
  target_sources(test_wasm_integer_tail PRIVATE tests/wasm_differential.c)
  target_sources(test_wasm_simd PRIVATE tests/wasm_differential.c)
  target_sources(test_wasm_cond PRIVATE tests/wasm_differential.c)
endif()
add_executable(test_three_dnow tests/test_three_dnow.c)
target_link_libraries(test_three_dnow PRIVATE x86port)
x86p_strict_target(test_three_dnow)
add_test(NAME test_three_dnow COMMAND test_three_dnow)

if(NOT WIN32)
  # These suites execute host instructions from POSIX mappings. Windows still
  # builds and exercises the shipping JIT and the test-only interpreter below;
  # the silicon-oracle suites require x86-64 hardware on Linux/macOS. Other
  # architectures report an explicit skip; their JIT tests remain mandatory.
  add_executable(test_integer_tail tests/test_integer_tail.c)
  target_link_libraries(test_integer_tail PRIVATE x86port_runtime x86port_test_oracle)
  x86p_strict_target(test_integer_tail)
  add_test(NAME test_integer_tail COMMAND test_integer_tail)

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

  set_tests_properties(test_integer_tail test_x87_fn test_simd test_string_ops
    PROPERTIES SKIP_RETURN_CODE 77)

  foreach(oracle IN ITEMS test_integer_tail test_x87_fn test_simd test_string_ops)
    # The compiled fixture proves the exit; also require CTest to classify that
    # exit as skipped for the real suite rather than as success or failure.
    get_test_property(${oracle} SKIP_RETURN_CODE oracle_skip_code)
    if(NOT oracle_skip_code STREQUAL "77")
      message(FATAL_ERROR "${oracle} must classify unavailable hardware as skip 77")
    endif()
    add_executable(${oracle}_unavailable tests/test_oracle_unavailable.c)
    target_compile_definitions(${oracle}_unavailable PRIVATE
      X86P_ORACLE_SOURCE="${oracle}.c")
    target_link_libraries(${oracle}_unavailable PRIVATE x86port_runtime)
    x86p_strict_target(${oracle}_unavailable)
    add_test(NAME ${oracle}_unavailable COMMAND ${oracle}_unavailable)
  endforeach()
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

add_executable(test_string_copy tests/test_string_copy.c)
target_link_libraries(test_string_copy PRIVATE x86port_runtime x86port_test_oracle)
x86p_strict_target(test_string_copy)
add_test(NAME test_string_copy COMMAND test_string_copy)

if(NOT EMSCRIPTEN)
  # This test probes native host fenv exception flags. WebAssembly has no
  # FE_DIVBYZERO host flag; test_wasm_x87 covers its software x87 path.
  add_executable(test_x87_narrow tests/test_x87_narrow.c)
  target_link_libraries(test_x87_narrow PRIVATE x86port_runtime)
  x86p_strict_target(test_x87_narrow)
  add_test(NAME test_x87_narrow COMMAND test_x87_narrow)
endif()

add_executable(test_x87_double_arith tests/test_x87_double_arith.c)
target_link_libraries(test_x87_double_arith PRIVATE x86port)
x86p_strict_target(test_x87_double_arith)
add_test(NAME test_x87_double_arith COMMAND test_x87_double_arith)

add_executable(test_x87_ext80_arith tests/test_x87_ext80_arith.cpp)
target_link_libraries(test_x87_ext80_arith PRIVATE x86port)
x86p_strict_target(test_x87_ext80_arith)
add_test(NAME test_x87_ext80_arith COMMAND test_x87_ext80_arith)

add_executable(test_x87_ext80_narrow tests/test_x87_ext80_narrow.cpp)
target_link_libraries(test_x87_ext80_narrow PRIVATE x86port)
x86p_strict_target(test_x87_ext80_narrow)
add_test(NAME test_x87_ext80_narrow COMMAND test_x87_ext80_narrow)

add_executable(test_x87_ext80_widen tests/test_x87_ext80_widen.cpp)
target_link_libraries(test_x87_ext80_widen PRIVATE x86port)
x86p_strict_target(test_x87_ext80_widen)
add_test(NAME test_x87_ext80_widen COMMAND test_x87_ext80_widen)

add_executable(test_x87_binary128 tests/test_x87_binary128.c)
target_link_libraries(test_x87_binary128 PRIVATE x86port)
if(NOT WIN32)
  target_link_libraries(test_x87_binary128 PRIVATE m)
endif()
x86p_strict_target(test_x87_binary128)
add_test(NAME test_x87_binary128 COMMAND test_x87_binary128)

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

# No Zydis-grade decode oracle exists for AArch64 here, so this test decodes
# with its own independently-written bitfield extractor instead (see the file
# header). It needs only the encoder, so it links x86port_runtime directly
# rather than the alias that pulls in Zydis.
add_executable(test_emit_arm64 tests/test_emit_arm64.c)
target_link_libraries(test_emit_arm64 PRIVATE x86port_runtime)
x86p_strict_target(test_emit_arm64)
add_test(NAME test_emit_arm64 COMMAND test_emit_arm64)

# The wasm encoder's oracle is a real WebAssembly engine rather than a
# disassembler: it validates each emitted module against the specification and
# runs it. Without node the test SKIPs (77) instead of passing, because a run
# that instantiated nothing has established nothing about the encoding.
add_executable(test_emit_wasm tests/test_emit_wasm.c)
target_link_libraries(test_emit_wasm PRIVATE x86port_runtime)
x86p_strict_target(test_emit_wasm)
add_test(NAME test_emit_wasm COMMAND test_emit_wasm)
set_tests_properties(test_emit_wasm PROPERTIES
  SKIP_RETURN_CODE 77
  WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
  ENVIRONMENT "X86P_WASM_ORACLE=${CMAKE_CURRENT_SOURCE_DIR}/tests/wasm_oracle.mjs")

# The WebAssembly lowering, differentially against the interpreter oracle in a
# real engine. It links the lowering (which builds on every host) rather than a
# backend, so it runs on a developer machine with no Emscripten toolchain --
# a module can be handed to any engine. Without node it SKIPs (77): a run that
# instantiated nothing has established nothing about the lowering.
add_executable(test_jit_wasm tests/test_jit_wasm.c)
target_link_libraries(test_jit_wasm PRIVATE x86port_runtime x86port_test_oracle)
x86p_strict_target(test_jit_wasm)
add_test(NAME test_jit_wasm COMMAND test_jit_wasm)
set_tests_properties(test_jit_wasm PROPERTIES
  SKIP_RETURN_CODE 77
  WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
  ENVIRONMENT "X86P_WASM_JIT_ORACLE=${CMAKE_CURRENT_SOURCE_DIR}/tests/wasm_jit_oracle.mjs")

# Module lifetime: the accounting an engine with no unload makes necessary.
# A stub host stands in for the engine, which is the point -- the bugs are in
# the accounting, and the accounting has nothing to do with wasm.
# Making one module out of many published blocks. Like the arena test, the
# engine is a stub -- what is being tested is the operation's ALL-OR-NOTHING
# behaviour, which is exactly what a real engine would make hard to provoke.
add_executable(test_jit_wasm_compact tests/test_jit_wasm_compact.c)
target_link_libraries(test_jit_wasm_compact PRIVATE x86port_runtime)
x86p_strict_target(test_jit_wasm_compact)
add_test(NAME test_jit_wasm_compact COMMAND test_jit_wasm_compact)

# The WebAssembly storage's own bookkeeping. The shipping source is compiled in
# rather than linked from the runtime library, because that library carries the
# NATIVE storage on a native host -- and the bug this test exists for is in how
# the wasm one counts block records against modules, which no native build
# would ever run.
add_executable(test_jit_storage_wasm tests/test_jit_storage_wasm.c src/x86port/jit_storage_wasm.c)
target_link_libraries(test_jit_storage_wasm PRIVATE x86port_runtime)
x86p_strict_target(test_jit_storage_wasm)
add_test(NAME test_jit_storage_wasm COMMAND test_jit_storage_wasm)

add_executable(test_jit_wasm_arena tests/test_jit_wasm_arena.c)
target_link_libraries(test_jit_wasm_arena PRIVATE x86port_runtime)
x86p_strict_target(test_jit_wasm_arena)
add_test(NAME test_jit_wasm_arena COMMAND test_jit_wasm_arena)

if(NOT EMSCRIPTEN)
  # Native executable-memory differential; WASM has its own differential tests.
  add_executable(test_jit_x64 tests/test_jit_x64.c tests/jit_x64_harness.c)
  target_link_libraries(test_jit_x64 PRIVATE x86port_runtime x86port_test_oracle)
  x86p_strict_target(test_jit_x64)
  add_test(NAME test_jit_x64 COMMAND test_jit_x64)
  set_tests_properties(test_jit_x64 PROPERTIES SKIP_RETURN_CODE 77)
  add_executable(test_jit_x64_x87 tests/test_jit_x64_x87.c tests/jit_x64_harness.c)
  target_link_libraries(test_jit_x64_x87 PRIVATE x86port_runtime x86port_test_oracle)
  x86p_strict_target(test_jit_x64_x87)
  add_test(NAME test_jit_x64_x87 COMMAND test_jit_x64_x87)
  set_tests_properties(test_jit_x64_x87 PROPERTIES SKIP_RETURN_CODE 77)
  # The same differentials over the CHECKED guest register cache
  # (jit_x64_gpr.h): the executable's own jit_x64_gpr.c, compiled to trap
  # wherever a cached host register disagrees with X86pCpu, takes the place of
  # the archive's copy, which then is never pulled in.
  foreach(differential test_jit_x64 test_jit_x64_x87 test_jit_engine)
    add_executable(${differential}_gpr_checked tests/${differential}.c src/x86port/jit_x64_gpr.c)
    if(NOT differential STREQUAL "test_jit_engine")
      target_sources(${differential}_gpr_checked PRIVATE tests/jit_x64_harness.c)
    endif()
    target_include_directories(${differential}_gpr_checked PRIVATE src)
    target_compile_definitions(${differential}_gpr_checked PRIVATE X86P_JIT_GPR_CHECK=1)
    target_link_libraries(${differential}_gpr_checked PRIVATE x86port_runtime x86port_test_oracle)
    x86p_strict_target(${differential}_gpr_checked)
    add_test(NAME ${differential}_gpr_checked COMMAND ${differential}_gpr_checked)
    set_tests_properties(${differential}_gpr_checked PROPERTIES SKIP_RETURN_CODE 77)
  endforeach()
endif()

add_executable(test_x87_f128_ext80 tests/test_x87_f128_ext80.cpp)
target_link_libraries(test_x87_f128_ext80 PRIVATE x86p_softfloat)
x86p_strict_target(test_x87_f128_ext80)
add_test(NAME test_x87_f128_ext80 COMMAND test_x87_f128_ext80)

add_executable(test_x87_software tests/test_x87_software.c)
target_link_libraries(test_x87_software PRIVATE x86port_runtime)
x86p_strict_target(test_x87_software)
add_test(NAME test_x87_software COMMAND test_x87_software)

# A measurement, not a gate: it reports a ratio of two wall-clock times, which
# is not a property a test can assert. Deliberately absent from add_test.
add_executable(bench_x87_arith tests/bench_x87_arith.cpp)
target_link_libraries(bench_x87_arith PRIVATE x86port_runtime x86p_softfloat)
x86p_strict_target(bench_x87_arith)

if(NOT EMSCRIPTEN)
  # Exercises a native callee-saved host register; the WASM ABI has no r12.
  add_executable(test_jit_startup tests/test_jit_startup.c)
  target_link_libraries(test_jit_startup PRIVATE x86port_runtime x86port_test_oracle)
  x86p_strict_target(test_jit_startup)
  add_test(NAME test_jit_startup COMMAND test_jit_startup)
  set_tests_properties(test_jit_startup PROPERTIES SKIP_RETURN_CODE 77)
endif()

if(NOT EMSCRIPTEN)
  # Uses native code-region publication, absent from WASM storage.
  add_executable(test_jit_control tests/test_jit_control.c)
  target_link_libraries(test_jit_control PRIVATE x86port_runtime x86port_test_oracle)
  x86p_strict_target(test_jit_control)
  add_test(NAME test_jit_control COMMAND test_jit_control)
  set_tests_properties(test_jit_control PROPERTIES SKIP_RETURN_CODE 77)
endif()

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
# CLAUDE.md requires the normal verifier to include a format check and it had
# none, so drift was invisible: a ten-file backend landed with seven files off
# the tracked style while the other 126 matched it exactly. This SKIPs (77)
# rather than passing when no clang-format is present, and its negative control
# proves both that the comparison fires and that the file list contains sources
# that have never been committed -- enumerating the index alone made this check
# blind to new files, which is the one case where its verdict is being read.
add_test(
  NAME verify_format
  COMMAND
    "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/verify_format.py"
    --root "${CMAKE_CURRENT_SOURCE_DIR}")
set_tests_properties(verify_format PROPERTIES SKIP_RETURN_CODE 77)
add_test(
  NAME verify_format_negative
  COMMAND
    "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/verify_format.py"
    --selftest "${CMAKE_CURRENT_SOURCE_DIR}")
set_tests_properties(verify_format_negative PROPERTIES SKIP_RETURN_CODE 77)
set_tests_properties(
  verify_product_boundary
  verify_product_boundary_rejects_oracle
  test_product_boundary_symbols
  verify_source_policy
  verify_source_policy_negative
  verify_format
  verify_format_negative
  PROPERTIES ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
