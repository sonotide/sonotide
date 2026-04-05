#define _ALLOW_KEYWORD_MACROS
#define private public
#include "sonotide/runtime.h"
#undef private

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "internal/runtime_backend.h"

namespace sonotide {

runtime::runtime(std::shared_ptr<detail::runtime_backend> backend) noexcept
    : backend_(std::move(backend)) {}

}  // namespace sonotide

namespace {

constexpr float kEpsilon = 0.01F;

[[noreturn]] void fail_check(const char* expression, const char* file, const int line) {
    std::cerr << file << ':' << line << ": check failed: " << expression << '\n';
    std::abort();
}

#define REQUIRE(condition) \
    do { \
        if (!(condition)) { \
            fail_check(#condition, __FILE__, __LINE__); \
        } \
    } while (false)

bool approximately_equal(const float left, const float right, const float epsilon = kEpsilon) {
    return std::fabs(left - right) <= epsilon;
}

bool equalizer_states_match(
    const sonotide::equalizer_state& left,
    const sonotide::equalizer_state& right,
    const float epsilon = kEpsilon) {
    if (left.status != right.status ||
        left.enabled != right.enabled ||
        left.active_preset_id != right.active_preset_id ||
        left.bands.size() != right.bands.size() ||
        left.last_nonflat_band_gains_db.size() != right.last_nonflat_band_gains_db.size() ||
        !approximately_equal(left.output_gain_db, right.output_gain_db, epsilon) ||
        !approximately_equal(left.headroom_compensation_db, right.headroom_compensation_db, epsilon) ||
        left.error_message != right.error_message) {
        return false;
    }

    for (std::size_t index = 0; index < left.bands.size(); ++index) {
        if (!approximately_equal(left.bands[index].center_frequency_hz, right.bands[index].center_frequency_hz, epsilon) ||
            !approximately_equal(left.bands[index].gain_db, right.bands[index].gain_db, epsilon) ||
            !approximately_equal(left.bands[index].q_value, right.bands[index].q_value, epsilon)) {
            return false;
        }
    }

    for (std::size_t index = 0; index < left.last_nonflat_band_gains_db.size(); ++index) {
        if (!approximately_equal(
                left.last_nonflat_band_gains_db[index],
                right.last_nonflat_band_gains_db[index],
                epsilon)) {
            return false;
        }
    }

    return true;
}

sonotide::error make_error(
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
        started_ = true;
        closed_ = false;
        return sonotide::result<void>::success();
    }

    sonotide::result<void> stop() override {
        started_ = false;
        return sonotide::result<void>::success();
    }

    sonotide::result<void> reset() override {
        return sonotide::result<void>::success();
    }

    sonotide::result<void> close() override {
        started_ = false;
        closed_ = true;
        return sonotide::result<void>::success();
    }

    [[nodiscard]] sonotide::stream_status status() const override {
        sonotide::stream_status snapshot;
        snapshot.state = closed_
            ? sonotide::stream_state::closed
            : (started_ ? sonotide::stream_state::running : sonotide::stream_state::stopped);
        return snapshot;
    }

private:
    bool started_ = false;
    bool closed_ = false;
};

class fake_runtime_backend final : public sonotide::detail::runtime_backend {
public:
    fake_runtime_backend()
        : handle_(std::make_shared<fake_stream_handle>()) {}

    [[nodiscard]] sonotide::result<std::vector<sonotide::device_info>> enumerate_devices(
        sonotide::device_direction) const override {
        return sonotide::result<std::vector<sonotide::device_info>>::success({});
    }

    [[nodiscard]] sonotide::result<sonotide::device_info> default_device(
        sonotide::device_direction,
        sonotide::device_role) const override {
        sonotide::device_info device;
        device.id = "fake-device";
        device.friendly_name = "Fake Device";
        return sonotide::result<sonotide::device_info>::success(std::move(device));
    }

    [[nodiscard]] sonotide::result<std::shared_ptr<sonotide::detail::stream_handle>> open_render_stream(
        const sonotide::render_stream_config&,
        sonotide::render_callback& callback) override {
        render_callback_ = &callback;
        return sonotide::result<std::shared_ptr<sonotide::detail::stream_handle>>::success(handle_);
    }

    [[nodiscard]] sonotide::result<std::shared_ptr<sonotide::detail::stream_handle>> open_capture_stream(
        const sonotide::capture_stream_config&,
        sonotide::capture_callback&) override {
        return sonotide::result<std::shared_ptr<sonotide::detail::stream_handle>>::failure(make_error(
            sonotide::error_code::not_implemented,
            "fake_runtime_backend::open_capture_stream",
            "Capture is not implemented in the fake runtime backend."));
    }

    [[nodiscard]] sonotide::result<std::shared_ptr<sonotide::detail::stream_handle>> open_loopback_stream(
        const sonotide::loopback_stream_config&,
        sonotide::capture_callback&) override {
        return sonotide::result<std::shared_ptr<sonotide::detail::stream_handle>>::failure(make_error(
            sonotide::error_code::not_implemented,
            "fake_runtime_backend::open_loopback_stream",
            "Loopback is not implemented in the fake runtime backend."));
    }

    sonotide::result<void> emit_render(const sonotide::audio_format& format, const std::uint32_t frame_count) {
        if (render_callback_ == nullptr) {
            return sonotide::result<void>::failure(make_error(
                sonotide::error_code::invalid_state,
                "fake_runtime_backend::emit_render",
                "Render callback is not attached."));
        }

        std::vector<std::byte> bytes(
            static_cast<std::size_t>(frame_count) *
            static_cast<std::size_t>(format.channel_count) *
            sizeof(float));
        return render_callback_->on_render(
            sonotide::audio_buffer_view{bytes, frame_count, format},
            sonotide::stream_timestamp{});
    }

private:
    std::shared_ptr<fake_stream_handle> handle_;
    sonotide::render_callback* render_callback_ = nullptr;
};

}  // namespace

int main() {
    auto backend = std::make_shared<fake_runtime_backend>();
    sonotide::runtime runtime(backend);

    auto session_result = runtime.open_playback_session({});
    REQUIRE(session_result);
    auto session = std::move(session_result.value());

    const std::array<float, 3> frequencies_hz{{120.0F, 1000.0F, 8000.0F}};

    // Пока sample rate не известна, session-level sampling должен корректно отказываться.
    auto early_curve_result = session.sample_equalizer_response(frequencies_hz);
    REQUIRE(!early_curve_result);
    REQUIRE(early_curve_result.error().code == sonotide::error_code::invalid_state);

    sonotide::equalizer_preview_state early_preview_state;
    early_preview_state.enabled = true;
    early_preview_state.bands = {{
        {.center_frequency_hz = 1000.0F, .gain_db = 3.0F, .q_value = 2.0F},
    }};
    auto early_preview_result = session.preview_equalizer_response(early_preview_state, frequencies_hz);
    REQUIRE(!early_preview_result);
    REQUIRE(early_preview_result.error().code == sonotide::error_code::invalid_state);

    // Invalid band index для нового Q API должен отклоняться.
    auto invalid_q_result = session.set_equalizer_band_q(99U, 2.0F);
    REQUIRE(!invalid_q_result);
    REQUIRE(invalid_q_result.error().code == sonotide::error_code::invalid_argument);

    REQUIRE(session.set_equalizer_enabled(true));
    REQUIRE(session.set_equalizer_band_gain(4U, 6.0F));
    REQUIRE(session.set_equalizer_band_q(4U, 4.0F));
    REQUIRE(session.set_equalizer_output_gain(1.5F));

    const sonotide::audio_format render_format{
        .sample = sonotide::sample_type::float32,
        .sample_rate = 48000,
        .channel_count = 2,
        .bits_per_sample = 32,
        .valid_bits_per_sample = 32,
        .channel_mask = 0,
        .interleaved = true,
    };
    REQUIRE(backend->emit_render(render_format, 32U));

    auto session_curve_result = session.sample_equalizer_response(frequencies_hz);
    REQUIRE(session_curve_result);
    REQUIRE(session_curve_result.value().enabled);
    REQUIRE(approximately_equal(session_curve_result.value().sample_rate_hz, 48000.0F));

    const sonotide::equalizer_state live_state_before_preview = session.equalizer_state();

    sonotide::equalizer_preview_state disabled_preview_state;
    auto disabled_preview_result = session.preview_equalizer_response(disabled_preview_state, frequencies_hz);
    REQUIRE(disabled_preview_result);
    REQUIRE(!disabled_preview_result.value().enabled);
    for (const auto& point : disabled_preview_result.value().points) {
        REQUIRE(approximately_equal(point.response_db, 0.0F));
    }

    sonotide::equalizer_preview_state preview_state;
    preview_state.enabled = true;
    preview_state.output_gain_db = 3.0F;
    preview_state.bands = {{
        {.center_frequency_hz = 1005.0F, .gain_db = -3.0F, .q_value = 100.0F},
        {.center_frequency_hz = 1000.0F, .gain_db = 6.0F, .q_value = 0.05F},
    }};

    auto preview_curve_result = session.preview_equalizer_response(preview_state, frequencies_hz);
    REQUIRE(preview_curve_result);
    REQUIRE(preview_curve_result.value().enabled);
    REQUIRE(approximately_equal(preview_curve_result.value().sample_rate_hz, 48000.0F));
    REQUIRE(approximately_equal(preview_curve_result.value().applied_output_gain_db, 3.0F));

    sonotide::equalizer_state expected_preview_state;
    expected_preview_state.enabled = true;
    expected_preview_state.output_gain_db = 3.0F;
    expected_preview_state.bands = {{
        {.center_frequency_hz = 1000.0F, .gain_db = 6.0F, .q_value = 0.1F},
        {.center_frequency_hz = 1010.0F, .gain_db = -3.0F, .q_value = 12.0F},
    }};
    const auto expected_preview_curve_result = sonotide::sample_equalizer_response(
        expected_preview_state,
        48000.0F,
        frequencies_hz);
    REQUIRE(expected_preview_curve_result);
    REQUIRE(preview_curve_result.value().points.size() == expected_preview_curve_result.value().points.size());
    REQUIRE(approximately_equal(
        preview_curve_result.value().applied_headroom_compensation_db,
        expected_preview_curve_result.value().applied_headroom_compensation_db,
        0.05F));
    for (std::size_t index = 0; index < preview_curve_result.value().points.size(); ++index) {
        REQUIRE(approximately_equal(
            preview_curve_result.value().points[index].frequency_hz,
            expected_preview_curve_result.value().points[index].frequency_hz));
        REQUIRE(approximately_equal(
            preview_curve_result.value().points[index].response_db,
            expected_preview_curve_result.value().points[index].response_db,
            0.05F));
    }

    const sonotide::equalizer_state live_state_after_preview = session.equalizer_state();
    REQUIRE(equalizer_states_match(live_state_before_preview, live_state_after_preview));

    const auto public_curve_result = sonotide::sample_equalizer_response(
        session.equalizer_state(),
        48000.0F,
        frequencies_hz);
    REQUIRE(public_curve_result);
    REQUIRE(session_curve_result.value().points.size() == public_curve_result.value().points.size());
    for (std::size_t index = 0; index < session_curve_result.value().points.size(); ++index) {
        REQUIRE(approximately_equal(
            session_curve_result.value().points[index].frequency_hz,
            public_curve_result.value().points[index].frequency_hz));
        REQUIRE(approximately_equal(
            session_curve_result.value().points[index].response_db,
            public_curve_result.value().points[index].response_db,
            0.05F));
    }

    return 0;
}
