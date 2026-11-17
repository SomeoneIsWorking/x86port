# x86port_symbol_constants(<target> IMAGES <images.toml> NAMESPACE <ns>)
#
# Defines the INTERFACE library <target>, whose include directory holds one
# `<stem>_symbols.hpp` per mapped image of a decomp's images.toml
# (tools/symbol_constants.py). The headers regenerate when the toml or a symbol
# file changes; a new image in the toml reconfigures.
set(X86P_SYMBOL_CONSTANTS_TOOL "${CMAKE_CURRENT_LIST_DIR}/../tools/symbol_constants.py")

function(x86port_symbol_constants target)
  cmake_parse_arguments(PARSE_ARGV 1 ARG "" "IMAGES;NAMESPACE" "")
  if(NOT ARG_IMAGES OR NOT ARG_NAMESPACE)
    message(FATAL_ERROR "x86port_symbol_constants(${target}): IMAGES and NAMESPACE are required")
  endif()
  find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
  execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${X86P_SYMBOL_CONSTANTS_TOOL}" --images "${ARG_IMAGES}" --list
    OUTPUT_VARIABLE listing ERROR_VARIABLE why RESULT_VARIABLE rc)
  if(rc)
    message(FATAL_ERROR "x86port_symbol_constants(${target}): ${why}")
  endif()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${ARG_IMAGES}")
  set(dir "${CMAKE_CURRENT_BINARY_DIR}/${target}/include")
  set(outputs)
  set(inputs "${ARG_IMAGES}" "${X86P_SYMBOL_CONSTANTS_TOOL}")
  string(REPLACE "\n" ";" lines "${listing}")
  foreach(line IN LISTS lines)
    if(line STREQUAL "")
      continue()
    endif()
    string(REPLACE "|" ";" line "${line}")
    list(GET line 0 header)
    list(GET line 1 symbols)
    list(APPEND outputs "${dir}/${header}")
    list(APPEND inputs "${symbols}")
  endforeach()
  # The tool rewrites only a changed header and the stamp marks the run, so
  # an edit to one symbol file recompiles only that header's users.
  set(stamp "${CMAKE_CURRENT_BINARY_DIR}/${target}/generated.stamp")
  add_custom_command(
    OUTPUT "${stamp}"
    BYPRODUCTS ${outputs}
    COMMAND "${Python3_EXECUTABLE}" "${X86P_SYMBOL_CONSTANTS_TOOL}" --images "${ARG_IMAGES}"
            --namespace "${ARG_NAMESPACE}" --output "${dir}"
    COMMAND "${CMAKE_COMMAND}" -E touch "${stamp}"
    DEPENDS ${inputs}
    COMMENT "Generating ${target} from ${ARG_IMAGES}"
    VERBATIM)
  add_custom_target(${target}_generate DEPENDS "${stamp}")
  add_library(${target} INTERFACE)
  target_include_directories(${target} INTERFACE "${dir}")
  add_dependencies(${target} ${target}_generate)
endfunction()
