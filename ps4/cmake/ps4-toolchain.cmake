# CMake toolchain for building the Perfect Dark port as PS4 homebrew with the OpenOrbis
# toolchain and a stock LLVM 18 (clang + ld.lld).
#
#   cmake -G Ninja -DCMAKE_TOOLCHAIN_FILE=<this file> ...
#
# Adapted from alechurri/shipofharkinian-ps4 (MIT). Paths default to the layout created by
# env.sh; override with -DOO_PS4_TOOLCHAIN=... / -DPS4_LLVM_BIN=... / -DPS4_PREFIX=... or the
# matching environment variables.

set(CMAKE_SYSTEM_NAME PS4)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_CROSSCOMPILING TRUE)
set(PS4 TRUE)

# perfect_dark/ps4/cmake/.. = the ps4 folder; tools are downloaded to ps4/tools by setup.sh
get_filename_component(PD_PS4_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(_PS4_TOOLS "${PD_PS4_ROOT}/tools")

if(NOT OO_PS4_TOOLCHAIN)
    if(DEFINED ENV{OO_PS4_TOOLCHAIN})
        file(TO_CMAKE_PATH "$ENV{OO_PS4_TOOLCHAIN}" OO_PS4_TOOLCHAIN)
    else()
        set(OO_PS4_TOOLCHAIN "${_PS4_TOOLS}/OpenOrbis/PS4Toolchain")
    endif()
endif()
if(NOT PS4_LLVM_BIN)
    if(DEFINED ENV{PS4_LLVM_BIN})
        file(TO_CMAKE_PATH "$ENV{PS4_LLVM_BIN}" PS4_LLVM_BIN)
    elseif(EXISTS "${_PS4_TOOLS}/llvm/bin")
        set(PS4_LLVM_BIN "${_PS4_TOOLS}/llvm/bin")
    else()
        set(PS4_LLVM_BIN "/usr/bin")
    endif()
endif()
if(NOT PS4_PREFIX)
    if(DEFINED ENV{PS4_PREFIX})
        file(TO_CMAKE_PATH "$ENV{PS4_PREFIX}" PS4_PREFIX)
    else()
        set(PS4_PREFIX "${PD_PS4_ROOT}/prefix")
    endif()
endif()
set(OO_PS4_TOOLCHAIN "${OO_PS4_TOOLCHAIN}" CACHE PATH "OpenOrbis toolchain root")
set(PS4_LLVM_BIN "${PS4_LLVM_BIN}" CACHE PATH "LLVM bin directory")
set(PS4_PREFIX "${PS4_PREFIX}" CACHE PATH "Install prefix of the cross-built dependencies")

# try_compile() re-reads this file in a fresh scope: forward what it needs.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES OO_PS4_TOOLCHAIN PS4_LLVM_BIN PS4_PREFIX)

if(CMAKE_HOST_WIN32)
    set(_EXE ".exe")
else()
    set(_EXE "")
endif()

set(CMAKE_C_COMPILER "${PS4_LLVM_BIN}/clang${_EXE}")
set(CMAKE_CXX_COMPILER "${PS4_LLVM_BIN}/clang++${_EXE}")
set(CMAKE_ASM_COMPILER "${PS4_LLVM_BIN}/clang${_EXE}")
set(CMAKE_AR "${PS4_LLVM_BIN}/llvm-ar${_EXE}" CACHE FILEPATH "")
set(CMAKE_RANLIB "${PS4_LLVM_BIN}/llvm-ranlib${_EXE}" CACHE FILEPATH "")
set(CMAKE_NM "${PS4_LLVM_BIN}/llvm-nm${_EXE}" CACHE FILEPATH "")
set(CMAKE_STRIP "${PS4_LLVM_BIN}/llvm-strip${_EXE}" CACHE FILEPATH "")
set(CMAKE_OBJCOPY "${PS4_LLVM_BIN}/llvm-objcopy${_EXE}" CACHE FILEPATH "")
set(PS4_LINKER "${PS4_LLVM_BIN}/ld.lld${_EXE}")

set(_PS4_TARGET x86_64-pc-freebsd12-elf)
set(CMAKE_C_COMPILER_TARGET ${_PS4_TARGET})
set(CMAKE_CXX_COMPILER_TARGET ${_PS4_TARGET})
set(CMAKE_ASM_COMPILER_TARGET ${_PS4_TARGET})

# Flags every translation unit needs. They are injected through the compile rules (see
# Platform/PS4.cmake) instead of CMAKE_<LANG>_FLAGS because third-party projects overwrite
# CMAKE_CXX_FLAGS wholesale.
#  - libc++ headers have to come before the C headers (they wrap them with #include_next).
#  - our own builds (SDL2 2.30, zlib) must win over the old copies bundled with OpenOrbis.
#  - ps4_fixups.h fixes the struct stat layout of the OpenOrbis headers and must be seen before
#    any system header, hence -include.
set(_PS4_COMMON "-fPIC -funwind-tables -D__PS4__ -D__ORBIS__ -D_BSD_SOURCE -D_GNU_SOURCE -isysroot \"${OO_PS4_TOOLCHAIN}\"")
set(_PS4_COMPAT "-isystem \"${PD_PS4_ROOT}/compat/include\" -include \"${PD_PS4_ROOT}/compat/include/ps4_fixups.h\"")
set(_PS4_PREFIX_INC "-isystem \"${PS4_PREFIX}/include\"")
set(PS4_C_MANDATORY_FLAGS "${_PS4_COMMON} ${_PS4_PREFIX_INC} -isystem \"${OO_PS4_TOOLCHAIN}/include\" ${_PS4_COMPAT}")
set(PS4_CXX_MANDATORY_FLAGS "${_PS4_COMMON} -nostdinc++ -isystem \"${OO_PS4_TOOLCHAIN}/include/c++/v1\" ${_PS4_PREFIX_INC} -isystem \"${OO_PS4_TOOLCHAIN}/include\" ${_PS4_COMPAT} -fexceptions -fcxx-exceptions")

set(CMAKE_C_FLAGS_DEBUG_INIT "-g -O0")
set(CMAKE_CXX_FLAGS_DEBUG_INIT "-g -O0")
set(CMAKE_C_FLAGS_RELEASE_INIT "-O2 -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE_INIT "-O2 -DNDEBUG")

# System libraries every executable gets; SCE stub libraries through PS4_SCE_LIBS.
set(PS4_BASE_LIBS "-lc++ -lc++abi -lunwind -lc -lkernel -lclang_rt.builtins-x86_64")
set(PS4_SCE_LIBS "-lSceSysmodule -lSceSystemService -lSceUserService -lScePad -lSceAudioOut -lSceVideoOut -lScePigletv2VSH -lSceRtc"
    CACHE STRING "SCE stub libraries linked into executables")

set(_PS4_LINK "\"${PS4_LINKER}\" -m elf_x86_64 -pie --script \"${OO_PS4_TOOLCHAIN}/link.x\" --eh-frame-hdr -o <TARGET> \"${OO_PS4_TOOLCHAIN}/lib/crt1.o\" <OBJECTS> <LINK_FLAGS> -L\"${PS4_PREFIX}/lib\" -L\"${OO_PS4_TOOLCHAIN}/lib\" --start-group <LINK_LIBRARIES> ${PS4_BASE_LIBS} --end-group ${PS4_SCE_LIBS}")
set(CMAKE_C_LINK_EXECUTABLE "${_PS4_LINK}")
set(CMAKE_CXX_LINK_EXECUTABLE "${_PS4_LINK}")

set(CMAKE_EXECUTABLE_SUFFIX ".elf")
set(CMAKE_EXECUTABLE_SUFFIX_C ".elf")
set(CMAKE_EXECUTABLE_SUFFIX_CXX ".elf")
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(CMAKE_FIND_ROOT_PATH "${PS4_PREFIX}" "${OO_PS4_TOOLCHAIN}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
list(APPEND CMAKE_PREFIX_PATH "${PS4_PREFIX}")
set(CMAKE_INSTALL_PREFIX "${PS4_PREFIX}" CACHE PATH "")

# Platform/PS4.cmake lives next to this file.
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}")

set(THREADS_PREFER_PTHREAD_FLAG OFF)
set(CMAKE_THREAD_LIBS_INIT "")
set(CMAKE_HAVE_THREADS_LIBRARY 1)
set(CMAKE_USE_PTHREADS_INIT 1)
set(Threads_FOUND TRUE)
