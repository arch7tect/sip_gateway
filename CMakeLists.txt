cmake_minimum_required(VERSION 4.1)
project(sip_gateway LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(SIPGATEWAY_WARNINGS
    -Wall
    -Wextra
    -Wpedantic
)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(Dependencies)
include(FetchContent)
find_package(OpenSSL)

if(NOT TARGET pjsip)
    message(FATAL_ERROR "PJSIP is required. Ensure SIPGATEWAY_BUILD_PJSIP is enabled.")
endif()

add_executable(sip_gateway
    src/main.cpp
    src/config.cpp
    src/logging.cpp
    src/backend/client.cpp
    src/backend/ws_client.cpp
    src/audio/port.cpp
    src/audio/player.cpp
    src/audio/recorder.cpp
    src/utils/async.cpp
    src/sip/app.cpp
    src/sip/account.cpp
    src/sip/call.cpp
    src/server/rest_server.cpp
    src/vad/model.cpp
    src/vad/processor.cpp
    include/sip_gateway/config.hpp
    include/sip_gateway/logging.hpp
    include/sip_gateway/backend/client.hpp
    include/sip_gateway/backend/ws_client.hpp
    include/sip_gateway/audio/port.hpp
    include/sip_gateway/audio/player.hpp
    include/sip_gateway/audio/recorder.hpp
    include/sip_gateway/utils/async.hpp
    include/sip_gateway/sip/app.hpp
    include/sip_gateway/sip/account.hpp
    include/sip_gateway/sip/call.hpp
    include/sip_gateway/server/rest_server.hpp
    include/sip_gateway/vad/model.hpp
    include/sip_gateway/vad/processor.hpp
)

target_compile_features(sip_gateway PRIVATE cxx_std_17)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(sip_gateway PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(sip_gateway PRIVATE ${SIPGATEWAY_WARNINGS})
if(TARGET spdlog::spdlog)
    target_link_libraries(sip_gateway PRIVATE spdlog::spdlog)
endif()
if(TARGET nlohmann_json::nlohmann_json)
    target_link_libraries(sip_gateway PRIVATE nlohmann_json::nlohmann_json)
endif()
if(TARGET httplib::httplib)
    target_link_libraries(sip_gateway PRIVATE httplib::httplib)
endif()
if(OpenSSL_FOUND)
    target_link_libraries(sip_gateway PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    target_compile_definitions(sip_gateway PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)
endif()
if(TARGET websocketpp)
    target_link_libraries(sip_gateway PRIVATE websocketpp)
endif()
if(TARGET asio)
    target_link_libraries(sip_gateway PRIVATE asio)
    target_compile_definitions(sip_gateway PRIVATE ASIO_STANDALONE)
endif()
target_link_libraries(sip_gateway PRIVATE pjsip)
if(TARGET pjproject)
    add_dependencies(sip_gateway pjproject)
    if(TARGET pjproject-install)
        add_dependencies(sip_gateway pjproject-install)
    endif()
endif()

if(SIPGATEWAY_FETCH_DEPS)
    FetchContent_GetProperties(spdlog)
    if(spdlog_POPULATED)
        target_include_directories(sip_gateway PRIVATE ${spdlog_SOURCE_DIR}/include)
        include_directories(${spdlog_SOURCE_DIR}/include)
    endif()
    FetchContent_GetProperties(nlohmann_json)
    if(nlohmann_json_POPULATED)
        include_directories(${nlohmann_json_SOURCE_DIR}/include)
    endif()
    FetchContent_GetProperties(httplib)
    if(httplib_POPULATED)
        include_directories(${httplib_SOURCE_DIR})
    endif()
endif()

if(NOT SIPGATEWAY_ONNX_INCLUDE_DIR OR NOT SIPGATEWAY_ONNX_LIB_DIR)
    message(FATAL_ERROR "ONNX Runtime is required. Ensure SIPGATEWAY_BUILD_ONNX is enabled.")
endif()

target_include_directories(sip_gateway PRIVATE ${SIPGATEWAY_ONNX_INCLUDE_DIR})
target_link_directories(sip_gateway PRIVATE ${SIPGATEWAY_ONNX_LIB_DIR})
if(TARGET onnxruntime_iface)
    target_link_libraries(sip_gateway PRIVATE onnxruntime_iface)
endif()
target_link_libraries(sip_gateway PRIVATE onnxruntime)
target_compile_definitions(sip_gateway PRIVATE SIPGATEWAY_HAS_ONNX=1)
if(TARGET onnxruntime_ep-install)
    add_dependencies(sip_gateway onnxruntime_ep-install)
endif()
