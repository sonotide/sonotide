cmake_minimum_required(VERSION 3.24)

project(Sonotide VERSION 0.1.0 LANGUAGES CXX)

include(CMakePackageConfigHelpers)
include(CTest)
include(GNUInstallDirs)

option(SONOTIDE_BUILD_EXAMPLES "Build Sonotide examples" ON)
option(SONOTIDE_BUILD_TESTS "Build Sonotide unit tests" ON)

add_library(sonotide
    src/error.cpp
    src/runtime.cpp
    src/render_stream.cpp
    src/capture_stream.cpp
    src/loopback_capture_stream.cpp
    src/internal/state_machine.cpp
)

if(WIN32)
    target_sources(sonotide PRIVATE
        src/internal/win/com_scope.cpp
        src/internal/win/device_utils.cpp
        src/internal/win/hresult_utils.cpp
        src/internal/win/wave_format_utils.cpp
        src/internal/win/wasapi_runtime_backend.cpp
        src/internal/win/wasapi_stream_handle.cpp
    )
else()
    target_sources(sonotide PRIVATE
        src/internal/stub/unsupported_runtime_backend.cpp
    )
endif()

add_library(Sonotide::sonotide ALIAS sonotide)

target_compile_features(sonotide PUBLIC cxx_std_20)

target_include_directories(sonotide
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if(MSVC)
    target_compile_options(sonotide PRIVATE /W4 /permissive-)
else()
    target_compile_options(sonotide PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(WIN32)
    target_link_libraries(sonotide
        PRIVATE
            avrt
            mfplat
            mfreadwrite
            mfuuid
            mmdevapi
            ole32
    )
endif()

install(
    TARGETS sonotide
    EXPORT SonotideTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(
    DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(
    EXPORT SonotideTargets
    FILE SonotideTargets.cmake
    NAMESPACE Sonotide::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Sonotide
)

configure_package_config_file(
    cmake/SonotideConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/SonotideConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Sonotide
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/SonotideConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(
    FILES
        ${CMAKE_CURRENT_BINARY_DIR}/SonotideConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/SonotideConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Sonotide
)

if(SONOTIDE_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(BUILD_TESTING AND SONOTIDE_BUILD_TESTS)
    add_subdirectory(tests)
endif()
