#include <atomic>
#include <barrier>
#include <latch>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "internal/runtime_backend.h"
#include "test_support/test_harness.h"

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
        close_calls.fetch_add(1, std::memory_order_relaxed);
        if (close_entered != nullptr &&
            !close_entered_signaled.exchange(true, std::memory_order_relaxed)) {
            close_entered->count_down();
        }
        if (close_gate != nullptr) {
            close_gate->arrive_and_wait();
        }
        if (throw_on_close) {
            throw std::runtime_error("synthetic close failure");
        }
        if (close_result && !closed.exchange(true, std::memory_order_relaxed)) {
            close_effects.fetch_add(1, std::memory_order_relaxed);
        }
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
    std::atomic<int> start_calls{0};
    std::atomic<int> stop_calls{0};
    std::atomic<int> reset_calls{0};
    std::atomic<int> close_calls{0};
    std::atomic<int> close_effects{0};
    std::atomic<bool> closed{false};
    std::atomic<bool> close_entered_signaled{false};
    bool throw_on_close = false;
    std::barrier<>* close_gate = nullptr;
    std::latch* close_entered = nullptr;
};

void assert_invalid_state_error(
    const sonotide::result<void>& result_value,
    const std::string& expected_operation) {
    REQUIRE(!result_value.has_value());
    REQUIRE(result_value.error().category == sonotide::error_category::stream);
    REQUIRE(result_value.error().code == sonotide::error_code::invalid_state);
    REQUIRE(result_value.error().operation == expected_operation);
}

template <typename stream_type>
void verify_concurrent_close_and_status(
    stream_type& stream,
    const std::shared_ptr<fake_stream_handle>& handle) {
    constexpr int close_thread_count = 4;
    constexpr int status_thread_count = 4;
    std::barrier close_gate(close_thread_count);
    std::barrier start_gate(close_thread_count + status_thread_count + 1);
    handle->close_gate = &close_gate;

    std::vector<std::thread> workers;
    workers.reserve(close_thread_count + status_thread_count);
    for (int index = 0; index < close_thread_count; ++index) {
        workers.emplace_back([&]() {
            start_gate.arrive_and_wait();
            const auto closed = stream.close();
            REQUIRE(closed.has_value());
        });
    }
    for (int index = 0; index < status_thread_count; ++index) {
        workers.emplace_back([&]() {
            start_gate.arrive_and_wait();
            for (int iteration = 0; iteration < 1000; ++iteration) {
                const auto snapshot = stream.status();
                REQUIRE(snapshot.state == sonotide::stream_state::running ||
                        snapshot.state == sonotide::stream_state::closed);
                (void)stream.is_open();
            }
        });
    }

    start_gate.arrive_and_wait();
    for (auto& worker : workers) {
        worker.join();
    }
    handle->close_gate = nullptr;
    REQUIRE(!stream.is_open());
    REQUIRE(handle->close_calls == close_thread_count);
    REQUIRE(handle->close_effects == 1);
}

}  // namespace

int main() {
    // Пустые stream-обёртки должны возвращать invalid_state и closed-status.
    sonotide::render_stream empty_render;
    REQUIRE(!empty_render.is_open());
    REQUIRE(empty_render.status().state == sonotide::stream_state::closed);
    assert_invalid_state_error(empty_render.start(), "render_stream::start");
    assert_invalid_state_error(empty_render.stop(), "render_stream::stop");
    assert_invalid_state_error(empty_render.reset(), "render_stream::reset");
    assert_invalid_state_error(empty_render.close(), "render_stream::close");

    sonotide::capture_stream empty_capture;
    REQUIRE(!empty_capture.is_open());
    REQUIRE(empty_capture.status().state == sonotide::stream_state::closed);
    assert_invalid_state_error(empty_capture.start(), "capture_stream::start");
    assert_invalid_state_error(empty_capture.stop(), "capture_stream::stop");
    assert_invalid_state_error(empty_capture.reset(), "capture_stream::reset");
    assert_invalid_state_error(empty_capture.close(), "capture_stream::close");

    sonotide::loopback_capture_stream empty_loopback;
    REQUIRE(!empty_loopback.is_open());
    REQUIRE(empty_loopback.status().state == sonotide::stream_state::closed);
    assert_invalid_state_error(empty_loopback.start(), "loopback_capture_stream::start");
    assert_invalid_state_error(empty_loopback.stop(), "loopback_capture_stream::stop");
    assert_invalid_state_error(empty_loopback.reset(), "loopback_capture_stream::reset");
    assert_invalid_state_error(empty_loopback.close(), "loopback_capture_stream::close");

    // Привязанные wrappers должны форвардить успешные вызовы в handle.
    auto render_handle = std::make_shared<fake_stream_handle>();
    render_handle->current_status.state = sonotide::stream_state::running;
    render_handle->current_status.statistics.callback_count = 12;
    auto render = sonotide::detail::make_render_stream(render_handle);
    REQUIRE(render.is_open());
    REQUIRE(render.status().state == sonotide::stream_state::running);
    REQUIRE(render.status().statistics.callback_count == 12);
    REQUIRE(render.start().has_value());
    REQUIRE(render.stop().has_value());
    REQUIRE(render.reset().has_value());
    REQUIRE(render.close().has_value());
    REQUIRE(!render.is_open());
    REQUIRE(render_handle->start_calls == 1);
    REQUIRE(render_handle->stop_calls == 1);
    REQUIRE(render_handle->reset_calls == 1);
    REQUIRE(render_handle->close_calls == 1);

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
    REQUIRE(!capture_start_result.has_value());
    REQUIRE(capture_start_result.error().operation == "fake_capture_start");
    const auto capture_stop_result = capture.stop();
    REQUIRE(!capture_stop_result.has_value());
    REQUIRE(capture_stop_result.error().operation == "fake_capture_stop");
    REQUIRE(capture_handle->start_calls == 1);
    REQUIRE(capture_handle->stop_calls == 1);

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
    REQUIRE(!loopback_reset_result.has_value());
    REQUIRE(loopback_reset_result.error().operation == "fake_loopback_reset");
    const auto loopback_close_result = loopback.close();
    REQUIRE(!loopback_close_result.has_value());
    REQUIRE(loopback_close_result.error().operation == "fake_loopback_close");
    REQUIRE(loopback.is_open());
    REQUIRE(loopback_handle->reset_calls == 1);
    REQUIRE(loopback_handle->close_calls == 1);

    // An asynchronous self-close-style failure must keep the wrapper attached so an
    // external caller can wait for completion and retry close successfully.
    auto deferred_close_handle = std::make_shared<fake_stream_handle>();
    deferred_close_handle->close_result = sonotide::result<void>::failure(make_stream_error(
        sonotide::error_code::invalid_state,
        "stream::close",
        "Close was requested from the audio callback and cannot complete synchronously."));
    auto deferred_close_stream = sonotide::detail::make_render_stream(deferred_close_handle);
    const auto deferred_close_result = deferred_close_stream.close();
    REQUIRE(!deferred_close_result.has_value());
    REQUIRE(deferred_close_result.error().code == sonotide::error_code::invalid_state);
    REQUIRE(deferred_close_stream.is_open());
    REQUIRE(deferred_close_handle->close_calls == 1);

    deferred_close_handle->close_result = sonotide::result<void>::success();
    REQUIRE(deferred_close_stream.close().has_value());
    REQUIRE(!deferred_close_stream.is_open());
    REQUIRE(deferred_close_handle->close_calls == 2);
    REQUIRE(deferred_close_handle->close_effects == 1);

    // RAII destruction must synchronously close the bound backend handle.
    auto destructor_handle = std::make_shared<fake_stream_handle>();
    {
        auto destructor_stream = sonotide::detail::make_render_stream(destructor_handle);
        REQUIRE(destructor_handle->close_calls == 0);
    }
    REQUIRE(destructor_handle->close_calls == 1);
    REQUIRE(destructor_handle->close_effects == 1);

    // Move-assignment closes the displaced handle before accepting the new one.
    auto displaced_handle = std::make_shared<fake_stream_handle>();
    auto replacement_handle = std::make_shared<fake_stream_handle>();
    {
        auto destination = sonotide::detail::make_capture_stream(displaced_handle);
        auto source = sonotide::detail::make_capture_stream(replacement_handle);
        destination = std::move(source);
        REQUIRE(displaced_handle->close_calls == 1);
        REQUIRE(displaced_handle->close_effects == 1);
        REQUIRE(replacement_handle->close_calls == 0);
    }
    REQUIRE(replacement_handle->close_calls == 1);
    REQUIRE(replacement_handle->close_effects == 1);

    // Explicit close detaches the wrapper, so destruction cannot close the backend twice.
    auto explicit_close_handle = std::make_shared<fake_stream_handle>();
    {
        auto explicit_close_stream =
            sonotide::detail::make_loopback_capture_stream(explicit_close_handle);
        REQUIRE(explicit_close_stream.close().has_value());
        REQUIRE(!explicit_close_stream.is_open());
        REQUIRE(explicit_close_handle->close_calls == 1);
        REQUIRE(explicit_close_handle->close_effects == 1);
    }
    REQUIRE(explicit_close_handle->close_calls == 1);
    REQUIRE(explicit_close_handle->close_effects == 1);

    // Implicit noexcept cleanup must contain unexpected backend exceptions.
    auto throwing_destructor_handle = std::make_shared<fake_stream_handle>();
    throwing_destructor_handle->throw_on_close = true;
    {
        auto throwing_stream =
            sonotide::detail::make_render_stream(throwing_destructor_handle);
        REQUIRE(throwing_stream.is_open());
    }
    REQUIRE(throwing_destructor_handle->close_calls == 1);

    // Atomic wrapper ownership keeps concurrent close/status/is_open free of shared_ptr races.
    auto concurrent_render_handle = std::make_shared<fake_stream_handle>();
    concurrent_render_handle->current_status.state = sonotide::stream_state::running;
    auto concurrent_render = sonotide::detail::make_render_stream(concurrent_render_handle);
    verify_concurrent_close_and_status(concurrent_render, concurrent_render_handle);

    auto concurrent_capture_handle = std::make_shared<fake_stream_handle>();
    concurrent_capture_handle->current_status.state = sonotide::stream_state::running;
    auto concurrent_capture = sonotide::detail::make_capture_stream(concurrent_capture_handle);
    verify_concurrent_close_and_status(concurrent_capture, concurrent_capture_handle);

    auto concurrent_loopback_handle = std::make_shared<fake_stream_handle>();
    concurrent_loopback_handle->current_status.state = sonotide::stream_state::running;
    auto concurrent_loopback =
        sonotide::detail::make_loopback_capture_stream(concurrent_loopback_handle);
    verify_concurrent_close_and_status(concurrent_loopback, concurrent_loopback_handle);

    // A close racing with move-assignment must never erase the replacement handle.
    auto racing_old_handle = std::make_shared<fake_stream_handle>();
    auto racing_new_handle = std::make_shared<fake_stream_handle>();
    racing_new_handle->current_status.state = sonotide::stream_state::prepared;
    std::barrier racing_close_gate(2);
    std::latch racing_close_entered(1);
    racing_old_handle->close_gate = &racing_close_gate;
    racing_old_handle->close_entered = &racing_close_entered;
    auto racing_destination = sonotide::detail::make_render_stream(racing_old_handle);
    auto racing_source = sonotide::detail::make_render_stream(racing_new_handle);
    std::thread racing_close([&]() {
        REQUIRE(racing_destination.close().has_value());
    });
    racing_close_entered.wait();
    racing_old_handle->close_entered = nullptr;
    racing_destination = std::move(racing_source);
    racing_close.join();
    racing_old_handle->close_gate = nullptr;
    REQUIRE(racing_destination.is_open());
    REQUIRE(racing_destination.status().state == sonotide::stream_state::prepared);
    REQUIRE(racing_old_handle->close_effects == 1);
    REQUIRE(racing_destination.close().has_value());
    REQUIRE(racing_new_handle->close_effects == 1);
    return 0;
}
