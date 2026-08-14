add_executable(sonotide_list_devices list_devices.cpp)
target_link_libraries(sonotide_list_devices PRIVATE Sonotide::sonotide)

add_executable(sonotide_render_silence render_silence.cpp)
target_link_libraries(sonotide_render_silence PRIVATE Sonotide::sonotide)

add_executable(sonotide_playback_session playback_session.cpp)
target_link_libraries(sonotide_playback_session PRIVATE Sonotide::sonotide)

if(SONOTIDE_ENABLE_ADDRESS_SANITIZER)
    sonotide_copy_asan_runtime(sonotide_list_devices)
    sonotide_copy_asan_runtime(sonotide_render_silence)
    sonotide_copy_asan_runtime(sonotide_playback_session)
endif()
