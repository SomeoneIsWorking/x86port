# Software x87 math from Bochs, pinned and compiled without its CPU dispatcher.
include(FetchContent)
FetchContent_Declare(x86p_bochs
  GIT_REPOSITORY https://github.com/bochs-emu/Bochs.git
  GIT_TAG 4ceffd60c1556f574fd0e35d2343ea579d8988ca)
FetchContent_MakeAvailable(x86p_bochs)
set(X86P_SOFTFLOAT_ROOT "${x86p_bochs_SOURCE_DIR}/bochs/cpu")
file(GLOB X86P_SOFTFLOAT_SOURCES CONFIGURE_DEPENDS
  "${X86P_SOFTFLOAT_ROOT}/softfloat3e/*.cc"
  "${X86P_SOFTFLOAT_ROOT}/softfloat3e/8086-SSE/*.cc")
add_library(x86p_softfloat STATIC ${X86P_SOFTFLOAT_SOURCES}
  "${X86P_SOFTFLOAT_ROOT}/fpu/fsincos.cc"
  "${X86P_SOFTFLOAT_ROOT}/fpu/poly.cc"
  "${X86P_SOFTFLOAT_ROOT}/fpu/f2xm1.cc"
  "${CMAKE_CURRENT_LIST_DIR}/../src/x86port/x87_softfloat_atan.cpp"
  "${X86P_SOFTFLOAT_ROOT}/fpu/fprem.cc"
  "${X86P_SOFTFLOAT_ROOT}/fpu/fyl2x.cc")
target_include_directories(x86p_softfloat PUBLIC
  "${CMAKE_CURRENT_LIST_DIR}/softfloat"
  "${X86P_SOFTFLOAT_ROOT}" "${X86P_SOFTFLOAT_ROOT}/fpu" "${X86P_SOFTFLOAT_ROOT}/softfloat3e/include")
target_compile_definitions(x86p_softfloat PRIVATE SOFTFLOAT_FAST_INT64 INLINE_LEVEL=5
  SOFTFLOAT_FAST_DIV32TO16 SOFTFLOAT_FAST_DIV64TO32)
set_target_properties(x86p_softfloat PROPERTIES POSITION_INDEPENDENT_CODE ON)
