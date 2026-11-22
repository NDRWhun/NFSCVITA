# Profile-guided optimisation of the gcc build. Sets NFSC_GCC_FLAGS (runtime, main), NFSC_GCC_GAME_FLAGS (generated
# code), NFSC_GCC_SOURCES and NFSC_GCC_LINK_FLAGS.
#   NFSC_PGO_GENERATE=ON   instrumented build (edge counters): the game writes ux0:data/NFSCVITA/nfsc.gcov
#   NFSC_PGO_USE=<files>   optimise with one or more nfsc.gcov files; gcov-tool merges them into the .gcda files of
#                          the build folder at configure time

set(NFSC_GCC_FLAGS "")
set(NFSC_GCC_GAME_FLAGS "")
set(NFSC_GCC_SOURCES "")
set(NFSC_GCC_LINK_FLAGS "")

if(NFSC_PGO_GENERATE)
  # VitaSDK's libgcov has no value profilers and no file output: edge counters, registered in the gcov_info section
  # and written by src/platform/pgo_gcov.cpp.
  set(NFSC_PGO_FLAGS -fprofile-generate -fno-profile-values -fprofile-update=atomic -fprofile-info-section=gcov_info)
  set(NFSC_GCC_FLAGS ${NFSC_PGO_FLAGS})
  set(NFSC_GCC_GAME_FLAGS ${NFSC_PGO_FLAGS})
  set(NFSC_GCC_LINK_FLAGS -fprofile-generate)
  list(APPEND NFSC_RUNTIME_DEFINES NFSC_PGO_GENERATE NFSC_PGO_GCOV)
  set(NFSC_GCC_SOURCES src/platform/pgo.cpp src/platform/pgo_gcov.cpp)
  set_property(SOURCE src/platform/pgo_gcov.cpp APPEND PROPERTY COMPILE_DEFINITIONS
               "NFSC_PGO_BUILD_DIR=\"${CMAKE_BINARY_DIR}\"")
elseif(NFSC_PGO_USE)
  get_filename_component(NFSC_GCC_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
  find_program(NFSC_GCOV_TOOL arm-vita-eabi-gcov-tool HINTS "${NFSC_GCC_BIN}" REQUIRED)
  # Merges the profiles again when the list or a file changed.
  set(stamp "")
  foreach(profile IN LISTS NFSC_PGO_USE)
    if(NOT EXISTS "${profile}")
      message(FATAL_ERROR "NFSC_PGO_USE: ${profile} not found")
    endif()
    file(SHA256 "${profile}" hash)
    string(APPEND stamp "${profile} ${hash}\n")
  endforeach()
  set(stampFile "${CMAKE_BINARY_DIR}/pgo-profiles.txt")
  set(previous "")
  if(EXISTS "${stampFile}")
    file(READ "${stampFile}" previous)
  endif()
  if(NOT previous STREQUAL stamp)
    file(GLOB_RECURSE stale "${CMAKE_BINARY_DIR}/CMakeFiles/*.gcda")
    if(stale)
      file(REMOVE ${stale})
    endif()
    foreach(profile IN LISTS NFSC_PGO_USE)
      message(STATUS "PGO: merging ${profile}")
      execute_process(COMMAND "${NFSC_GCOV_TOOL}" merge-stream "${profile}"
                      WORKING_DIRECTORY "${CMAKE_BINARY_DIR}" RESULT_VARIABLE result)
      if(NOT result EQUAL 0)
        file(REMOVE "${stampFile}")
        message(FATAL_ERROR "gcov-tool merge-stream ${profile} failed (${result})")
      endif()
    endforeach()
    file(WRITE "${stampFile}" "${stamp}")
  endif()
  # Functions without counts (not run while profiling) are optimised as without a profile; a source changed since the
  # profile run loses the profile of the changed functions.
  set(NFSC_PGO_FLAGS -fprofile-use -fno-profile-values -fprofile-partial-training -Wno-missing-profile
      -Wno-error=coverage-mismatch)
  set(NFSC_GCC_FLAGS ${NFSC_PGO_FLAGS})
  set(NFSC_GCC_GAME_FLAGS ${NFSC_PGO_FLAGS})
  # Sources compiled with the profile are rebuilt when it changes.
  set_property(SOURCE ${NFSC_GENERATED_SOURCES} ${NFSC_RUNTIME_SOURCES} src/nfsc/main.cpp
               APPEND PROPERTY OBJECT_DEPENDS ${NFSC_PGO_USE})
endif()

message(STATUS "gcc build: PGO generate ${NFSC_PGO_GENERATE}, PGO use '${NFSC_PGO_USE}'")
