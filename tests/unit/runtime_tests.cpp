#include <utility>

#include "sonotide/runtime.h"
#include "test_support/test_harness.h"

namespace {

class unused_render_callback final : public sonotide::render_callback {
public:
    sonotide::result<void> on_render(
        sonotide::audio_buffer_view,
        sonotide::stream_timestamp) override {
        return sonotide::result<void>::success();
    }
};

class unused_capture_callback final : public sonotide::capture_callback {
public:
    sonotide::result<void> on_capture(
        sonotide::const_audio_buffer_view,
        sonotide::stream_timestamp) override {
        return sonotide::result<void>::success();
    }
};

template <typename value_type>
void require_invalid_runtime_result(
    const sonotide::result<value_type>& value,
    const char* expected_operation) {
    REQUIRE(!value);
    REQUIRE(value.error().category == sonotide::error_category::initialization);
    REQUIRE(value.error().code == sonotide::error_code::invalid_state);
    REQUIRE(value.error().operation == expected_operation);
}

}  // namespace

int main() {
    auto runtime_result = sonotide::runtime::create();
    REQUIRE(runtime_result);

    sonotide::runtime moved_from = std::move(runtime_result.value());
    sonotide::runtime active = std::move(moved_from);
    (void)active;

    unused_render_callback render_callback;
    unused_capture_callback capture_callback;

    require_invalid_runtime_result(
        moved_from.enumerate_devices(sonotide::device_direction::render),
        "runtime::enumerate_devices");
    require_invalid_runtime_result(
        moved_from.default_device(sonotide::device_direction::render),
        "runtime::default_device");
    require_invalid_runtime_result(
        moved_from.open_render_stream({}, render_callback),
        "runtime::open_render_stream");
    require_invalid_runtime_result(
        moved_from.open_capture_stream({}, capture_callback),
        "runtime::open_capture_stream");
    require_invalid_runtime_result(
        moved_from.open_loopback_stream({}, capture_callback),
        "runtime::open_loopback_stream");
    require_invalid_runtime_result(
        moved_from.open_playback_session(),
        "runtime::open_playback_session");

    return 0;
}
