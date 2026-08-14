cmake_minimum_required(VERSION 3.24)

project(SonotideSmokeConsumer LANGUAGES CXX)

find_package(Sonotide CONFIG REQUIRED)

if(NOT TARGET Sonotide::sonotide)
    message(FATAL_ERROR "Sonotide package did not define Sonotide::sonotide")
endif()

add_executable(sonotide_smoke_consumer main.cpp)
target_link_libraries(sonotide_smoke_consumer PRIVATE Sonotide::sonotide)
target_compile_features(sonotide_smoke_consumer PRIVATE cxx_std_20)

include(CTest)
add_test(NAME sonotide_smoke_consumer COMMAND sonotide_smoke_consumer)
