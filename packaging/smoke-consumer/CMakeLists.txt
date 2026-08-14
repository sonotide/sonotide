cmake_minimum_required(VERSION 3.24)

project(SonotideSmokeConsumer LANGUAGES CXX)

if(NOT DEFINED SONOTIDE_EXPECTED_VERSION OR SONOTIDE_EXPECTED_VERSION STREQUAL "")
    message(FATAL_ERROR "SONOTIDE_EXPECTED_VERSION must be provided by package verification")
endif()

find_package(Sonotide ${SONOTIDE_EXPECTED_VERSION} EXACT CONFIG REQUIRED)

if(NOT Sonotide_VERSION STREQUAL SONOTIDE_EXPECTED_VERSION)
    message(FATAL_ERROR
        "Sonotide package reports version '${Sonotide_VERSION}', expected '${SONOTIDE_EXPECTED_VERSION}'")
endif()

if(NOT TARGET Sonotide::sonotide)
    message(FATAL_ERROR "Sonotide package did not define Sonotide::sonotide")
endif()

add_executable(sonotide_smoke_consumer main.cpp)
target_link_libraries(sonotide_smoke_consumer PRIVATE Sonotide::sonotide)
target_compile_features(sonotide_smoke_consumer PRIVATE cxx_std_20)
target_compile_definitions(
    sonotide_smoke_consumer
    PRIVATE SONOTIDE_EXPECTED_VERSION="${SONOTIDE_EXPECTED_VERSION}")

include(CTest)
add_test(NAME sonotide_smoke_consumer COMMAND sonotide_smoke_consumer)
