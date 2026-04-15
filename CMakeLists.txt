cmake_minimum_required(VERSION 3.16)
project(AACIceEncoderWebUI VERSION 1.0.18)

# Generate a header with version info
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/version.h.in
        ${CMAKE_CURRENT_BINARY_DIR}/version_generated.h @ONLY)

set(CMAKE_CXX_STANDARD 17)

find_package(PkgConfig REQUIRED)
pkg_check_modules(PORTAUDIO REQUIRED portaudio-2.0)
pkg_check_modules(XML2 REQUIRED libxml-2.0)
pkg_check_modules(OPENSSL REQUIRED openssl)

find_path(FDKAAC_INCLUDE_DIR aacenc_lib.h
        HINTS /usr/include/fdk-aac
)

find_library(FDKAAC_LIBRARY fdk-aac
        HINTS /usr/lib /usr/lib/x86_64-linux-gnu
)

# Vendor single-header httplib
include_directories(${CMAKE_SOURCE_DIR}/vendor/httplib)

include_directories(
        ${PORTAUDIO_INCLUDE_DIRS}
        ${XML2_INCLUDE_DIRS}
        ${OPENSSL_INCLUDE_DIRS}
        ${FDKAAC_INCLUDE_DIR}
        ${CMAKE_CURRENT_BINARY_DIR}
        ${CMAKE_SOURCE_DIR}/vendor/httplib
        ${CMAKE_SOURCE_DIR}/vendor/json
        ${CMAKE_SOURCE_DIR}/vendor/wsserver
        ${CMAKE_SOURCE_DIR}/vendor/asio-master
        ${CMAKE_SOURCE_DIR}/vendor/lame/include
)

link_directories(
        ${PORTAUDIO_LIBRARY_DIRS}
        ${XML2_LIBRARY_DIRS}
        ${OPENSSL_LIBRARY_DIRS}
)

find_package(CURL REQUIRED)

# -------------------------------------------------------------------
# NEW SECTION: Add vendored LAME (static library)
# -------------------------------------------------------------------
# Build libmp3lame from sources in vendor/lame
file(GLOB LAME_SRC ${CMAKE_SOURCE_DIR}/vendor/lame/libmp3lame/*.c)

add_library(mp3lame STATIC ${LAME_SRC})

# Provide a config header (simplified).
configure_file(${CMAKE_SOURCE_DIR}/vendor/lame/include/configMS.h
        ${CMAKE_CURRENT_BINARY_DIR}/config.h COPYONLY)

# Make sure build-dir comes first so config.h is found there
target_include_directories(mp3lame PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}
        ${CMAKE_SOURCE_DIR}/vendor/lame/include




)
target_compile_definitions(mp3lame PRIVATE HAVE_CONFIG_H)

# -------------------------------------------------------------------

add_executable(encoder src/main.cpp
        src/encoders.h
)

target_link_libraries(encoder
        ${PORTAUDIO_LIBRARIES}
        ${XML2_LIBRARIES}
        ${OPENSSL_LIBRARIES}
        ${FDKAAC_LIBRARY}
        CURL::libcurl
        pthread
        mp3lame   # <-- NEW: link against static LAME
)

add_executable(list_devices src/list_devices.cpp)
target_link_libraries(list_devices ${PORTAUDIO_LIBRARIES})

target_compile_definitions(encoder PRIVATE ASIO_STANDALONE)

# -------------------------------------------------------------------
# Ensure config directory is copied into build output
# -------------------------------------------------------------------
file(COPY ${CMAKE_SOURCE_DIR}/config DESTINATION ${CMAKE_BINARY_DIR})

install(DIRECTORY ${CMAKE_SOURCE_DIR}/config DESTINATION .)

# Optionally, also install logs directory structure (empty by default)
install(DIRECTORY DESTINATION logs)
