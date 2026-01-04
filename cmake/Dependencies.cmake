include(FetchContent)
include(CheckIncludeFileCXX)

option(SIPGATEWAY_FETCH_DEPS "Fetch third-party dependencies" ON)
set(SIPGATEWAY_FETCH_DEPS ON CACHE BOOL "Fetch third-party dependencies" FORCE)
option(SIPGATEWAY_BUILD_PJSIP "Build PJSIP from source" ON)
set(SIPGATEWAY_BUILD_PJSIP ON CACHE BOOL "Build PJSIP from source" FORCE)
option(SIPGATEWAY_BUILD_OPUS "Build Opus from source" ON)
set(SIPGATEWAY_BUILD_OPUS ON CACHE BOOL "Build Opus from source" FORCE)
option(SIPGATEWAY_BUILD_ONNX "Build ONNX Runtime from source" ON)
set(SIPGATEWAY_BUILD_ONNX ON CACHE BOOL "Build ONNX Runtime from source" FORCE)

set(SIPGATEWAY_DEPS_PREFIX "${CMAKE_BINARY_DIR}/deps" CACHE PATH "Third-party install prefix")
set(SIPGATEWAY_PJSIP_VERSION "2.16" CACHE STRING "PJSIP version")
set(SIPGATEWAY_OPUS_VERSION "1.5.2" CACHE STRING "Opus version")
set(SIPGATEWAY_ONNX_VERSION "1.23.2" CACHE STRING "ONNX Runtime version")
set(SIPGATEWAY_OPENSSL_PREFIX "" CACHE PATH "OpenSSL prefix for PJSIP (optional)")

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

    FetchContent_Declare(
        asio
        GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
        GIT_TAG asio-1-30-2
    )

    FetchContent_MakeAvailable(spdlog nlohmann_json httplib Catch2 asio)

    if(asio_POPULATED)
        add_library(asio INTERFACE)
        target_include_directories(asio INTERFACE ${asio_SOURCE_DIR}/asio/include)
    endif()
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
    if(NOT SIPGATEWAY_OPENSSL_PREFIX)
        if(APPLE)
            if(EXISTS "/opt/homebrew/opt/openssl@3")
                set(SIPGATEWAY_OPENSSL_PREFIX "/opt/homebrew/opt/openssl@3")
            endif()
        elseif(UNIX)
            set(SIPGATEWAY_OPENSSL_PREFIX "/usr")
        endif()
    endif()
    set(_SIPGATEWAY_PJSIP_CONFIGURE_COMMAND <SOURCE_DIR>/configure)
    if(APPLE)
        set(_SIPGATEWAY_PKG_CONFIG_WRAPPER "${CMAKE_CURRENT_LIST_DIR}/pjproject_pkg_config.sh")
        file(CHMOD
            "${_SIPGATEWAY_PKG_CONFIG_WRAPPER}"
            PERMISSIONS
                OWNER_READ OWNER_WRITE OWNER_EXECUTE
                GROUP_READ GROUP_EXECUTE
                WORLD_READ WORLD_EXECUTE
        )
        set(_SIPGATEWAY_PJSIP_CONFIGURE_COMMAND
            ${CMAKE_COMMAND} -E env PKG_CONFIG=${_SIPGATEWAY_PKG_CONFIG_WRAPPER} <SOURCE_DIR>/configure
        )
    endif()
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
    string(REPLACE ";" "," _SIPGATEWAY_PJSIP_LIB_NAMES_CSV "${_SIPGATEWAY_PJSIP_LIB_NAMES}")
    set(SIPGATEWAY_PJSIP_LIB_NAMES_CSV "${_SIPGATEWAY_PJSIP_LIB_NAMES_CSV}" CACHE INTERNAL "")
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
        CONFIGURE_COMMAND ${_SIPGATEWAY_PJSIP_CONFIGURE_COMMAND}
            --prefix=<INSTALL_DIR>
            --with-opus=${SIPGATEWAY_OPUS_PREFIX}
            --enable-ssl
            --with-ssl=${SIPGATEWAY_OPENSSL_PREFIX}
            --with-config-site=${CMAKE_CURRENT_LIST_DIR}/pjsip_config_site.h
            --disable-shared
        BUILD_COMMAND make -j4
        INSTALL_COMMAND
            make install
            COMMAND ${CMAKE_COMMAND}
                -E copy_if_different
                ${CMAKE_CURRENT_LIST_DIR}/pjsip_config_site.h
                <INSTALL_DIR>/include/pj/config_site.h
            COMMAND ${CMAKE_COMMAND}
                -D LIB_DIR=<INSTALL_DIR>/lib
                -D LIBS=${_SIPGATEWAY_PJSIP_LIB_NAMES_CSV}
                -P ${CMAKE_CURRENT_LIST_DIR}/pjsip_symlinks.cmake
        BUILD_IN_SOURCE 1
        INSTALL_DIR ${SIPGATEWAY_DEPS_PREFIX}/pjproject
        BUILD_BYPRODUCTS ${SIPGATEWAY_PJSIP_LIBS}
        DEPENDS opus
    )
    ExternalProject_Add_StepTargets(pjproject install)
    ExternalProject_Add_Step(
        pjproject
        ensure_install
        COMMAND ${CMAKE_COMMAND} -E echo "Ensuring pjproject install step completed"
        DEPENDEES install
        ALWAYS TRUE
    )
    ExternalProject_Add_StepTargets(pjproject ensure_install)
    add_custom_target(pjsip-ready DEPENDS pjproject-install pjproject-ensure_install)

    set(SIPGATEWAY_PJSIP_PREFIX ${SIPGATEWAY_DEPS_PREFIX}/pjproject CACHE PATH "")
    set(SIPGATEWAY_PJSIP_INCLUDE_DIR ${SIPGATEWAY_PJSIP_PREFIX}/include CACHE PATH "")
    set(SIPGATEWAY_PJSIP_LIB_DIR ${SIPGATEWAY_PJSIP_PREFIX}/lib CACHE PATH "")

    add_library(pjsip INTERFACE)
    add_dependencies(pjsip pjproject-install pjproject-ensure_install)
    add_dependencies(pjsip opus)
    target_include_directories(pjsip INTERFACE ${SIPGATEWAY_PJSIP_INCLUDE_DIR})
    target_link_directories(pjsip INTERFACE ${SIPGATEWAY_PJSIP_LIB_DIR})

    option(SIPGATEWAY_USE_PJPROJECT_PKGCONFIG "Use pkg-config for pjproject" OFF)
    set(_SIPGATEWAY_PJSIP_PC "${SIPGATEWAY_PJSIP_LIB_DIR}/pkgconfig/libpjproject.pc")
    if(SIPGATEWAY_USE_PJPROJECT_PKGCONFIG AND EXISTS "${_SIPGATEWAY_PJSIP_PC}")
        find_package(PkgConfig REQUIRED)
        set(PKG_CONFIG_USE_STATIC_LIBS ON)
        set(_SIPGATEWAY_PKG_CONFIG_PATH "${SIPGATEWAY_PJSIP_LIB_DIR}/pkgconfig")
        if(DEFINED ENV{PKG_CONFIG_PATH} AND NOT "$ENV{PKG_CONFIG_PATH}" STREQUAL "")
            set(_SIPGATEWAY_PKG_CONFIG_PATH "${_SIPGATEWAY_PKG_CONFIG_PATH}:$ENV{PKG_CONFIG_PATH}")
        endif()
        set(ENV{PKG_CONFIG_PATH} "${_SIPGATEWAY_PKG_CONFIG_PATH}")
        pkg_check_modules(PJPROJECT REQUIRED libpjproject)
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
            ${SIPGATEWAY_PJSIP_LIBS}
        )
        target_link_directories(pjsip INTERFACE ${SIPGATEWAY_OPUS_LIB_DIR})
        target_link_libraries(pjsip INTERFACE opus)
        if(APPLE)
            target_link_options(pjsip INTERFACE
                "SHELL:-framework CoreAudio -framework AudioUnit -framework AudioToolbox -framework CoreServices -framework Foundation -framework AppKit -framework AVFoundation -framework CoreGraphics -framework QuartzCore -framework CoreVideo -framework CoreMedia -framework Metal -framework MetalKit -framework VideoToolbox"
            )
        elseif(UNIX)
            target_link_libraries(pjsip INTERFACE ssl crypto asound uuid opencore-amrnb opencore-amrwb)
        endif()
    else()
        target_compile_definitions(pjsip INTERFACE
            PJ_IS_LITTLE_ENDIAN=1
            PJ_IS_BIG_ENDIAN=0
        )
        target_link_libraries(pjsip INTERFACE ${SIPGATEWAY_PJSIP_LIBS})
        target_link_directories(pjsip INTERFACE ${SIPGATEWAY_OPUS_LIB_DIR})
        target_link_libraries(pjsip INTERFACE opus)
        if(APPLE)
            target_link_options(pjsip INTERFACE
                "SHELL:-framework CoreAudio -framework AudioUnit -framework AudioToolbox -framework CoreServices -framework Foundation -framework AppKit -framework AVFoundation -framework CoreGraphics -framework QuartzCore -framework CoreVideo -framework CoreMedia -framework Metal -framework MetalKit -framework VideoToolbox"
            )
        elseif(UNIX)
            target_link_libraries(pjsip INTERFACE ssl crypto asound uuid opencore-amrnb opencore-amrwb)
        endif()
    endif()
endif()

if(SIPGATEWAY_BUILD_ONNX)
    set(_SIPGATEWAY_ONNX_OS "")
    set(_SIPGATEWAY_ONNX_ARCH "")
    if(APPLE)
        set(_SIPGATEWAY_ONNX_OS "osx")
    elseif(UNIX)
        set(_SIPGATEWAY_ONNX_OS "linux")
    endif()

    if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
        set(_SIPGATEWAY_ONNX_ARCH "arm64")
        if(UNIX AND NOT APPLE)
            set(_SIPGATEWAY_ONNX_ARCH "aarch64")
        endif()
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64")
        set(_SIPGATEWAY_ONNX_ARCH "x64")
    endif()

    if(NOT _SIPGATEWAY_ONNX_OS OR NOT _SIPGATEWAY_ONNX_ARCH)
        message(FATAL_ERROR "Unsupported platform for ONNX Runtime prebuilt package.")
    endif()

    set(_SIPGATEWAY_ONNX_PACKAGE "onnxruntime-${_SIPGATEWAY_ONNX_OS}-${_SIPGATEWAY_ONNX_ARCH}-${SIPGATEWAY_ONNX_VERSION}")
    set(_SIPGATEWAY_ONNX_URL
        "https://github.com/microsoft/onnxruntime/releases/download/v${SIPGATEWAY_ONNX_VERSION}/${_SIPGATEWAY_ONNX_PACKAGE}.tgz"
    )
    set(_SIPGATEWAY_ONNX_SOURCE_DIR "${CMAKE_BINARY_DIR}/_deps/onnxruntime-src")

    ExternalProject_Add(
        onnxruntime_ep
        URL ${_SIPGATEWAY_ONNX_URL}
        CONFIGURE_COMMAND ""
        BUILD_COMMAND ""
        UPDATE_COMMAND ""
        SOURCE_DIR ${_SIPGATEWAY_ONNX_SOURCE_DIR}
        INSTALL_COMMAND
            ${CMAKE_COMMAND}
                -D SOURCE_DIR=${_SIPGATEWAY_ONNX_SOURCE_DIR}
                -D PACKAGE_NAME=${_SIPGATEWAY_ONNX_PACKAGE}
                -D DEST_DIR=${SIPGATEWAY_DEPS_PREFIX}/onnxruntime
                -P ${CMAKE_CURRENT_LIST_DIR}/onnxruntime_install.cmake
    )
    ExternalProject_Add_StepTargets(onnxruntime_ep install)

    set(SIPGATEWAY_ONNX_PREFIX ${SIPGATEWAY_DEPS_PREFIX}/onnxruntime CACHE PATH "")
    set(SIPGATEWAY_ONNX_INCLUDE_DIR ${SIPGATEWAY_ONNX_PREFIX}/include CACHE PATH "")
    set(SIPGATEWAY_ONNX_LIB_DIR ${SIPGATEWAY_ONNX_PREFIX}/lib CACHE PATH "")

    add_library(onnxruntime_iface INTERFACE)
    add_dependencies(onnxruntime_iface onnxruntime_ep-install)
    target_include_directories(onnxruntime_iface INTERFACE ${SIPGATEWAY_ONNX_INCLUDE_DIR})
    target_link_directories(onnxruntime_iface INTERFACE ${SIPGATEWAY_ONNX_LIB_DIR})
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
    add_dependencies(websocketpp websocketpp_ep)
    target_include_directories(websocketpp INTERFACE ${SOURCE_DIR})
endif()
