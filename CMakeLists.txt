# NFSCVITA - Need for Speed Carbon (static recompilation) -> PS Vita. ELF -> SELF -> VPK.
cmake_minimum_required(VERSION 3.16)

option(NFSC_RUNTIME_CHECK "Only compile the runtime sources that build without the generated code (CI)" OFF)
# gcc: VitaSDK's compiler, PGO (cmake/gcc.cmake). clang: clang and lld (cmake/vita-clang.cmake, LLVM from NFSC_LLVM),
# ThinLTO, PGO.
set(NFSC_COMPILER "gcc" CACHE STRING "Compiler: gcc (VitaSDK) or clang (LLVM)")
set_property(CACHE NFSC_COMPILER PROPERTY STRINGS gcc clang)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(NFSC_COMPILER STREQUAL "clang")
    set(CMAKE_TOOLCHAIN_FILE "${CMAKE_SOURCE_DIR}/cmake/vita-clang.cmake" CACHE PATH "toolchain file")
  elseif(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Define the VITASDK environment variable (point at your VitaSDK root).")
  endif()
endif()

project(NFSCVITA C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# GNU mode keeps newlib's POSIX declarations (ftruncate, strcasecmp) visible.
set(CMAKE_CXX_EXTENSIONS ON)

include("${VITASDK}/share/vita.cmake" REQUIRED)

if(NFSC_COMPILER STREQUAL "clang")
  if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR "NFSC_COMPILER=clang needs the toolchain file cmake/vita-clang.cmake")
  endif()
  set(NFSC_CLANG ON)
elseif(NFSC_COMPILER STREQUAL "gcc")
  set(NFSC_CLANG OFF)
else()
  message(FATAL_ERROR "NFSC_COMPILER must be gcc or clang")
endif()

set(VITA_APP_NAME "NFSCVITA")
set(VITA_TITLEID  "NFSCVITA0")
set(VITA_VERSION  "00.01")

option(NFSC_LOG_DEBUG        "Verbose runtime logging" OFF)
option(NFSC_ASSERT_SOFT      "Log failed assertions instead of trapping" OFF)
option(NFSC_RENDER_CHECK     "Compare native renderer calls with the translated code's (not for timing)" OFF)
set(NFSC_GUEST_HEAP_MIB "232" CACHE STRING "Guest heap, thread stacks and shim buffers in MiB")
if(NOT NFSC_GUEST_HEAP_MIB MATCHES "^[0-9]+$" OR NFSC_GUEST_HEAP_MIB LESS 64 OR NFSC_GUEST_HEAP_MIB GREATER 232)
  message(FATAL_ERROR "NFSC_GUEST_HEAP_MIB must be between 64 and 232")
endif()
set_source_files_properties(src/lib/memmap.cpp PROPERTIES
  COMPILE_DEFINITIONS "NFSC_GUEST_HEAP_MIB=${NFSC_GUEST_HEAP_MIB}")
# The run-time helpers the code generator calls must stay out of link-time optimisation, which would drop them.
set_source_files_properties(src/platform/memfuncs.cpp PROPERTIES COMPILE_OPTIONS -fno-lto)
# Profile-guided optimisation: cmake/gcc.cmake, cmake/clang.cmake.
option(NFSC_PGO_GENERATE "Instrument for a PGO profile" OFF)
set(NFSC_PGO_USE "" CACHE STRING "Profile to optimise with: nfsc.gcov files (gcc, a list) or an indexed .profdata (clang)")
if(NFSC_PGO_GENERATE AND NFSC_PGO_USE)
  message(FATAL_ERROR "NFSC_PGO_GENERATE and NFSC_PGO_USE exclude each other")
endif()

include(cmake/shaders.cmake)
include(cmake/assets.cmake)
include(cmake/sources.cmake)

nfsc_add_shaders(NFSC_SHADER_SOURCES)
nfsc_add_prompt_glyphs(NFSC_ASSET_SOURCES)

if(NFSC_CLANG)
  set(NFSC_ARCH_FLAGS -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard)
else()
  set(NFSC_ARCH_FLAGS -march=armv7-a -mtune=cortex-a9 -mfpu=neon -mfloat-abi=hard)
endif()
set(NFSC_COMMON_FLAGS
  ${NFSC_ARCH_FLAGS}
  # Math functions do not set errno: sqrt is a single vsqrt.f64.
  -fno-math-errno
  -fno-exceptions -fno-asynchronous-unwind-tables -fno-unwind-tables
  -ffunction-sections -fdata-sections
  -Wall -Wextra
  # __FILE__ in assertions and logs: paths relative to the repository, nothing from the build machine.
  -ffile-prefix-map=${CMAKE_SOURCE_DIR}/=
)
# Generated code only sees the stable defines; debug switches stay in the runtime to avoid full rebuilds.
set(NFSC_DEFINES VITA __vita__ WITH_MMX)
# Census build: generated code marks the guest memory it reads and writes for display_census (slower everywhere).
option(NFSC_CENSUS_ACCESS "Mark guest memory reads and writes for the display census" OFF)
if(NFSC_CENSUS_ACCESS)
  list(APPEND NFSC_DEFINES NFSC_CENSUS_ACCESS)
endif()
set(NFSC_RUNTIME_DEFINES ${NFSC_DEFINES})
if(NFSC_LOG_DEBUG)
  list(APPEND NFSC_RUNTIME_DEFINES NFSC_LOG_DEBUG)
endif()
if(NFSC_ASSERT_SOFT)
  list(APPEND NFSC_RUNTIME_DEFINES NFSC_ASSERT_SOFT)
endif()
if(NFSC_RENDER_CHECK)
  list(APPEND NFSC_RUNTIME_DEFINES NFSC_RENDER_CHECK=1)
endif()

set(NFSC_GENERATED_DIR "${CMAKE_SOURCE_DIR}/src/nfsc/generated" CACHE PATH "Generated game code (tools/generate.ps1)")
set(NFSC_INCLUDES
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src/nfsc
  ${NFSC_GENERATED_DIR}
)

# Compiles the runtime sources that do not need the generated game code; no VPK (CI without game binaries).
if(NFSC_RUNTIME_CHECK)
  add_library(nfsc_runtime_check OBJECT ${NFSC_STANDALONE_SOURCES} ${NFSC_SHADER_SOURCES} ${NFSC_ASSET_SOURCES})
  target_compile_options(nfsc_runtime_check PRIVATE ${NFSC_COMMON_FLAGS} -O3 -Werror)
  target_compile_definitions(nfsc_runtime_check PRIVATE ${NFSC_RUNTIME_DEFINES})
  target_include_directories(nfsc_runtime_check PRIVATE ${NFSC_INCLUDES})
  return()
endif()

file(GLOB NFSC_GENERATED_SOURCES CONFIGURE_DEPENDS "${NFSC_GENERATED_DIR}/*.cpp")
if(NOT NFSC_GENERATED_SOURCES)
  message(FATAL_ERROR "${NFSC_GENERATED_DIR} is empty; run tools/generate.ps1 first.")
endif()

if(NFSC_CLANG)
  include(cmake/clang.cmake)
  set(NFSC_NO_SLP -fno-slp-vectorize)
else()
  include(cmake/gcc.cmake)
  set(NFSC_NO_SLP -fno-tree-slp-vectorize)
endif()
# The native audio decoders store their float samples one at a time: no SLP packing through the stack either.
set_property(SOURCE src/nfsc/audio_decode.cpp APPEND PROPERTY COMPILE_OPTIONS ${NFSC_NO_SLP})

# Generated game code: large, optimise for size and compile time.
add_library(nfsc_game OBJECT ${NFSC_GENERATED_SOURCES})
# No SLP vectorisation: it packs guest stores into NEON stores through the stack (store forwarding stalls).
target_compile_options(nfsc_game PRIVATE ${NFSC_COMMON_FLAGS} -O2 ${NFSC_NO_SLP} -Wno-unused-label -Wno-unused-variable -Wno-unused-parameter ${NFSC_CLANG_GAME_FLAGS} ${NFSC_GCC_GAME_FLAGS})
# Start-up method registration (nfsc.cpp, runs once) gains nothing from -O2 and compiles much faster at -Og.
set(NFSC_LIGHT_SOURCES ${NFSC_GENERATED_SOURCES})
list(FILTER NFSC_LIGHT_SOURCES INCLUDE REGEX "/nfsc\\.cpp$")
set_source_files_properties(${NFSC_LIGHT_SOURCES} PROPERTIES COMPILE_OPTIONS -Og)
if(NFSC_CLANG)
  set_property(SOURCE ${NFSC_LIGHT_SOURCES} APPEND PROPERTY COMPILE_OPTIONS ${NFSC_CLANG_LIGHT_FLAGS})
endif()
target_compile_definitions(nfsc_game PRIVATE ${NFSC_DEFINES})
target_include_directories(nfsc_game PRIVATE ${NFSC_INCLUDES})

# Runtime and Vita backend.
add_library(nfsc_runtime OBJECT ${NFSC_RUNTIME_SOURCES} ${NFSC_SHADER_SOURCES} ${NFSC_ASSET_SOURCES})
target_compile_options(nfsc_runtime PRIVATE ${NFSC_COMMON_FLAGS} -O3 ${NFSC_CLANG_FLAGS} ${NFSC_GCC_FLAGS})
target_compile_definitions(nfsc_runtime PRIVATE ${NFSC_RUNTIME_DEFINES})
target_include_directories(nfsc_runtime PRIVATE ${NFSC_INCLUDES})

add_executable(NFSCVITA.elf src/nfsc/main.cpp ${NFSC_CLANG_SOURCES} ${NFSC_GCC_SOURCES}
               $<TARGET_OBJECTS:nfsc_game> $<TARGET_OBJECTS:nfsc_runtime>)
target_compile_options(NFSCVITA.elf PRIVATE ${NFSC_COMMON_FLAGS} -O2 ${NFSC_CLANG_FLAGS} ${NFSC_GCC_FLAGS})
target_compile_definitions(NFSCVITA.elf PRIVATE ${NFSC_RUNTIME_DEFINES})
target_include_directories(NFSCVITA.elf PRIVATE ${NFSC_INCLUDES})
# Nothing references the main thread's stack size; keep it from being collected so the loader sees it.
target_link_options(NFSCVITA.elf PRIVATE -Wl,--gc-sections -Wl,--undefined=sceUserMainThreadStackSize)
if(NFSC_CLANG)
  target_link_options(NFSCVITA.elf PRIVATE ${NFSC_CLANG_LINK_FLAGS})
  target_link_libraries(NFSCVITA.elf ${NFSC_CLANG_LIBRARIES})
  set_target_properties(NFSCVITA.elf PROPERTIES LINK_DEPENDS "${NFSC_LLD_SCRIPT}")
else()
  target_link_options(NFSCVITA.elf PRIVATE ${NFSC_GCC_LINK_FLAGS})
endif()
target_link_libraries(NFSCVITA.elf
  stdc++
  SceGxm_stub
  SceDisplay_stub
  SceCtrl_stub
  SceTouch_stub
  SceCommonDialog_stub
  SceAppUtil_stub
  SceAudio_stub
  ScePower_stub
  SceRtc_stub
  SceLibKernel_stub
  SceKernelThreadMgr_stub
  SceSysmem_stub
  SceProcessmgr_stub
  c
  m
)

vita_create_self(NFSCVITA.self NFSCVITA.elf UNSAFE)

# ATTRIBUTE2=12: extended memory budget for the guest arena.
set(VITA_MKSFOEX_FLAGS "-d ATTRIBUTE2=12")
configure_file(
  ${CMAKE_SOURCE_DIR}/sce_sys/livearea/contents/template.xml.in
  ${CMAKE_BINARY_DIR}/generated/livearea/template.xml
  @ONLY)

vita_create_vpk(NFSCVITA.vpk ${VITA_TITLEID} NFSCVITA.self
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE ${CMAKE_SOURCE_DIR}/sce_sys/icon0.png sce_sys/icon0.png
  FILE ${CMAKE_SOURCE_DIR}/sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
  FILE ${CMAKE_SOURCE_DIR}/sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
  FILE ${CMAKE_BINARY_DIR}/generated/livearea/template.xml sce_sys/livearea/contents/template.xml
)
