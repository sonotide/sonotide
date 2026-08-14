add_executable(sonotide_result_tests unit/result_tests.cpp)
target_link_libraries(sonotide_result_tests PRIVATE Sonotide::sonotide)
target_include_directories(sonotide_result_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME sonotide_result_tests COMMAND sonotide_result_tests)

add_executable(sonotide_state_machine_tests unit/state_machine_tests.cpp)
target_link_libraries(sonotide_state_machine_tests PRIVATE Sonotide::sonotide)
target_include_directories(sonotide_state_machine_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
add_test(NAME sonotide_state_machine_tests COMMAND sonotide_state_machine_tests)

add_executable(sonotide_equalizer_tests unit/equalizer_tests.cpp)
target_link_libraries(sonotide_equalizer_tests PRIVATE Sonotide::sonotide)
target_include_directories(sonotide_equalizer_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME sonotide_equalizer_tests COMMAND sonotide_equalizer_tests)

add_executable(sonotide_dsp_tests unit/dsp_tests.cpp)
target_link_libraries(sonotide_dsp_tests PRIVATE Sonotide::sonotide)
target_include_directories(sonotide_dsp_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
add_test(NAME sonotide_dsp_tests COMMAND sonotide_dsp_tests)

add_executable(sonotide_stream_wrapper_tests unit/stream_wrapper_tests.cpp)
target_link_libraries(sonotide_stream_wrapper_tests PRIVATE Sonotide::sonotide)
target_include_directories(sonotide_stream_wrapper_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
add_test(NAME sonotide_stream_wrapper_tests COMMAND sonotide_stream_wrapper_tests)

add_executable(sonotide_playback_session_tests unit/playback_session_tests.cpp)
target_link_libraries(sonotide_playback_session_tests PRIVATE Sonotide::sonotide)
target_include_directories(sonotide_playback_session_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
add_test(NAME sonotide_playback_session_tests COMMAND sonotide_playback_session_tests)

add_executable(sonotide_runtime_tests unit/runtime_tests.cpp)
target_link_libraries(sonotide_runtime_tests PRIVATE Sonotide::sonotide)
target_include_directories(sonotide_runtime_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME sonotide_runtime_tests COMMAND sonotide_runtime_tests)

if(WIN32)
    add_executable(sonotide_win_validation_tests unit/win_validation_tests.cpp)
    target_link_libraries(sonotide_win_validation_tests PRIVATE Sonotide::sonotide)
    target_include_directories(sonotide_win_validation_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
    )
    add_test(NAME sonotide_win_validation_tests COMMAND sonotide_win_validation_tests)
endif()

# A concurrency regression must fail the build instead of holding an unattended
# CTest/CI run forever. Individual tests use shorter deterministic waits for
# diagnostics; this is the final process-level safety net.
set_tests_properties(
    sonotide_result_tests
    sonotide_state_machine_tests
    sonotide_equalizer_tests
    sonotide_dsp_tests
    sonotide_stream_wrapper_tests
    sonotide_playback_session_tests
    sonotide_runtime_tests
    PROPERTIES TIMEOUT 30)

if(WIN32)
    set_tests_properties(sonotide_win_validation_tests PROPERTIES TIMEOUT 30)
endif()
