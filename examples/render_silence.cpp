#include <algorithm>
#include <cstddef>
#include <iostream>

#include "sonotide/runtime.h"

namespace {

// Минимальный обратный вызов рендеринга, который намеренно заполняет буфер тишиной.
class silence_callback final : public sonotide::render_callback {
public:
    // В этой быстрой проверке именно обратный вызов определяет, какие сэмплы попадут в буфер рендеринга.
    sonotide::result<void> on_render(
        sonotide::audio_buffer_view buffer,
        sonotide::stream_timestamp) override {
        std::fill(buffer.bytes.begin(), buffer.bytes.end(), std::byte{0});
        return sonotide::result<void>::success();
    }
};

}  // namespace

int main() {
    // Инициализируем среду выполнения, чтобы пример проходил через реальную Windows-реализацию.
    auto runtime_result = sonotide::runtime::create();
    if (!runtime_result) {
        std::cerr << runtime_result.error().message << '\n';
        return 1;
    }

    sonotide::runtime audio_runtime = std::move(runtime_result.value());
    // Обратный вызов с тишиной удерживает поток в рабочем состоянии, не создавая слышимого сигнала.
    silence_callback callback;
    // Для быстрой проверки достаточно конфигурации по умолчанию.
    sonotide::render_stream_config config;

    // Открываем поток и запускаем его, чтобы фактически проверить путь WASAPI.
    auto stream_result = audio_runtime.open_render_stream(config, callback);
    if (!stream_result) {
        std::cerr << stream_result.error().message << '\n';
        return 1;
    }

    // Если start() прошёл, то критический путь рендеринга уже подтверждён.
    auto start_result = stream_result.value().start();
    if (!start_result) {
        std::cerr << start_result.error().message << '\n';
        return 1;
    }

    return 0;
}
