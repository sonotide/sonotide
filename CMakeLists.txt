cmake_minimum_required(VERSION 3.24)

# project(...) задаёт базовые свойства всего CMake-проекта:
# - Sonotide: имя проекта; используется в сообщениях CMake и package metadata.
# - VERSION 0.1.0: версия проекта; позже её можно переиспользовать в install/package файлах.
# - LANGUAGES CXX: подключаем только язык C++, без C/RC/других языков.
project(Sonotide VERSION 0.1.0 LANGUAGES CXX)

# include(...) подгружает встроенные модули CMake:
# - CMakePackageConfigHelpers: helpers для генерации SonotideConfig.cmake и version-файла.
# - CTest: добавляет инфраструктуру тестов и переменную BUILD_TESTING.
# - GNUInstallDirs: даёт стандартные install-переменные вроде lib/include/bin.
include(CMakePackageConfigHelpers)
include(CTest)
include(GNUInstallDirs)

# option(<name> <help> <default>) создаёт bool-переменную CMake.
# Пользователь или preset могут переопределить её через -D...=ON/OFF.
# Здесь это нужно, чтобы можно было собирать только библиотеку без examples/tests.
option(SONOTIDE_BUILD_EXAMPLES "Build Sonotide examples" ON)
option(SONOTIDE_BUILD_TESTS "Build Sonotide unit tests" ON)

# add_library(<name> STATIC <sources...>) создаёт именно статическую библиотеку.
# Мы фиксируем STATIC явно, чтобы BUILD_SHARED_LIBS не мог случайно превратить
# Sonotide в DLL-сборку без полноценной export/import поддержки публичного API.
# Target в modern CMake - это главный объект, на который потом навешиваются:
# include directories, compile options, compile features, link libraries и install rules.
# Здесь перечислена только платформенно-независимая часть библиотеки.
add_library(sonotide STATIC
    src/error.cpp
    src/equalizer.cpp
    src/runtime.cpp
    src/playback_session.cpp
    src/capture_stream.cpp
    src/render_stream.cpp
    src/loopback_capture_stream.cpp
    src/internal/state_machine.cpp
    src/internal/dsp/parameter_smoother.cpp
    src/internal/dsp/biquad_filter.cpp
    src/internal/dsp/output_headroom_controller.cpp
    src/internal/dsp/equalizer_chain.cpp
)

# if(WIN32) проверяет целевую платформу сборки, а не просто текущую оболочку.
# target_sources(<target> PRIVATE ...) добавляет дополнительные исходники в уже созданный target.
# PRIVATE означает: эти файлы относятся только к реализации sonotide и не влияют на потребителя.
# На Windows подключаем реальный backend, на остальных платформах - stub.
if(WIN32)
    target_sources(sonotide PRIVATE
        src/internal/win/media_foundation_decoder.cpp
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

# ALIAS создаёт второе имя для уже существующего target-а.
# Это удобно, потому что:
# - внутри проекта можно продолжать использовать короткое имя sonotide;
# - снаружи красивее и привычнее ссылаться на Sonotide::sonotide.
# Такой namespace-style особенно хорошо сочетается с find_package.
add_library(Sonotide::sonotide ALIAS sonotide)

# target_compile_features сообщает CMake, какие возможности языка обязательны.
# PUBLIC означает:
# - самому sonotide нужен C++20;
# - потребителям sonotide тоже нужен C++20, так как они включают наши headers.
target_compile_features(sonotide PUBLIC cxx_std_20)

# target_include_directories задаёт пути поиска заголовков для конкретного target-а.
# PUBLIC и PRIVATE - ключевая часть modern CMake:
# - PRIVATE: нужно только самой библиотеке при её сборке;
# - PUBLIC: нужно и библиотеке, и тем, кто к ней линкуется.
# BUILD_INTERFACE/INSTALL_INTERFACE - генераторные выражения:
# - первое используется пока работаем прямо из исходников;
# - второе используется после cmake --install и find_package.
target_include_directories(sonotide
    PUBLIC
        # При сборке из исходников публичные заголовки берутся прямо из include/.
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        # После установки пакета include path должен указывать уже на install tree.
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        # Внутренние заголовки реализации нужны только самой библиотеке.
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# target_compile_options добавляет флаги компилятора.
# Здесь они PRIVATE, потому что это наши внутренние правила сборки,
# а не обязательные требования для чужого приложения.
# /W4, /permissive-, /FS - флаги MSVC; -Wall/-Wextra/-Wpedantic - типичный набор для GCC/Clang.
if(MSVC)
    target_compile_options(sonotide PRIVATE /W4 /permissive- /FS)
else()
    target_compile_options(sonotide PRIVATE -Wall -Wextra -Wpedantic)
endif()

# target_link_libraries связывает target с другими библиотеками.
# В данном случае это системные Windows SDK библиотеки, нужные backend-у:
# mfplat/mfreadwrite/mfuuid - Media Foundation,
# mmdevapi - Windows audio devices,
# ole32 - COM,
# avrt - audio thread priority helpers.
# PRIVATE означает, что эти зависимости нужны реализации sonotide, а пользователь
# по-прежнему просто линкуется к одному target-у Sonotide::sonotide.
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

# install(TARGETS ...) описывает, что попадёт в install tree при cmake --install.
# Важные аргументы:
# - TARGETS sonotide: устанавливаем именно эту библиотеку;
# - EXPORT SonotideTargets: добавляем её в экспортируемый набор targets для package config;
# - ARCHIVE DESTINATION: куда класть static/import libraries (.lib, .a);
# - LIBRARY DESTINATION: куда класть shared libraries на Unix-подобных системах;
# - RUNTIME DESTINATION: куда класть .dll и .exe на Windows.
install(
    TARGETS sonotide
    EXPORT SonotideTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# install(DIRECTORY ...) копирует каталог целиком в install prefix.
# Здесь это будущая SDK-часть, чтобы после установки были доступны headers:
# <prefix>/include/sonotide/...
install(
    DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# install(EXPORT ...) генерирует CMake-файл с описанием экспортированных targets.
# Важные аргументы:
# - EXPORT SonotideTargets: используем тот же export-set, что и выше в install(TARGETS ...);
# - FILE SonotideTargets.cmake: имя выходного файла;
# - NAMESPACE Sonotide::: target-ы внутри файла будут доступны как Sonotide::...;
# - DESTINATION: где этот файл окажется внутри install tree.
# Именно этот шаг позволяет внешнему проекту получить target Sonotide::sonotide через find_package.
install(
    EXPORT SonotideTargets
    FILE SonotideTargets.cmake
    NAMESPACE Sonotide::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Sonotide
)

# configure_package_config_file(...) делает главный пакетный config для find_package.
# Аргументы здесь означают:
# - cmake/SonotideConfig.cmake.in: шаблон, который лежит в репозитории;
# - ${CMAKE_CURRENT_BINARY_DIR}/SonotideConfig.cmake: куда сгенерировать готовый файл;
# - INSTALL_DESTINATION: где этот config будет лежать после установки.
# Потом внешний проект сможет делать find_package(Sonotide CONFIG REQUIRED).
configure_package_config_file(
    cmake/SonotideConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/SonotideConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Sonotide
)

# write_basic_package_version_file(...) генерирует версионный companion-файл для пакета.
# VERSION ${PROJECT_VERSION} берёт версию из project(...).
# COMPATIBILITY SameMajorVersion означает, что версии с тем же major считаются совместимыми.
# Это нужно, если потребитель пишет find_package(Sonotide 0.1 REQUIRED).
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/SonotideConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

# Устанавливаем оба package metadata файла рядом с SonotideTargets.cmake.
# Обычно итоговая структура после install будет такой:
# <prefix>/lib/cmake/Sonotide/
#   SonotideConfig.cmake
#   SonotideConfigVersion.cmake
#   SonotideTargets.cmake
install(
    FILES
        ${CMAKE_CURRENT_BINARY_DIR}/SonotideConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/SonotideConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Sonotide
)

# add_subdirectory(...) говорит CMake зайти в подпроект и прочитать его CMakeLists.txt.
# Так examples/ и tests/ становятся частью общего build graph.
# Условие вокруг них позволяет не тянуть лишние target-ы, когда нужна только библиотека.
if(SONOTIDE_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(BUILD_TESTING AND SONOTIDE_BUILD_TESTS)
    add_subdirectory(tests)
endif()
