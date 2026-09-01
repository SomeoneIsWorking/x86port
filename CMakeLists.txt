cmake_minimum_required(VERSION 3.20)
project(x86port C)

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

add_library(x86port STATIC
  src/x86port/three_dnow.c
  src/x86port/decode.c
  src/x86port/engine.c
  src/x86port/flags.c
  src/x86port/alu.c
  src/x86port/cpu.c
  src/x86port/cond.c)
target_include_directories(x86port PUBLIC src/x86port)
target_link_libraries(x86port PRIVATE Zydis)
target_compile_options(x86port PRIVATE -Wall -Wextra -Werror)

# The differential instrument. Not a test: it needs a title's Ghidra export,
# which is not in this repo and never will be.
add_executable(x86p_decode_diff tools/decode_diff.c)
target_link_libraries(x86p_decode_diff PRIVATE x86port)
target_compile_options(x86p_decode_diff PRIVATE -Wall -Wextra -Werror)

enable_testing()
add_executable(test_three_dnow tests/test_three_dnow.c)
target_link_libraries(test_three_dnow PRIVATE x86port)
target_compile_options(test_three_dnow PRIVATE -Wall -Wextra -Werror)
add_test(NAME test_three_dnow COMMAND test_three_dnow)

add_executable(test_decode tests/test_decode.c)
target_link_libraries(test_decode PRIVATE x86port)
target_compile_options(test_decode PRIVATE -Wall -Wextra -Werror)
add_test(NAME test_decode COMMAND test_decode)

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
