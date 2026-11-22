# The clang build (NFSC_COMPILER=clang): lld linker script, ThinLTO and profile-guided optimisation.
# Sets NFSC_CLANG_FLAGS (runtime, main), NFSC_CLANG_GAME_FLAGS (generated code), NFSC_CLANG_LIGHT_FLAGS (nfsc.cpp),
# NFSC_CLANG_SOURCES, NFSC_CLANG_LINK_FLAGS and NFSC_LLD_SCRIPT.
#   NFSC_LTO=ON            ThinLTO over the generated code, the runtime and main.cpp (default)
#   NFSC_PGO_GENERATE=ON   instrumented build: the game writes ux0:data/NFSCVITA/nfsc.profraw
#   NFSC_PGO_USE=<file>    optimise with an indexed profile (llvm-profdata merge)

option(NFSC_LTO "ThinLTO (clang)" ON)
set(NFSC_PGO_LIST "" CACHE FILEPATH "Optional PGO instrumentation selection")

include(cmake/vita-lld.cmake)
nfsc_lld_script(NFSC_LLD_SCRIPT)

set(NFSC_CLANG_LINK_FLAGS "-Wl,-T,${NFSC_LLD_SCRIPT}")

# Generated code: warnings off.
set(NFSC_CLANG_GAME_FLAGS -Wno-unused-but-set-variable -Wno-unused-function -Wno-parentheses-equality -Wno-self-assign)
set(NFSC_CLANG_FLAGS "")
set(NFSC_CLANG_LIGHT_FLAGS "")
set(NFSC_CLANG_SOURCES "")
set(NFSC_CLANG_LIBRARIES "")

if(NFSC_LTO)
  list(APPEND NFSC_CLANG_FLAGS -flto=thin)
  list(APPEND NFSC_CLANG_GAME_FLAGS -flto=thin)
  # nfsc.cpp is compiled to native code.
  list(APPEND NFSC_CLANG_LIGHT_FLAGS -fno-lto)
  cmake_host_system_information(RESULT NFSC_LTO_JOBS QUERY NUMBER_OF_LOGICAL_CORES)
  # Link-time backends: -O2, one job per core, a cache in the build directory, no SLP vectorisation.
  list(APPEND NFSC_CLANG_LINK_FLAGS -flto=thin -Wl,--lto-O2 -Wl,--thinlto-jobs=${NFSC_LTO_JOBS}
       "-Wl,--thinlto-cache-dir=${CMAKE_BINARY_DIR}/thinlto-cache" -Wl,-mllvm,-vectorize-slp=false)
endif()

if(NFSC_PGO_GENERATE)
  include(cmake/pgo-runtime.cmake)
  nfsc_pgo_runtime(NFSC_PGO_RUNTIME)
  # IR-level instrumentation with value profiles.
  set(NFSC_PGO_FLAGS -fprofile-generate)
  if(NFSC_PGO_LIST)
    if(NOT EXISTS "${NFSC_PGO_LIST}")
      message(FATAL_ERROR "NFSC_PGO_LIST: ${NFSC_PGO_LIST} not found")
    endif()
    list(APPEND NFSC_PGO_FLAGS "-fprofile-list=${NFSC_PGO_LIST}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${NFSC_PGO_LIST}")
  endif()
  list(APPEND NFSC_CLANG_FLAGS ${NFSC_PGO_FLAGS})
  list(APPEND NFSC_CLANG_GAME_FLAGS ${NFSC_PGO_FLAGS})
  list(APPEND NFSC_RUNTIME_DEFINES NFSC_PGO_GENERATE)
  list(APPEND NFSC_CLANG_SOURCES src/platform/pgo.cpp)
  # The profile runtime library (cmake/pgo-runtime.cmake).
  set(NFSC_CLANG_LIBRARIES ${NFSC_PGO_RUNTIME})
elseif(NFSC_PGO_USE)
  if(NOT EXISTS "${NFSC_PGO_USE}")
    message(FATAL_ERROR "NFSC_PGO_USE: ${NFSC_PGO_USE} not found")
  endif()
  # Profile mismatch warnings off.
  set(NFSC_PGO_FLAGS "-fprofile-use=${NFSC_PGO_USE}" -Wno-backend-plugin)
  list(APPEND NFSC_CLANG_FLAGS ${NFSC_PGO_FLAGS})
  list(APPEND NFSC_CLANG_GAME_FLAGS ${NFSC_PGO_FLAGS})
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${NFSC_PGO_USE}")
endif()

message(STATUS "clang build: ThinLTO ${NFSC_LTO}, PGO generate ${NFSC_PGO_GENERATE}, PGO use '${NFSC_PGO_USE}'")
