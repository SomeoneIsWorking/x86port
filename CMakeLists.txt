cmake_minimum_required(VERSION 3.20)
project(x86port C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# The guest is x86-32 and this is a HOST library: nothing here is compiled for
# the guest architecture, so no -m32 anywhere. That is the whole point of a
# runtime engine.
add_library(x86port STATIC
  src/x86port/three_dnow.c)
target_include_directories(x86port PUBLIC src/x86port)
target_compile_options(x86port PRIVATE -Wall -Wextra -Werror)

enable_testing()
add_executable(test_three_dnow tests/test_three_dnow.c)
target_link_libraries(test_three_dnow PRIVATE x86port)
target_compile_options(test_three_dnow PRIVATE -Wall -Wextra -Werror)
add_test(NAME test_three_dnow COMMAND test_three_dnow)
