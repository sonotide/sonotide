#include <chrono>
#include <iostream>
#include <thread>

#include "sonotide/runtime.h"

int main(int argc, char** argv) {
    // Пример ожидает один URI или путь к файлу.
    if (argc < 2) {
        std::cerr << "Usage: sonotide_playback_session <uri>\n";
        return 1;
    }

    // Создаём runtime, чтобы сессия воспроизведения работала на реальной реализации Sonotide.
    auto runtime_result = sonotide::runtime::create();
    if (!runtime_result) {
        std::cerr << runtime_result.error().message << '\n';
        return 1;
    }

    sonotide::runtime audio_runtime = std::move(runtime_result.value());
    // Открываем именно высокоуровневую сессию воспроизведения, а не низкоуровневый поток рендеринга.
    auto session_result = audio_runtime.open_playback_session();
    if (!session_result) {
        std::cerr << session_result.error().message << '\n';
        return 1;
    }

    // Загружаем источник перед входом в цикл опроса.
    auto load_result = session_result.value().load(argv[1]);
    if (!load_result) {
        std::cerr << load_result.error().message << '\n';
        return 1;
    }
    // Таймаут не даёт примеру зависнуть навсегда на проблемном источнике.
    const auto started_at = std::chrono::steady_clock::now();
    while (true) {
        // Снимок состояния печатаем целиком, чтобы пример был полезен и как быстрая проверка, и как демонстрация.
        const auto state = session_result.value().state();
        std::cout << "status=" << static_cast<int>(state.status)
                  << " position_ms=" << state.position_ms
                  << " duration_ms=" << state.duration_ms << '\n'
                  << std::flush;
        if (state.status == sonotide::playback_status::idle ||
            state.status == sonotide::playback_status::error) {
            break;
        }

        // Период сна даёт внутренней реализации время продвигать воспроизведение и не забивает CPU.
        if (std::chrono::steady_clock::now() - started_at > std::chrono::seconds(30)) {
            std::cerr << "Playback example timed out.\n";
            return 2;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    return 0;
}
