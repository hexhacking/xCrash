cmake_minimum_required(VERSION 3.4.1)

#######################################
# global
#######################################

add_compile_options(
        -std=c11
        -Weverything
        -Werror)

#######################################
# external
#######################################

set(BSDSYSDS_PATH ../../../../external/bsdsysds)
set(LZMA_PATH ../../../../external/lzma/C)
set(XDL_PATH ../../../../external/xdl/xdl_lib/src/main/cpp)

#######################################
# libxcrash.so
#######################################

file(GLOB XCRASH_SRC
        xcrash/*.c
        common/*.c
        ${XDL_PATH}/*.c)

add_library(xcrash SHARED
        ${XCRASH_SRC})

target_include_directories(xcrash PUBLIC
        xcrash
        common
        ${BSDSYSDS_PATH}
        ${XDL_PATH})

target_link_libraries(xcrash
        log
        dl)

if(USEASAN)

target_compile_options(xcrash PUBLIC
        -fsanitize=address
        -fno-omit-frame-pointer)

set_target_properties(xcrash PROPERTIES
        LINK_FLAGS " \
        -fsanitize=address")

else()

target_compile_options(xcrash PUBLIC
        -Oz
        -flto
        -ffunction-sections
        -fdata-sections)

set_target_properties(xcrash PROPERTIES
        LINK_FLAGS " \
        -O3 \
        -flto \
        -Wl,--exclude-libs,ALL \
        -Wl,--gc-sections \
        -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/xcrash.exports")

endif()

#######################################
# libxcrash_dumper.so
#######################################

file(GLOB XCRASH_DUMPER_SRC
        xcrash_dumper/*.c
        common/*.c)

set(LZME_SRC
        ${LZMA_PATH}/7zCrc.c
        ${LZMA_PATH}/7zCrcOpt.c
        ${LZMA_PATH}/CpuArch.c
        ${LZMA_PATH}/Bra.c
        ${LZMA_PATH}/Bra86.c
        ${LZMA_PATH}/BraIA64.c
        ${LZMA_PATH}/Delta.c
        ${LZMA_PATH}/Lzma2Dec.c
        ${LZMA_PATH}/LzmaDec.c
        ${LZMA_PATH}/Sha256.c
        ${LZMA_PATH}/Xz.c
        ${LZMA_PATH}/XzCrc64.c
        ${LZMA_PATH}/XzCrc64Opt.c
        ${LZMA_PATH}/XzDec.c)

set_source_files_properties(${LZME_SRC} PROPERTIES
        COMPILE_FLAGS " \
        -D_7ZIP_ST \
        -Wno-enum-conversion \
        -Wno-reserved-id-macro \
        -Wno-undef \
        -Wno-missing-prototypes \
        -Wno-missing-variable-declarations \
        -Wno-cast-align \
        -Wno-sign-conversion \
        -Wno-assign-enum \
        -Wno-unused-macros \
        -Wno-padded \
        -Wno-cast-qual \
        -Wno-strict-prototypes \
        -Wno-extra-semi-stmt")

add_executable(xcrash_dumper
        ${XCRASH_DUMPER_SRC}
        ${LZME_SRC})

target_include_directories(xcrash_dumper PUBLIC
        xcrash_dumper
        common
        ${BSDSYSDS_PATH}
        ${LZMA_PATH})

target_link_libraries(xcrash_dumper
        log
        dl)

if(USEASAN)

target_compile_options(xcrash_dumper PUBLIC
        -fsanitize=address
        -fno-omit-frame-pointer)

set_target_properties(xcrash_dumper PROPERTIES
        LINK_FLAGS " \
        -fsanitize=address")

else()

target_compile_options(xcrash_dumper PUBLIC
        -Oz
        -flto
        -ffunction-sections
        -fdata-sections)

set_target_properties(xcrash_dumper PROPERTIES
        LINK_FLAGS " \
        -O3 \
        -flto \
        -Wl,--exclude-libs,ALL \
        -Wl,--gc-sections")

endif()

set_target_properties(xcrash_dumper PROPERTIES
        PREFIX "lib"
        SUFFIX ".so")
