#include <algorithm>
#include <cstddef>
#include <iostream>

#include "sonotide/runtime.h"

namespace {

class silence_callback final : public sonotide::render_callback {
public:
    sonotide::result<void> on_render(
        sonotide::audio_buffer_view buffer,
        sonotide::stream_timestamp) override {
        std::fill(buffer.bytes.begin(), buffer.bytes.end(), std::byte{0});
        return sonotide::result<void>::success();
    }
};

}  // namespace

int main() {
    auto runtime_result = sonotide::runtime::create();
    if (!runtime_result) {
        std::cerr << runtime_result.error().message << '\n';
        return 1;
    }

    sonotide::runtime audio_runtime = std::move(runtime_result.value());
    silence_callback callback;
    sonotide::render_stream_config config;

    auto stream_result = audio_runtime.open_render_stream(config, callback);
    if (!stream_result) {
        std::cerr << stream_result.error().message << '\n';
        return 1;
    }

    auto start_result = stream_result.value().start();
    if (!start_result) {
        std::cerr << start_result.error().message << '\n';
        return 1;
    }

    return 0;
}

