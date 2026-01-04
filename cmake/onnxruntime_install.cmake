if(NOT DEFINED SOURCE_DIR OR NOT DEFINED DEST_DIR)
    message(FATAL_ERROR "SOURCE_DIR and DEST_DIR are required")
endif()

set(_onnx_base "${SOURCE_DIR}")
if(DEFINED PACKAGE_NAME)
    if(EXISTS "${SOURCE_DIR}/${PACKAGE_NAME}/include")
        set(_onnx_base "${SOURCE_DIR}/${PACKAGE_NAME}")
    endif()
endif()

if(NOT EXISTS "${_onnx_base}/include")
    message(FATAL_ERROR "ONNX Runtime include dir not found under ${_onnx_base}")
endif()
if(NOT EXISTS "${_onnx_base}/lib")
    message(FATAL_ERROR "ONNX Runtime lib dir not found under ${_onnx_base}")
endif()

file(MAKE_DIRECTORY "${DEST_DIR}/include")
file(MAKE_DIRECTORY "${DEST_DIR}/lib")
file(COPY "${_onnx_base}/include/" DESTINATION "${DEST_DIR}/include")
file(COPY "${_onnx_base}/lib/" DESTINATION "${DEST_DIR}/lib")

if(NOT EXISTS "${DEST_DIR}/lib/libonnxruntime.so")
    file(GLOB _onnx_libs "${DEST_DIR}/lib/libonnxruntime.so*")
    list(LENGTH _onnx_libs _onnx_lib_count)
    if(_onnx_lib_count GREATER 0)
        list(SORT _onnx_libs)
        list(GET _onnx_libs 0 _onnx_target)
        execute_process(
            COMMAND ${CMAKE_COMMAND} -E create_symlink "${_onnx_target}" "${DEST_DIR}/lib/libonnxruntime.so"
        )
    endif()
endif()
