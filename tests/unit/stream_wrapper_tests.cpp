#include <cassert>
#include <memory>
#include <string>
#include <utility>

#include "internal/runtime_backend.h"

namespace {

sonotide::error make_stream_error(
    const sonotide::error_code code,
    std::string operation,
    std::string message) {
    sonotide::error failure;
    failure.category = sonotide::error_category::stream;
    failure.code = code;
    failure.operation = std::move(operation);
    failure.message = std::move(message);
    return failure;
}

class fake_stream_handle final : public sonotide::detail::stream_handle {
public:
    sonotide::result<void> start() override {
        ++start_calls;
        return start_result;
    }

    sonotide::result<void> stop() override {
        ++stop_calls;
        return stop_result;
    }

    sonotide::result<void> reset() override {
        ++reset_calls;
        return reset_result;
    }

    sonotide::result<void> close() override {
        ++close_calls;
        return close_result;
    }

    [[nodiscard]] sonotide::stream_status status() const override {
        return current_status;
    }

    sonotide::result<void> start_result = sonotide::result<void>::success();
    sonotide::result<void> stop_result = sonotide::result<void>::success();
    sonotide::result<void> reset_result = sonotide::result<void>::success();
    sonotide::result<void> close_result = sonotide::result<void>::success();
    sonotide::stream_status current_status{};
    int start_calls = 0;
    int stop_calls = 0;
    int reset_calls = 0;
    int close_calls = 0;
};

void assert_invalid_state_error(
    const sonotide::result<void>& result_value,
    const std::string& expected_operation) {
    assert(!result_value.has_value());
    assert(result_value.error().category == sonotide::error_category::stream);
    assert(result_value.error().code == sonotide::error_code::invalid_state);
    assert(result_value.error().operation == expected_operation);
}

}  // namespace

int main() {
    // Пустые stream-обёртки должны возвращать invalid_state и closed-status.
    sonotide::render_stream empty_render;
    assert(!empty_render.is_open());
    assert(empty_render.status().state == sonotide::stream_state::closed);
    assert_invalid_state_error(empty_render.start(), "render_stream::start");
    assert_invalid_state_error(empty_render.stop(), "render_stream::stop");
    assert_invalid_state_error(empty_render.reset(), "render_stream::reset");
    assert_invalid_state_error(empty_render.close(), "render_stream::close");

    sonotide::capture_stream empty_capture;
    assert(!empty_capture.is_open());
    assert(empty_capture.status().state == sonotide::stream_state::closed);
    assert_invalid_state_error(empty_capture.start(), "capture_stream::start");
    assert_invalid_state_error(empty_capture.stop(), "capture_stream::stop");
    assert_invalid_state_error(empty_capture.reset(), "capture_stream::reset");
    assert_invalid_state_error(empty_capture.close(), "capture_stream::close");

    sonotide::loopback_capture_stream empty_loopback;
    assert(!empty_loopback.is_open());
    assert(empty_loopback.status().state == sonotide::stream_state::closed);
    assert_invalid_state_error(empty_loopback.start(), "loopback_capture_stream::start");
    assert_invalid_state_error(empty_loopback.stop(), "loopback_capture_stream::stop");
    assert_invalid_state_error(empty_loopback.reset(), "loopback_capture_stream::reset");
    assert_invalid_state_error(empty_loopback.close(), "loopback_capture_stream::close");

    // Привязанные wrappers должны форвардить успешные вызовы в handle.
    auto render_handle = std::make_shared<fake_stream_handle>();
    render_handle->current_status.state = sonotide::stream_state::running;
    render_handle->current_status.statistics.callback_count = 12;
    auto render = sonotide::detail::make_render_stream(render_handle);
    assert(render.is_open());
    assert(render.status().state == sonotide::stream_state::running);
    assert(render.status().statistics.callback_count == 12);
    assert(render.start().has_value());
    assert(render.stop().has_value());
    assert(render.reset().has_value());
    assert(render.close().has_value());
    assert(render_handle->start_calls == 1);
    assert(render_handle->stop_calls == 1);
    assert(render_handle->reset_calls == 1);
    assert(render_handle->close_calls == 1);

    // Ошибки из handle должны пробрасываться без подмены.
    auto capture_handle = std::make_shared<fake_stream_handle>();
    capture_handle->start_result = sonotide::result<void>::failure(make_stream_error(
        sonotide::error_code::stream_start_failed,
        "fake_capture_start",
        "capture failed to start"));
    capture_handle->stop_result = sonotide::result<void>::failure(make_stream_error(
        sonotide::error_code::stream_stop_failed,
        "fake_capture_stop",
        "capture failed to stop"));
    auto capture = sonotide::detail::make_capture_stream(capture_handle);
    const auto capture_start_result = capture.start();
    assert(!capture_start_result.has_value());
    assert(capture_start_result.error().operation == "fake_capture_start");
    const auto capture_stop_result = capture.stop();
    assert(!capture_stop_result.has_value());
    assert(capture_stop_result.error().operation == "fake_capture_stop");
    assert(capture_handle->start_calls == 1);
    assert(capture_handle->stop_calls == 1);

    auto loopback_handle = std::make_shared<fake_stream_handle>();
    loopback_handle->reset_result = sonotide::result<void>::failure(make_stream_error(
        sonotide::error_code::invalid_state,
        "fake_loopback_reset",
        "loopback reset rejected"));
    loopback_handle->close_result = sonotide::result<void>::failure(make_stream_error(
        sonotide::error_code::stream_stop_failed,
        "fake_loopback_close",
        "loopback close rejected"));
    auto loopback = sonotide::detail::make_loopback_capture_stream(loopback_handle);
    const auto loopback_reset_result = loopback.reset();
    assert(!loopback_reset_result.has_value());
    assert(loopback_reset_result.error().operation == "fake_loopback_reset");
    const auto loopback_close_result = loopback.close();
    assert(!loopback_close_result.has_value());
    assert(loopback_close_result.error().operation == "fake_loopback_close");
    assert(loopback_handle->reset_calls == 1);
    assert(loopback_handle->close_calls == 1);
    return 0;
}
