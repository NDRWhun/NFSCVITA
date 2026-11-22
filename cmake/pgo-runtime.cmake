# nfsc_pgo_runtime(<out-var>): adds the static library nfsc_profile (compiler-rt's bare-metal profile runtime with
# value profiling, psp2-tooling's Vita platform file at a pinned commit, src/platform/pgo_writer.c), returns its name.
# compiler-rt comes from NFSC_COMPILER_RT, else is downloaded for the compiler's version.

set(NFSC_COMPILER_RT "$ENV{NFSC_COMPILER_RT}" CACHE PATH "compiler-rt source tree for the PGO runtime (empty: download)")

set(NFSC_PSP2_TOOLING_COMMIT bed2efdf4e7c3f83e58c00fa4f2da09c6441d053)
set(NFSC_PSP2_TOOLING_PLATFORM_SHA256 f6456c874b663a6ef61335b5120d734e388a0c078206da4f703628884558d6a1)

# Downloads url to path unless it exists; extra arguments go to file(DOWNLOAD).
function(nfsc_pgo_download url path)
  if(NOT EXISTS "${path}")
    message(STATUS "PGO runtime: downloading ${url}")
    file(DOWNLOAD "${url}" "${path}.part" TLS_VERIFY ON STATUS status ${ARGN})
    list(GET status 0 code)
    if(NOT code EQUAL 0)
      file(REMOVE "${path}.part")
      message(FATAL_ERROR "Download of ${url} failed (${status})")
    endif()
    file(RENAME "${path}.part" "${path}")
  endif()
endfunction()

function(nfsc_pgo_runtime out_var)
  set(sources InstrProfiling.c InstrProfilingInternal.c InstrProfilingBuffer.c InstrProfilingMerge.c
      InstrProfilingMergeFile.c InstrProfilingNameVar.c InstrProfilingVersionVar.c InstrProfilingWriter.c
      InstrProfilingValue.c)
  set(headers lib/profile/InstrProfiling.h lib/profile/InstrProfilingInternal.h lib/profile/InstrProfilingPort.h
      lib/profile/InstrProfilingUtil.h include/profile/InstrProfData.inc include/profile/instr_prof_interface.h)
  if(NFSC_COMPILER_RT)
    set(root "${NFSC_COMPILER_RT}")
  else()
    set(version "${CMAKE_CXX_COMPILER_VERSION}")
    set(root "${CMAKE_BINARY_DIR}/compiler-rt-${version}")
    set(url "https://raw.githubusercontent.com/llvm/llvm-project/llvmorg-${version}/compiler-rt")
    set(files ${headers})
    foreach(source ${sources})
      list(APPEND files lib/profile/${source})
    endforeach()
    foreach(file ${files})
      nfsc_pgo_download("${url}/${file}" "${root}/${file}")
    endforeach()
  endif()
  set(paths "")
  foreach(source ${sources})
    if(NOT EXISTS "${root}/lib/profile/${source}")
      message(FATAL_ERROR "${root}/lib/profile/${source} not found (NFSC_COMPILER_RT)")
    endif()
    list(APPEND paths "${root}/lib/profile/${source}")
  endforeach()
  # Profile section bounds from lld's __start_/__stop_ symbols, no binary IDs.
  set(platform "${CMAKE_BINARY_DIR}/psp2-tooling-${NFSC_PSP2_TOOLING_COMMIT}/pgo/InstrProfilingPlatformVita.c")
  nfsc_pgo_download(
    "https://raw.githubusercontent.com/Grarak/psp2-tooling/${NFSC_PSP2_TOOLING_COMMIT}/pgo/InstrProfilingPlatformVita.c"
    "${platform}" EXPECTED_HASH SHA256=${NFSC_PSP2_TOOLING_PLATFORM_SHA256})
  list(APPEND paths "${platform}")
  add_library(nfsc_profile STATIC ${paths} src/platform/pgo_writer.c)
  target_compile_options(nfsc_profile PRIVATE ${NFSC_ARCH_FLAGS} -O2 -fno-lto -w)
  target_compile_definitions(nfsc_profile PRIVATE COMPILER_RT_PROFILE_BAREMETAL=1 COMPILER_RT_HAS_ATOMICS=1)
  target_include_directories(nfsc_profile PRIVATE "${root}/lib" "${root}/lib/profile" "${root}/include")
  set(${out_var} nfsc_profile PARENT_SCOPE)
endfunction()
