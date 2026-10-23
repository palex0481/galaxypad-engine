foreach(_relative lib/aurora.cpp lib/window.cpp lib/dolphin/gx/GXFrameBuffer.cpp)
    set(_original "${aurora_SOURCE_DIR}/${_relative}")
    if(_relative STREQUAL "lib/aurora.cpp")
        set(_target aurora_core)
        set(_input "${CMAKE_CURRENT_BINARY_DIR}/allocation-safe/${_relative}")
    elseif(_relative STREQUAL "lib/window.cpp")
        set(_target aurora_core)
        set(_input "${_original}")
    else()
        set(_target aurora_gx)
        set(_input "${_original}")
    endif()
    set(_output "${CMAKE_CURRENT_BINARY_DIR}/present/${_relative}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/patch_aurora_present.py" "${_original}")
    execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/patch_aurora_present.py"
        "${_input}" "${_output}" COMMAND_ERROR_IS_FATAL ANY)
    get_target_property(_sources ${_target} SOURCES)
    list(REMOVE_ITEM _sources "${_relative}" "${_original}" "${_input}")
    list(APPEND _sources "${_output}")
    set_property(TARGET ${_target} PROPERTY SOURCES "${_sources}")
    get_filename_component(_dir "${_original}" DIRECTORY)
    set_source_files_properties("${_output}" TARGET_DIRECTORY ${_target} PROPERTIES
        COMPILE_OPTIONS "-iquote${_dir};-iquote${aurora_SOURCE_DIR}/lib;-iquote${CMAKE_CURRENT_SOURCE_DIR}/present")
endforeach()
target_sources(aurora_core PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/present/present_aurora.cpp")
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/present/present_aurora.cpp" TARGET_DIRECTORY aurora_core PROPERTIES
    COMPILE_OPTIONS "-iquote${aurora_SOURCE_DIR}/lib")
# Stereoscopic window (BETA): the eye IOSurfaces, imported into Dawn as shared texture memory.
target_sources(aurora_core PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/present/stereo_bridge.cpp")
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/present/stereo_bridge.cpp" TARGET_DIRECTORY aurora_core PROPERTIES
    COMPILE_OPTIONS "-iquote${aurora_SOURCE_DIR}/lib")
if(APPLE)
    target_link_libraries(aurora_core PUBLIC "-framework IOSurface" "-framework CoreFoundation")
endif()
# Dawn must be asked for the shared texture memory features when the device is created.
set(_gpu_relative lib/webgpu/gpu.cpp)
set(_gpu_original "${aurora_SOURCE_DIR}/${_gpu_relative}")
set(_gpu_output "${CMAKE_CURRENT_BINARY_DIR}/stereo/${_gpu_relative}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/patch_aurora_stereo.py" "${_gpu_original}")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/patch_aurora_stereo.py"
    "${_gpu_original}" "${_gpu_output}" COMMAND_ERROR_IS_FATAL ANY)
get_target_property(_sources aurora_core SOURCES)
list(REMOVE_ITEM _sources "${_gpu_relative}" "${_gpu_original}")
list(APPEND _sources "${_gpu_output}")
set_property(TARGET aurora_core PROPERTY SOURCES "${_sources}")
get_filename_component(_gpu_dir "${_gpu_original}" DIRECTORY)
set_source_files_properties("${_gpu_output}" TARGET_DIRECTORY aurora_core PROPERTIES
    COMPILE_OPTIONS "-iquote${_gpu_dir};-iquote${aurora_SOURCE_DIR}/lib")
add_library(petari_present STATIC present/present_host.cpp)
target_link_libraries(petari_present PUBLIC petari_platform_vi petari_platform_sc)
if(_petari_build_testing)
    add_test(NAME native_present_patch COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/native/tests/present_patch_tests.py" "${aurora_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}")
endif()
