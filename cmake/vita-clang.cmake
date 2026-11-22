# Toolchain: clang and lld from NFSC_LLVM (else PATH) with VitaSDK's sysroot, libraries, crt objects, stubs and
# packaging tools.

if(NOT DEFINED VITASDK)
  if(NOT DEFINED ENV{VITASDK})
    message(FATAL_ERROR "Define the VITASDK environment variable (point at your VitaSDK root).")
  endif()
  set(VITASDK "$ENV{VITASDK}")
endif()
file(TO_CMAKE_PATH "${VITASDK}" VITASDK)
set(VITASDK "${VITASDK}" CACHE PATH "VitaSDK root")

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_VERSION 1)
set(CMAKE_SYSTEM_PROCESSOR armv7-a)
set(VITA True)

if(CMAKE_HOST_WIN32)
  set(NFSC_EXE ".exe")
else()
  set(NFSC_EXE "")
endif()

if(NOT DEFINED NFSC_LLVM AND DEFINED ENV{NFSC_LLVM})
  file(TO_CMAKE_PATH "$ENV{NFSC_LLVM}" NFSC_LLVM)
endif()
set(NFSC_LLVM "${NFSC_LLVM}" CACHE PATH "LLVM root (bin/clang); empty: clang from PATH")
if(NFSC_LLVM)
  set(NFSC_LLVM_BIN "${NFSC_LLVM}/bin/")
else()
  set(NFSC_LLVM_BIN "")
endif()

set(CMAKE_C_COMPILER   "${NFSC_LLVM_BIN}clang${NFSC_EXE}"       CACHE FILEPATH "C compiler")
set(CMAKE_CXX_COMPILER "${NFSC_LLVM_BIN}clang++${NFSC_EXE}"     CACHE FILEPATH "C++ compiler")
set(CMAKE_ASM_COMPILER "${NFSC_LLVM_BIN}clang${NFSC_EXE}"       CACHE FILEPATH "assembler")
set(CMAKE_AR           "${NFSC_LLVM_BIN}llvm-ar${NFSC_EXE}"     CACHE FILEPATH "archiver")
set(CMAKE_RANLIB       "${NFSC_LLVM_BIN}llvm-ranlib${NFSC_EXE}" CACHE FILEPATH "ranlib")
set(NFSC_LLVM_PROFDATA "${NFSC_LLVM_BIN}llvm-profdata${NFSC_EXE}" CACHE FILEPATH "llvm-profdata")

set(VITA_ELF_CREATE "${VITASDK}/bin/vita-elf-create${NFSC_EXE}" CACHE FILEPATH "vita-elf-create")
set(VITA_ELF_EXPORT "${VITASDK}/bin/vita-elf-export${NFSC_EXE}" CACHE FILEPATH "vita-elf-export")
set(VITA_LIBS_GEN   "${VITASDK}/bin/vita-libs-gen${NFSC_EXE}"   CACHE FILEPATH "vita-libs-gen")
set(VITA_MAKE_FSELF "${VITASDK}/bin/vita-make-fself${NFSC_EXE}" CACHE FILEPATH "vita-make-fself")
set(VITA_MKSFOEX    "${VITASDK}/bin/vita-mksfoex${NFSC_EXE}"    CACHE FILEPATH "vita-mksfoex")
set(VITA_PACK_VPK   "${VITASDK}/bin/vita-pack-vpk${NFSC_EXE}"   CACHE FILEPATH "vita-pack-vpk")

set(NFSC_VITA_SYSROOT "${VITASDK}/arm-vita-eabi")
set(CMAKE_FIND_ROOT_PATH "${VITASDK}/bin" "${NFSC_VITA_SYSROOT}")
set(CMAKE_SYSTEM_PREFIX_PATH ${CMAKE_FIND_ROOT_PATH})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# libstdc++ headers and crt objects of VitaSDK's gcc.
file(GLOB NFSC_GCC_LIB_DIR LIST_DIRECTORIES true "${VITASDK}/lib/gcc/arm-vita-eabi/*")
file(GLOB NFSC_CXX_INCLUDE_DIR LIST_DIRECTORIES true "${NFSC_VITA_SYSROOT}/include/c++/*")
if(NOT NFSC_GCC_LIB_DIR OR NOT NFSC_CXX_INCLUDE_DIR)
  message(FATAL_ERROR "VitaSDK's gcc runtime (lib/gcc/arm-vita-eabi, include/c++) not found under ${VITASDK}")
endif()
list(GET NFSC_GCC_LIB_DIR 0 NFSC_GCC_LIB_DIR)
list(GET NFSC_CXX_INCLUDE_DIR 0 NFSC_CXX_INCLUDE_DIR)

set(CMAKE_C_COMPILER_TARGET   armv7a-none-eabihf)
set(CMAKE_CXX_COMPILER_TARGET armv7a-none-eabihf)
set(CMAKE_ASM_COMPILER_TARGET armv7a-none-eabihf)
set(CMAKE_SYSROOT "${NFSC_VITA_SYSROOT}")
set(CMAKE_CXX_STANDARD_INCLUDE_DIRECTORIES "${NFSC_CXX_INCLUDE_DIR}" "${NFSC_CXX_INCLUDE_DIR}/arm-vita-eabi")

# VitaSDK gcc's ABI: Thumb-2, small enums, no PIC, emulated TLS.
set(NFSC_TOOLCHAIN_FLAGS "-mthumb -fshort-enums -fno-pic -fno-pie -femulated-tls -D__vita__=1")
set(CMAKE_C_FLAGS   "${NFSC_TOOLCHAIN_FLAGS}" CACHE STRING "C flags" FORCE)
set(CMAKE_CXX_FLAGS "${NFSC_TOOLCHAIN_FLAGS}" CACHE STRING "C++ flags" FORCE)
set(CMAKE_ASM_FLAGS "${NFSC_TOOLCHAIN_FLAGS}" CACHE STRING "assembler flags" FORCE)
set(CMAKE_C_FLAGS_RELEASE   "-O3 -DNDEBUG" CACHE STRING "C release flags")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG" CACHE STRING "C++ release flags")
set(CMAKE_C_FLAGS_DEBUG   "-O0 -g -DDEBUG -D_DEBUG" CACHE STRING "C debug flags")
set(CMAKE_CXX_FLAGS_DEBUG "-O0 -g -DDEBUG -D_DEBUG" CACHE STRING "C++ debug flags")

# Link of VitaSDK's gcc driver: crti crtbegin crt0, the objects and libraries, its default libraries, crtend crtn;
# relocations kept (-q), 4 KB pages. The linker script is added in cmake/clang.cmake.
set(CMAKE_EXE_LINKER_FLAGS
  "-fuse-ld=lld -nostdlib -Wl,-q -Wl,-z,nocopyreloc -Wl,-z,norelro -Wl,-z,max-page-size=0x1000 -Wl,--no-rosegment \
-Wl,--no-eh-frame-hdr -Wl,--pic-veneer -L${NFSC_GCC_LIB_DIR} \
${NFSC_GCC_LIB_DIR}/crti.o ${NFSC_GCC_LIB_DIR}/crtbegin.o ${NFSC_VITA_SYSROOT}/lib/crt0.o"
  CACHE STRING "executable linker flags" FORCE)
set(NFSC_VITA_LIBRARIES
  "-lstdc++ -lm -Wl,--start-group -lgcc -lc -lSceRtc_stub -lSceSysmem_stub -lSceKernelThreadMgr_stub \
-lSceKernelModulemgr_stub -lSceIofilemgr_stub -lSceProcessmgr_stub -lSceLibKernel_stub -lSceNet_stub \
-lSceNetCtl_stub -lSceSysmodule_stub -Wl,--end-group ${NFSC_GCC_LIB_DIR}/crtend.o ${NFSC_GCC_LIB_DIR}/crtn.o")
set(CMAKE_C_STANDARD_LIBRARIES   "${NFSC_VITA_LIBRARIES}" CACHE STRING "C libraries" FORCE)
set(CMAKE_CXX_STANDARD_LIBRARIES "${NFSC_VITA_LIBRARIES}" CACHE STRING "C++ libraries" FORCE)

# Compiler checks build a static library.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
