# Sonotide

Sonotide - это независимый C++20-фреймворк для работы со звуком на Windows.
Он закрывает низкоуровневую часть вокруг WASAPI, а сверху даёт понятный API
для перечисления устройств, открытия потоков вывода, захвата и loopback, а
также для сборки сессий воспроизведения с Media Foundation и встроенным
эквалайзером.

Фреймворк задуман как отдельный пакет, а не как кусок приложения. У него свой
CMake-проект, свои примеры, свои тесты и своя документация. Это удобно, когда
нужно переиспользовать аудиошину в нескольких проектах и не тащить туда
компактный, но всё же сложный Windows-специфичный слой.

## Что умеет

- перечислять аудиоустройства и получать устройство по умолчанию;
- открывать потоки вывода в shared-режиме с событийной моделью WASAPI;
- открывать потоки захвата в shared-режиме с событийной моделью WASAPI;
- открывать loopback-потоки в shared-режиме;
- собирать сессию воспроизведения поверх `runtime` и декодера;
- загружать и декодировать источники через Media Foundation на Windows;
- применять встроенный эквалайзер с динамическим числом полос до 10, пресетами и компенсацией запаса по уровню;
- хранить снимок состояния воспроизведения, включая согласованный формат и активное устройство;
- использовать явную модель ошибок через `sonotide::result<T>`.

На платформах вне Windows проект тоже конфигурируется, но `runtime` там
работает в режиме заглушки и возвращает `unsupported_platform`.

## Структура репозитория

- `include/sonotide/` - публичный API фреймворка;
- `src/` - реализация `runtime`, воспроизведения и внутренних backend-слоёв;
- `examples/` - небольшие программы, показывающие типовые сценарии использования;
- `tests/` - unit-тесты для платформенно-независимых частей;
- `docs/` - архитектура, API, заметки по миграции и архитектурные решения;
- `cmake/` - шаблоны и вспомогательные файлы для сборки и упаковки.

## Сборка

Рекомендуемый путь для Windows и WSL - через preset `msvc-x64-debug`.

```bash
"/mnt/c/Program Files/Microsoft Visual Studio/18/Professional/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --preset msvc-x64-debug
"/mnt/c/Program Files/Microsoft Visual Studio/18/Professional/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build --preset msvc-x64-debug
"/mnt/c/Program Files/Microsoft Visual Studio/18/Professional/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/ctest.exe" --preset msvc-x64-debug
```

Если `cmake` уже доступен в `PATH`, команды те же самые, только без полного
пути к бинарю:

```bash
cmake --preset msvc-x64-debug
cmake --build --preset msvc-x64-debug
ctest --preset msvc-x64-debug
```

Если запускаешь `ctest` вручную против каталога сборки Visual Studio, не забудь
указать конфигурацию:

```bash
ctest --test-dir build/msvc-x64-debug -C Debug
```

## Быстрый пример

Ниже минимальный пример, который открывает поток вывода и пишет в него тишину.
Это не полноценный плеер, а проверка того, что `runtime` и ветка вывода
поднимаются корректно.

```cpp
#include <algorithm>

#include "sonotide/runtime.h"

class silence_callback final : public sonotide::render_callback {
public:
    sonotide::result<void> on_render(
        sonotide::audio_buffer_view buffer,
        sonotide::stream_timestamp) override {
        std::fill(buffer.bytes.begin(), buffer.bytes.end(), std::byte{0});
        return sonotide::result<void>::success();
    }
};

int main() {
    auto runtime_result = sonotide::runtime::create();
    if (!runtime_result) {
        return 1;
    }

    sonotide::runtime runtime = std::move(runtime_result.value());
    silence_callback callback;

    auto stream_result = runtime.open_render_stream({}, callback);
    if (!stream_result) {
        return 1;
    }

    auto start_result = stream_result.value().start();
    return start_result ? 0 : 1;
}
```

Для сценария воспроизведения есть отдельный пример в `examples/playback_session.cpp`.
Он показывает, как фреймворк открывает файл, строит сессию воспроизведения и ведёт
состояние воспроизведения без привязки к конкретному приложению.

## Принципы

- публичный API не выставляет сырой COM наружу;
- жизненный цикл объекта должен читаться без сюрпризов;
- ошибки возвращаются явно, без исключений в горячем пути;
- backend должен оставаться близким к реальному WASAPI-миру, а не прятать его;
- верхний слой воспроизведения не обязан тащить в себя доменную логику приложения.

Подробности по устройству фреймворка лежат в [docs/foundation.md](docs/foundation.md),
[docs/architecture.md](docs/architecture.md) и [docs/api.md](docs/api.md).
