# THE ANDROID x86-64 `long double`, ON A LINUX DESKTOP (issue: xmen2 #172).
#
# Bionic's x86-64 `long double` is IEEE binary128, not the ext80 object glibc
# and every other x86-64 host use, so the x64 backend's x87 slow paths meet a
# register layout and an operand width nothing else exercises. A mismatch there
# wedged a real title while every native gate stayed green. Clang's
# -mlong-double-128 gives this host that layout; glibc's `*l` libm entry points
# still take ext80, so the few this build calls are bound to glibc's _Float128
# implementations by a test-only shim linked into every executable.
#
# Test-only: nothing here changes a product build, and the option refuses any
# host where the flag or the _Float128 libm does not exist.
option(X86P_BINARY128_LONG_DOUBLE_MODEL
  "Build and test with Android x86-64's binary128 long double (Linux x86-64, Clang, glibc)" OFF)

if(X86P_BINARY128_LONG_DOUBLE_MODEL)
  if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$"
     OR NOT CMAKE_C_COMPILER_ID STREQUAL "Clang" OR NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR
      "X86P_BINARY128_LONG_DOUBLE_MODEL needs Linux x86-64 with Clang for C and C++ "
      "(-mlong-double-128) and glibc's _Float128 libm; this is ${CMAKE_SYSTEM_NAME} "
      "${CMAKE_SYSTEM_PROCESSOR} with ${CMAKE_C_COMPILER_ID}/${CMAKE_CXX_COMPILER_ID}.")
  endif()
  add_compile_options(-mlong-double-128)
  # OBJECT, not STATIC: an archive member is pulled only for a symbol still
  # undefined, and libm defines every one of these, so a static shim is
  # silently skipped wherever the compiler did not fold the call away.
  add_library(x86p_binary128_libm OBJECT tests/binary128_libm_shim.c)
  x86p_strict_target(x86p_binary128_libm)
  target_link_libraries(x86p_binary128_libm PUBLIC m)
endif()

# Called once every target exists: each of this project's executables resolves
# the `*l` calls in the shim. Executables only -- a library would carry the
# shim into the vendored projects' install exports.
function(x86p_binary128_model_link_executables)
  if(NOT X86P_BINARY128_LONG_DOUBLE_MODEL)
    return()
  endif()
  get_property(targets DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" PROPERTY BUILDSYSTEM_TARGETS)
  foreach(target IN LISTS targets)
    get_target_property(type ${target} TYPE)
    if(type STREQUAL "EXECUTABLE")
      target_link_libraries(${target} PRIVATE x86p_binary128_libm)
    endif()
  endforeach()
endfunction()
