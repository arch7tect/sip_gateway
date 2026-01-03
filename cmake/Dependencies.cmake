include(FetchContent)
include(CheckIncludeFileCXX)

option(SIPGATEWAY_FETCH_DEPS "Fetch third-party dependencies" ON)
set(SIPGATEWAY_FETCH_DEPS ON CACHE BOOL "Fetch third-party dependencies" FORCE)
option(SIPGATEWAY_BUILD_PJSIP "Build PJSIP from source" ON)
set(SIPGATEWAY_BUILD_PJSIP ON CACHE BOOL "Build PJSIP from source" FORCE)
option(SIPGATEWAY_BUILD_OPUS "Build Opus from source" ON)
set(SIPGATEWAY_BUILD_OPUS ON CACHE BOOL "Build Opus from source" FORCE)
option(SIPGATEWAY_BUILD_ONNX "Build ONNX Runtime from source" OFF)

set(SIPGATEWAY_DEPS_PREFIX "${CMAKE_BINARY_DIR}/deps" CACHE PATH "Third-party install prefix")
set(SIPGATEWAY_PJSIP_VERSION "2.16" CACHE STRING "PJSIP version")
set(SIPGATEWAY_OPUS_VERSION "1.5.2" CACHE STRING "Opus version")
set(SIPGATEWAY_ONNX_VERSION "1.23.2" CACHE STRING "ONNX Runtime version")

if(SIPGATEWAY_FETCH_DEPS)
    set(SPDLOG_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
    set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        spdlog
        GIT_REPOSITORY https://github.com/gabime/spdlog.git
        GIT_TAG v1.13.0
    )

    set(JSON_BuildTests OFF CACHE INTERNAL "" FORCE)
    FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.11.3
    )

    FetchContent_Declare(
        httplib
        GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
        GIT_TAG v0.16.0
    )

    set(CATCH_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
    set(CATCH_INSTALL_EXTRAS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.5.4
    )

    FetchContent_MakeAvailable(spdlog nlohmann_json httplib Catch2)
endif()

if(SIPGATEWAY_BUILD_PJSIP OR SIPGATEWAY_BUILD_ONNX OR SIPGATEWAY_BUILD_OPUS OR SIPGATEWAY_FETCH_DEPS)
    include(ExternalProject)
endif()

if(SIPGATEWAY_BUILD_OPUS)
    ExternalProject_Add(
        opus
        URL https://github.com/xiph/opus/archive/refs/tags/v${SIPGATEWAY_OPUS_VERSION}.tar.gz
        CMAKE_ARGS
            -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
            -DBUILD_SHARED_LIBS=OFF
            -DOPUS_BUILD_TESTING=OFF
        INSTALL_DIR ${SIPGATEWAY_DEPS_PREFIX}/opus
        BUILD_BYPRODUCTS ${SIPGATEWAY_DEPS_PREFIX}/opus/lib/libopus.a
    )
    set(SIPGATEWAY_OPUS_PREFIX ${SIPGATEWAY_DEPS_PREFIX}/opus CACHE PATH "")
    set(SIPGATEWAY_OPUS_INCLUDE_DIR ${SIPGATEWAY_OPUS_PREFIX}/include CACHE PATH "")
    set(SIPGATEWAY_OPUS_LIB_DIR ${SIPGATEWAY_OPUS_PREFIX}/lib CACHE PATH "")
endif()

if(SIPGATEWAY_BUILD_PJSIP)
    set(_SIPGATEWAY_PJSIP_LIB_NAMES
        libpjsua2
        libpjsua
        libpjsip-ua
        libpjsip-simple
        libpjsip
        libpjmedia-codec
        libpjmedia-videodev
        libpjmedia-audiodev
        libpjmedia
        libpjnath
        libpjlib-util
        libpj
        libsrtp
        libresample
        libgsmcodec
        libspeex
        libilbccodec
        libg7221codec
        libyuv
        libwebrtc
    )
    set(SIPGATEWAY_PJSIP_LINK_NAMES "")
    foreach(_lib_name IN LISTS _SIPGATEWAY_PJSIP_LIB_NAMES)
        string(REGEX REPLACE "^lib" "" _link_name "${_lib_name}")
        list(APPEND SIPGATEWAY_PJSIP_LINK_NAMES "${_link_name}")
    endforeach()
    set(SIPGATEWAY_PJSIP_LIBS "")
    foreach(_lib_name IN LISTS _SIPGATEWAY_PJSIP_LIB_NAMES)
        set(_plain_path "${SIPGATEWAY_DEPS_PREFIX}/pjproject/lib/${_lib_name}.a")
        if(EXISTS "${_plain_path}")
            list(APPEND SIPGATEWAY_PJSIP_LIBS "${_plain_path}")
        else()
            file(GLOB _candidates "${SIPGATEWAY_DEPS_PREFIX}/pjproject/lib/${_lib_name}-*.a")
            list(SORT _candidates)
            list(LENGTH _candidates _candidate_count)
            if(_candidate_count GREATER 0)
                list(GET _candidates 0 _picked)
                list(APPEND SIPGATEWAY_PJSIP_LIBS "${_picked}")
            else()
                list(APPEND SIPGATEWAY_PJSIP_LIBS "${_plain_path}")
            endif()
        endif()
    endforeach()
    ExternalProject_Add(
        pjproject
        URL https://github.com/pjsip/pjproject/archive/refs/tags/${SIPGATEWAY_PJSIP_VERSION}.tar.gz
        CONFIGURE_COMMAND <SOURCE_DIR>/configure --prefix=<INSTALL_DIR> --with-opus=${SIPGATEWAY_OPUS_PREFIX}
        BUILD_COMMAND make -j4
        INSTALL_COMMAND
            make install
            COMMAND ${CMAKE_COMMAND}
                -D LIB_DIR=<INSTALL_DIR>/lib
                -D LIBS="${_SIPGATEWAY_PJSIP_LIB_NAMES}"
                -P ${CMAKE_CURRENT_LIST_DIR}/pjsip_symlinks.cmake
        BUILD_IN_SOURCE 1
        INSTALL_DIR ${SIPGATEWAY_DEPS_PREFIX}/pjproject
        BUILD_BYPRODUCTS ${SIPGATEWAY_PJSIP_LIBS}
        DEPENDS opus
    )

    set(SIPGATEWAY_PJSIP_PREFIX ${SIPGATEWAY_DEPS_PREFIX}/pjproject CACHE PATH "")
    set(SIPGATEWAY_PJSIP_INCLUDE_DIR ${SIPGATEWAY_PJSIP_PREFIX}/include CACHE PATH "")
    set(SIPGATEWAY_PJSIP_LIB_DIR ${SIPGATEWAY_PJSIP_PREFIX}/lib CACHE PATH "")

    add_library(pjsip INTERFACE)
    add_dependencies(pjsip pjproject)
    add_dependencies(pjsip opus)
    target_include_directories(pjsip INTERFACE ${SIPGATEWAY_PJSIP_INCLUDE_DIR})
    target_link_directories(pjsip INTERFACE ${SIPGATEWAY_PJSIP_LIB_DIR})

    set(_SIPGATEWAY_PJSIP_PC "${SIPGATEWAY_PJSIP_LIB_DIR}/pkgconfig/libpjproject.pc")
    if(EXISTS "${_SIPGATEWAY_PJSIP_PC}")
        find_package(PkgConfig REQUIRED)
        set(PKG_CONFIG_USE_STATIC_LIBS ON)
        set(_SIPGATEWAY_PKG_CONFIG_PATH "${SIPGATEWAY_PJSIP_LIB_DIR}/pkgconfig")
        if(DEFINED ENV{PKG_CONFIG_PATH} AND NOT "$ENV{PKG_CONFIG_PATH}" STREQUAL "")
            set(_SIPGATEWAY_PKG_CONFIG_PATH "${_SIPGATEWAY_PKG_CONFIG_PATH}:$ENV{PKG_CONFIG_PATH}")
        endif()
        set(ENV{PKG_CONFIG_PATH} "${_SIPGATEWAY_PKG_CONFIG_PATH}")
        pkg_check_modules(PJPROJECT REQUIRED libpjproject)
        set(_SIPGATEWAY_PJPROJECT_LIBS ${PJPROJECT_LIBRARIES})
        list(REMOVE_DUPLICATES _SIPGATEWAY_PJPROJECT_LIBS)
        set(_SIPGATEWAY_PJPROJECT_LDOPTS "")
        foreach(_flag IN LISTS PJPROJECT_LDFLAGS_OTHER)
            if(_flag MATCHES "^-l")
                continue()
            endif()
            list(APPEND _SIPGATEWAY_PJPROJECT_LDOPTS "${_flag}")
        endforeach()
        list(REMOVE_DUPLICATES _SIPGATEWAY_PJPROJECT_LDOPTS)
        target_include_directories(pjsip INTERFACE ${PJPROJECT_INCLUDE_DIRS})
        target_compile_options(pjsip INTERFACE ${PJPROJECT_CFLAGS_OTHER})
        target_link_directories(pjsip INTERFACE ${PJPROJECT_LIBRARY_DIRS})
        target_link_options(pjsip INTERFACE ${_SIPGATEWAY_PJPROJECT_LDOPTS})
        target_link_libraries(pjsip INTERFACE
            ${_SIPGATEWAY_PJPROJECT_LIBS}
            ${SIPGATEWAY_PJSIP_LINK_NAMES}
        )
        target_link_directories(pjsip INTERFACE ${SIPGATEWAY_OPUS_LIB_DIR})
        target_link_libraries(pjsip INTERFACE opus)
        if(APPLE)
            target_link_options(pjsip INTERFACE
                "SHELL:-framework CoreAudio -framework AudioUnit -framework AudioToolbox -framework CoreServices -framework Foundation -framework AppKit -framework AVFoundation -framework CoreGraphics -framework QuartzCore -framework CoreVideo -framework CoreMedia -framework Metal -framework MetalKit -framework VideoToolbox"
            )
        endif()
    else()
        target_compile_definitions(pjsip INTERFACE
            PJ_IS_LITTLE_ENDIAN=1
            PJ_IS_BIG_ENDIAN=0
        )
        target_link_libraries(pjsip INTERFACE ${SIPGATEWAY_PJSIP_LINK_NAMES})
        target_link_directories(pjsip INTERFACE ${SIPGATEWAY_OPUS_LIB_DIR})
        target_link_libraries(pjsip INTERFACE opus)
        if(APPLE)
            target_link_options(pjsip INTERFACE
                "SHELL:-framework CoreAudio -framework AudioUnit -framework AudioToolbox -framework CoreServices -framework Foundation -framework AppKit -framework AVFoundation -framework CoreGraphics -framework QuartzCore -framework CoreVideo -framework CoreMedia -framework Metal -framework MetalKit -framework VideoToolbox"
            )
        endif()
    endif()
endif()

if(SIPGATEWAY_BUILD_ONNX)
    ExternalProject_Add(
        onnxruntime
        URL https://github.com/microsoft/onnxruntime/archive/refs/tags/v${SIPGATEWAY_ONNX_VERSION}.tar.gz
        CONFIGURE_COMMAND ""
        BUILD_COMMAND ""
        INSTALL_COMMAND ""
        UPDATE_COMMAND ""
        BUILD_IN_SOURCE 1
        INSTALL_DIR ${SIPGATEWAY_DEPS_PREFIX}/onnxruntime
    )

    set(SIPGATEWAY_ONNX_PREFIX ${SIPGATEWAY_DEPS_PREFIX}/onnxruntime CACHE PATH "")
    set(SIPGATEWAY_ONNX_INCLUDE_DIR ${SIPGATEWAY_ONNX_PREFIX}/include CACHE PATH "")
    set(SIPGATEWAY_ONNX_LIB_DIR ${SIPGATEWAY_ONNX_PREFIX}/lib CACHE PATH "")
endif()

if(SIPGATEWAY_FETCH_DEPS)
    ExternalProject_Add(
        websocketpp_ep
        URL https://github.com/zaphoyd/websocketpp/archive/refs/tags/0.8.2.tar.gz
        CONFIGURE_COMMAND ""
        BUILD_COMMAND ""
        INSTALL_COMMAND ""
        UPDATE_COMMAND ""
    )
    ExternalProject_Get_Property(websocketpp_ep SOURCE_DIR)
    add_library(websocketpp INTERFACE)
    target_include_directories(websocketpp INTERFACE ${SOURCE_DIR})
endif()
