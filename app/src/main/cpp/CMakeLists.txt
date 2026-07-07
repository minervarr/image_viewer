cmake_minimum_required(VERSION 3.22.1)
project("hdrviewer")

# Ensure 16KB page size compatibility for modern Android 15+ devices
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,-z,max-page-size=16384")

include_directories(${CMAKE_CURRENT_SOURCE_DIR})

# Prebuilt libavif + libaom (AV1 encoder) static libs, built once for arm64 with
# the NDK toolchain (see libs/prebuilt/). Used for lossless/HDR AVIF export.
set(AVIF_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../../libs)
set(PREBUILT_DIR ${AVIF_DIR}/prebuilt/${ANDROID_ABI})

add_library(avif_lib STATIC IMPORTED)
set_target_properties(avif_lib PROPERTIES IMPORTED_LOCATION ${PREBUILT_DIR}/libavif.a)

add_library(aom_lib STATIC IMPORTED)
set_target_properties(aom_lib PROPERTIES IMPORTED_LOCATION ${PREBUILT_DIR}/libaom.a)

add_library(
        hdrviewer
        SHARED
        native-lib.cpp
        miniz.c)

target_include_directories(hdrviewer PRIVATE ${AVIF_DIR}/libavif/include)

find_library(
        log-lib
        log)

find_library(
        jnigraphics-lib
        jnigraphics)

target_link_libraries(
        hdrviewer
        ${log-lib}
        ${jnigraphics-lib}
        avif_lib
        aom_lib)
