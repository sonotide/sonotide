#define _ALLOW_KEYWORD_MACROS
#define private public
#include "sonotide/runtime.h"
#undef private

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <future>
#include <iostream>
#include <memory>
#include <limits>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "internal/runtime_backend.h"
#include "internal/playback/decoded_audio_queue.h"
#include "internal/playback/playback_decoder.h"
#include "test_support/test_harness.h"

namespace sonotide {

runtime::runtime(std::shared_ptr<detail::runtime_backend> backend) noexcept
    : backend_(std::move(backend)) {}

}  // namespace sonotide

namespace {

constexpr float kEpsilon = 0.01F;

bool approximately_equal(const float left, const float right, const float epsilon = kEpsilon) {
    return std::fabs(left - right) <= epsilon;
}

template <typename T>
bool is_invalid_state_result(const sonotide::result<T>& value) {
    return !value && value.error().code == sonotide::error_code::invalid_state;
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
        close_count_.fetch_add(1U);
        return sonotide::result<void>::success();
    }

    [[nodiscard]] sonotide::stream_status status() const override {
        sonotide::stream_status snapshot;
        snapshot.state = closed_
            ? sonotide::stream_state::closed
            : (started_ ? sonotide::stream_state::running : sonotide::stream_state::stopped);
        return snapshot;
    }

    [[nodiscard]] std::size_t close_count() const noexcept {
        return close_count_.load();
    }

private:
    bool started_ = false;
    bool closed_ = false;
    std::atomic<std::size_t> close_count_{0U};
};

class fake_runtime_backend final : public sonotide::detail::runtime_backend {
public:
    fake_runtime_backend()
        : handle_(std::make_shared<fake_stream_handle>()) {}

    [[nodiscard]] sonotide::result<std::vector<sonotide::device_info>> enumerate_devices(
        sonotide::device_direction) const override {
        std::unique_lock lock(enumerate_mutex_);
        if (block_enumerate_) {
            enumerate_entered_ = true;
            enumerate_condition_.notify_all();
            enumerate_condition_.wait(lock, [this]() { return !block_enumerate_; });
        }
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
        ++open_render_count_;
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
        auto render_result = render_callback_->on_render(
            sonotide::audio_buffer_view{bytes, frame_count, format},
            sonotide::stream_timestamp{});
        {
            std::scoped_lock lock(render_mutex_);
            last_render_bytes_ = std::move(bytes);
            ++render_count_;
        }
        render_condition_.notify_all();
        return render_result;
    }

    [[nodiscard]] std::shared_ptr<fake_stream_handle> handle() const {
        return handle_;
    }
    [[nodiscard]] std::size_t open_render_count() const noexcept { return open_render_count_.load(); }

    void block_enumeration() {
        std::scoped_lock lock(enumerate_mutex_);
        block_enumerate_ = true;
        enumerate_entered_ = false;
    }

    bool wait_for_enumeration() const {
        std::unique_lock lock(enumerate_mutex_);
        return enumerate_condition_.wait_for(
            lock, std::chrono::seconds(5), [this]() { return enumerate_entered_; });
    }

    void release_enumeration() {
        {
            std::scoped_lock lock(enumerate_mutex_);
            block_enumerate_ = false;
        }
        enumerate_condition_.notify_all();
    }

    [[nodiscard]] std::vector<std::byte> last_render_bytes() const {
        std::scoped_lock lock(render_mutex_);
        return last_render_bytes_;
    }

private:
    std::shared_ptr<fake_stream_handle> handle_;
    sonotide::render_callback* render_callback_ = nullptr;
    mutable std::mutex enumerate_mutex_;
    mutable std::condition_variable enumerate_condition_;
    mutable bool block_enumerate_ = false;
    mutable bool enumerate_entered_ = false;
    mutable std::mutex render_mutex_;
    mutable std::condition_variable render_condition_;
    std::vector<std::byte> last_render_bytes_;
    std::size_t render_count_ = 0;
    std::atomic<std::size_t> open_render_count_{0U};
};

struct controlled_decoder_state {
    std::mutex mutex;
    std::condition_variable condition;
    bool block_open = false;
    bool open_entered = false;
    bool block_read = false;
    bool read_entered = false;
    bool fail_reads = false;
    bool fail_open = false;
    bool fail_seek = false;
    bool next_eos = false;
    std::string active_uri;
    std::vector<std::string> opened_uris;
    std::vector<std::string> read_uris;
    std::vector<std::int64_t> seeks;
    std::size_t read_count = 0;
    std::size_t completed_read_count = 0;
    std::size_t reads_after_seek = 0;
    std::size_t cancel_count = 0;
    std::uint64_t cancel_epoch = 0;

    bool wait_for_open_count(const std::size_t count) {
        std::unique_lock lock(mutex);
        return condition.wait_for(lock, std::chrono::seconds(5), [&]() { return opened_uris.size() >= count; });
    }
    bool wait_for_read() {
        std::unique_lock lock(mutex);
        return condition.wait_for(lock, std::chrono::seconds(5), [&]() { return read_entered; });
    }
    bool wait_for_completed_reads(const std::size_t count) {
        std::unique_lock lock(mutex);
        return condition.wait_for(lock, std::chrono::seconds(5), [&]() { return completed_read_count >= count; });
    }
    bool wait_for_read_count(const std::size_t count) {
        std::unique_lock lock(mutex);
        return condition.wait_for(lock, std::chrono::seconds(5), [&]() { return read_count >= count; });
    }
    bool wait_for_seek_count(const std::size_t count) {
        std::unique_lock lock(mutex);
        return condition.wait_for(lock, std::chrono::seconds(5), [&]() { return seeks.size() >= count; });
    }
    bool wait_for_reads_after_seek(const std::size_t count) {
        std::unique_lock lock(mutex);
        return condition.wait_for(lock, std::chrono::seconds(5), [&]() { return reads_after_seek >= count; });
    }
    bool wait_for_uri_read_count(const std::string& uri, const std::size_t count) {
        std::unique_lock lock(mutex);
        return condition.wait_for(lock, std::chrono::seconds(5), [&]() {
            return static_cast<std::size_t>(std::count(read_uris.begin(), read_uris.end(), uri)) >= count;
        });
    }
    void release_open() {
        {
            std::scoped_lock lock(mutex);
            block_open = false;
        }
        condition.notify_all();
    }
};

class controlled_decoder final : public sonotide::detail::playback::decoder {
public:
    explicit controlled_decoder(std::shared_ptr<controlled_decoder_state> state)
        : state_(std::move(state)) {}

    sonotide::result<void> open(
        const std::string& uri,
        const sonotide::audio_format&,
        const std::uint64_t operation_epoch) override {
        std::unique_lock lock(state_->mutex);
        if (state_->cancel_epoch != operation_epoch) {
            return sonotide::result<void>::failure(make_error(
                sonotide::error_code::invalid_state, "controlled_decoder::open", "cancelled"));
        }
        state_->active_uri = uri;
        if (uri == "track-b") {
            state_->next_eos = false;
        }
        state_->opened_uris.push_back(uri);
        state_->open_entered = true;
        state_->condition.notify_all();
        if (!state_->condition.wait_for(
                lock, std::chrono::seconds(5), [&]() {
                    return !state_->block_open || state_->cancel_epoch != operation_epoch;
                })) {
            return sonotide::result<void>::failure(make_error(
                sonotide::error_code::invalid_state, "controlled_decoder::open", "test timeout"));
        }
        if (state_->cancel_epoch != operation_epoch) {
            return sonotide::result<void>::failure(make_error(
                sonotide::error_code::invalid_state, "controlled_decoder::open", "cancelled"));
        }
        if (state_->fail_open) {
            return sonotide::result<void>::failure(make_error(
                sonotide::error_code::stream_open_failed,
                "controlled_decoder::open",
                "forced open failure"));
        }
        return sonotide::result<void>::success();
    }

    sonotide::result<void> seek_to(
        const std::int64_t position_ms,
        const std::uint64_t operation_epoch) override {
        std::scoped_lock lock(state_->mutex);
        if (state_->cancel_epoch != operation_epoch) {
            return sonotide::result<void>::failure(make_error(
                sonotide::error_code::invalid_state,
                "controlled_decoder::seek_to",
                "cancelled"));
        }
        state_->seeks.push_back(position_ms);
        state_->condition.notify_all();
        if (state_->fail_seek) {
            return sonotide::result<void>::failure(make_error(
                sonotide::error_code::invalid_state,
                "controlled_decoder::seek_to",
                "forced seek failure"));
        }
        return sonotide::result<void>::success();
    }

    sonotide::result<sonotide::detail::playback::decoded_block> read_frames(
        const std::uint32_t frame_count,
        const std::uint64_t operation_epoch) override {
        std::unique_lock lock(state_->mutex);
        if (state_->cancel_epoch != operation_epoch) {
            return sonotide::result<sonotide::detail::playback::decoded_block>::failure(make_error(
                sonotide::error_code::invalid_state,
                "controlled_decoder::read_frames",
                "cancelled"));
        }
        state_->read_entered = true;
        ++state_->read_count;
        state_->read_uris.push_back(state_->active_uri);
        if (!state_->seeks.empty()) {
            ++state_->reads_after_seek;
        }
        state_->condition.notify_all();
        if (!state_->condition.wait_for(
                lock, std::chrono::seconds(5), [&]() {
                    return !state_->block_read || state_->cancel_epoch != operation_epoch;
                })) {
            return sonotide::result<sonotide::detail::playback::decoded_block>::failure(make_error(
                sonotide::error_code::invalid_state, "controlled_decoder::read_frames", "test timeout"));
        }
        if (state_->cancel_epoch != operation_epoch || state_->fail_reads) {
            return sonotide::result<sonotide::detail::playback::decoded_block>::failure(make_error(
                sonotide::error_code::invalid_state, "controlled_decoder::read_frames", "cancelled"));
        }
        const float marker = state_->active_uri == "track-b" ? 0.5F : 0.25F;
        const std::int64_t position = state_->seeks.empty() ?
            (state_->active_uri == "track-b" ? 222 : 111) : state_->seeks.back();
        sonotide::detail::playback::decoded_block block;
        block.samples.assign(static_cast<std::size_t>(frame_count) * 2U, marker);
        block.position_ms = position;
        block.duration_ms = 1000;
        block.end_of_stream = state_->next_eos;
        state_->next_eos = false;
        ++state_->completed_read_count;
        state_->condition.notify_all();
        return sonotide::result<sonotide::detail::playback::decoded_block>::success(std::move(block));
    }

    std::uint64_t request_cancel() noexcept override {
        std::uint64_t next_epoch = 0;
        {
            std::scoped_lock lock(state_->mutex);
            next_epoch = ++state_->cancel_epoch;
            ++state_->cancel_count;
        }
        state_->condition.notify_all();
        return next_epoch;
    }
    void close() noexcept override {}
    std::int64_t duration_ms() const noexcept override { return 1000; }

private:
    std::shared_ptr<controlled_decoder_state> state_;
};

struct uncooperative_decoder_state {
    std::mutex mutex;
    std::condition_variable condition;
    std::vector<std::string> opened_uris;
    bool release_first_open = false;
    bool read_entered = false;
    bool release_read = false;
    std::size_t decoder_close_count = 0;
    std::uint64_t cancel_epoch = 0;

    bool wait_for_open_count(const std::size_t count) {
        std::unique_lock lock(mutex);
        return condition.wait_for(lock, std::chrono::seconds(5), [&]() {
            return opened_uris.size() >= count;
        });
    }

    bool wait_for_read() {
        std::unique_lock lock(mutex);
        return condition.wait_for(lock, std::chrono::seconds(5), [&]() {
            return read_entered;
        });
    }

    bool wait_for_decoder_close_count(const std::size_t count) {
        std::unique_lock lock(mutex);
        return condition.wait_for(lock, std::chrono::seconds(5), [&]() {
            return decoder_close_count >= count;
        });
    }
};

// Models a synchronous third-party Media Foundation handler: cancellation
// changes its generation but deliberately does not release an in-flight call.
class uncooperative_decoder final : public sonotide::detail::playback::decoder {
public:
    explicit uncooperative_decoder(std::shared_ptr<uncooperative_decoder_state> state)
        : state_(std::move(state)) {}

    sonotide::result<void> open(
        const std::string& uri,
        const sonotide::audio_format&,
        std::uint64_t) override {
        std::unique_lock lock(state_->mutex);
        state_->opened_uris.push_back(uri);
        const bool first_open = state_->opened_uris.size() == 1U;
        state_->condition.notify_all();
        if (first_open) {
            state_->condition.wait(lock, [this]() { return state_->release_first_open; });
        }
        return sonotide::result<void>::success();
    }

    sonotide::result<void> seek_to(std::int64_t, std::uint64_t) override {
        return sonotide::result<void>::success();
    }

    sonotide::result<sonotide::detail::playback::decoded_block> read_frames(
        const std::uint32_t frame_count,
        std::uint64_t) override {
        std::unique_lock lock(state_->mutex);
        state_->read_entered = true;
        state_->condition.notify_all();
        state_->condition.wait(lock, [this]() { return state_->release_read; });
        sonotide::detail::playback::decoded_block block;
        block.samples.assign(static_cast<std::size_t>(frame_count) * 2U, 0.75F);
        block.position_ms = 333;
        block.duration_ms = 1000;
        return sonotide::result<sonotide::detail::playback::decoded_block>::success(
            std::move(block));
    }

    std::uint64_t request_cancel() noexcept override {
        std::scoped_lock lock(state_->mutex);
        return ++state_->cancel_epoch;
    }

    void close() noexcept override {
        {
            std::scoped_lock lock(state_->mutex);
            ++state_->decoder_close_count;
        }
        state_->condition.notify_all();
    }

    std::int64_t duration_ms() const noexcept override { return 1000; }

private:
    std::shared_ptr<uncooperative_decoder_state> state_;
};

struct decoder_factory_reset_guard {
    ~decoder_factory_reset_guard() {
        sonotide::detail::playback::reset_decoder_factory_for_testing();
    }
};

}  // namespace

int main() {
    auto backend = std::make_shared<fake_runtime_backend>();
    sonotide::runtime runtime(backend);

    sonotide::playback_session_config invalid_timeout_config;
    invalid_timeout_config.decoder_shutdown_timeout_ms = 0;
    auto zero_timeout_result = runtime.open_playback_session(invalid_timeout_config);
    REQUIRE(!zero_timeout_result);
    REQUIRE(zero_timeout_result.error().code == sonotide::error_code::invalid_argument);
    REQUIRE(backend->open_render_count() == 0U);
    invalid_timeout_config.decoder_shutdown_timeout_ms = 5001;
    auto excessive_timeout_result = runtime.open_playback_session(invalid_timeout_config);
    REQUIRE(!excessive_timeout_result);
    REQUIRE(excessive_timeout_result.error().code == sonotide::error_code::invalid_argument);
    REQUIRE(backend->open_render_count() == 0U);

    sonotide::playback_session_config invalid_initial_config;
    sonotide::equalizer_state invalid_initial_state;
    invalid_initial_state.bands = sonotide::make_default_equalizer_bands(1U);
    invalid_initial_state.bands[0].gain_db = std::numeric_limits<float>::quiet_NaN();
    invalid_initial_config.initial_equalizer_state = invalid_initial_state;
    REQUIRE(!runtime.open_playback_session(invalid_initial_config));
    REQUIRE(backend->open_render_count() == 0U);
    invalid_initial_state.bands[0].gain_db = 0.0F;
    invalid_initial_state.last_nonflat_band_gains_db = {
        std::numeric_limits<float>::infinity()};
    invalid_initial_config.initial_equalizer_state = invalid_initial_state;
    REQUIRE(!runtime.open_playback_session(invalid_initial_config));
    REQUIRE(backend->open_render_count() == 0U);
    invalid_initial_state.last_nonflat_band_gains_db.clear();
    invalid_initial_state.bands.assign(
        sonotide::supported_equalizer_band_count_limits().max_band_count + 1U,
        sonotide::equalizer_band{.center_frequency_hz = 1000.0F, .gain_db = 0.0F, .q_value = 1.0F});
    invalid_initial_config.initial_equalizer_state = invalid_initial_state;
    REQUIRE(!runtime.open_playback_session(invalid_initial_config));
    REQUIRE(backend->open_render_count() == 0U);

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

    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float infinity = std::numeric_limits<float>::infinity();
    early_preview_state.output_gain_db = infinity;
    REQUIRE(!session.preview_equalizer_response(early_preview_state, frequencies_hz));
    const std::array<float, 1> invalid_frequencies{{nan}};
    REQUIRE(!session.sample_equalizer_response(invalid_frequencies));

    // Invalid band index для нового Q API должен отклоняться.
    auto invalid_q_result = session.set_equalizer_band_q(99U, 2.0F);
    REQUIRE(!invalid_q_result);
    REQUIRE(invalid_q_result.error().code == sonotide::error_code::invalid_argument);
    REQUIRE(!session.set_equalizer_band_gain(0U, nan));
    REQUIRE(!session.set_equalizer_band_q(0U, infinity));
    REQUIRE(!session.set_equalizer_output_gain(-infinity));
    REQUIRE(!session.add_equalizer_band(nan, 0.0F));
    REQUIRE(!session.add_equalizer_band(1000.0F, infinity));
    REQUIRE(!session.set_equalizer_band_frequency(0U, infinity));
    sonotide::equalizer_state invalid_finite_state = session.equalizer_state();
    invalid_finite_state.bands[0].gain_db = nan;
    REQUIRE(!session.apply_equalizer_state(invalid_finite_state));
    invalid_finite_state = session.equalizer_state();
    invalid_finite_state.last_nonflat_band_gains_db[0] = infinity;
    REQUIRE(!session.apply_equalizer_state(invalid_finite_state));

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


    // `close` атомарно отсоединяет implementation: параллельный вызов, успевший получить
    // snapshot, либо завершается безопасно, либо получает invalid_state, но не обращается
    // к освобождённой памяти. Барьер через mutex делает старт одновременно воспроизводимым.
    std::mutex start_mutex;
    std::condition_variable start_condition;
    bool start = false;
    auto concurrent_call = std::async(std::launch::async, [&]() {
        {
            std::unique_lock lock(start_mutex);
            start_condition.wait(lock, [&]() { return start; });
        }
        return session.set_volume_percent(75);
    });
    {
        std::scoped_lock lock(start_mutex);
        start = true;
    }
    start_condition.notify_one();
    REQUIRE(session.close());
    REQUIRE(concurrent_call.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    const auto concurrent_result = concurrent_call.get();
    REQUIRE(concurrent_result || concurrent_result.error().code == sonotide::error_code::invalid_state);
    REQUIRE(!session.is_open());
    REQUIRE(backend->handle()->close_count() == 1U);
    REQUIRE(session.close());
    REQUIRE(backend->handle()->close_count() == 1U);

    // API-вызов удерживает shared snapshot implementation до своего возврата. До исправления
    // `close()` удалял implementation, пока этот управляемо заблокированный вызов ещё выполнялся.
    auto race_backend = std::make_shared<fake_runtime_backend>();
    sonotide::runtime race_runtime(race_backend);
    auto race_session_result = race_runtime.open_playback_session({});
    REQUIRE(race_session_result);
    auto race_session = std::move(race_session_result.value());
    race_backend->block_enumeration();
    auto enumeration = std::async(std::launch::async, [&]() {
        return race_session.list_output_devices();
    });
    REQUIRE(race_backend->wait_for_enumeration());
    REQUIRE(race_session.close());
    REQUIRE(race_backend->handle()->close_count() == 1U);
    race_backend->release_enumeration();
    REQUIRE(enumeration.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    const auto enumeration_result = enumeration.get();
    REQUIRE(!enumeration_result);
    REQUIRE(enumeration_result.error().code == sonotide::error_code::invalid_state);

    // Неявное RAII-закрытие обязано остановить recovery worker и закрыть handle ровно один раз.
    auto destruction_backend = std::make_shared<fake_runtime_backend>();
    {
        sonotide::runtime destruction_runtime(destruction_backend);
        auto destruction_session_result = destruction_runtime.open_playback_session({});
        REQUIRE(destruction_session_result);
        auto destruction_session = std::move(destruction_session_result.value());
        REQUIRE(destruction_session.is_open());
    }
    REQUIRE(destruction_backend->handle()->close_count() == 1U);

    // Decode I/O выполняется только worker-ом: заблокированный open не блокирует callback,
    // а callback при пустой очереди явно возвращает тишину.
    auto decoder_state = std::make_shared<controlled_decoder_state>();
    decoder_factory_reset_guard factory_guard;
    decoder_state->block_open = true;
    sonotide::detail::playback::set_decoder_factory_for_testing([decoder_state]() {
        return std::make_unique<controlled_decoder>(decoder_state);
    });
    auto worker_backend = std::make_shared<fake_runtime_backend>();
    sonotide::runtime worker_runtime(worker_backend);
    auto worker_session_result = worker_runtime.open_playback_session({});
    REQUIRE(worker_session_result);
    auto worker_session = std::move(worker_session_result.value());
    REQUIRE(worker_session.load("track-a"));
    for (std::size_t attempt = 0; attempt < 16U && worker_session.state().position_ms != 222; ++attempt) {
        REQUIRE(worker_backend->emit_render(render_format, 32U));
    }
    REQUIRE(decoder_state->wait_for_open_count(1U));
    const auto underrun_bytes = worker_backend->last_render_bytes();
    REQUIRE(std::all_of(underrun_bytes.begin(), underrun_bytes.end(), [](const std::byte value) {
        return value == std::byte{0};
    }));

    // Rapid load отменяет blocked read старого generation. Ни его PCM, ни EOS не могут
    // попасть в состояние нового source.
    {
        std::scoped_lock lock(decoder_state->mutex);
        decoder_state->block_read = true;
        decoder_state->next_eos = true;
    }
    decoder_state->release_open();
    REQUIRE(decoder_state->wait_for_read());
    REQUIRE(worker_session.load("track-b"));
    REQUIRE(decoder_state->wait_for_open_count(2U));
    {
        std::scoped_lock lock(decoder_state->mutex);
        decoder_state->block_read = false;
        decoder_state->read_entered = false;
    }
    decoder_state->condition.notify_all();
    // Начало следующего read доказывает, что предыдущий B-блок уже помещён в очередь.
    REQUIRE(decoder_state->wait_for_uri_read_count("track-b", 2U));
    for (std::size_t attempt = 0; attempt < 16U && worker_session.state().position_ms != 500; ++attempt) {
        REQUIRE(worker_backend->emit_render(render_format, 32U));
    }
    REQUIRE(worker_session.state().source_uri == "track-b");
    REQUIRE(worker_session.state().position_ms == 222);
    REQUIRE(worker_session.state().completion_token == 0U);

    // Pause не потребляет готовый PCM: callback возвращает silence; play затем потребляет
    // тот же заранее подготовленный блок без decoder-вызова из callback.
    REQUIRE(worker_session.pause());
    REQUIRE(worker_backend->emit_render(render_format, 32U));
    const auto paused_bytes = worker_backend->last_render_bytes();
    REQUIRE(std::all_of(paused_bytes.begin(), paused_bytes.end(), [](const std::byte value) {
        return value == std::byte{0};
    }));
    REQUIRE(worker_session.play());
    REQUIRE(worker_backend->emit_render(render_format, 32U));

    // Seek command sequence инвалидирует уже декодируемый блок и публикует только PCM
    // после seek текущего generation.
    REQUIRE(worker_session.seek_to(500));
    // Old queued PCM must be rejected by atomic seek sequence before copy/DSP.
    REQUIRE(worker_backend->emit_render(render_format, 32U));
    const auto stale_seek_bytes = worker_backend->last_render_bytes();
    REQUIRE(std::all_of(stale_seek_bytes.begin(), stale_seek_bytes.end(), [](const std::byte value) {
        return value == std::byte{0};
    }));
    REQUIRE(decoder_state->wait_for_seek_count(1U));
    // Второй post-seek read начинается только после публикации первого post-seek блока.
    REQUIRE(decoder_state->wait_for_reads_after_seek(2U));
    REQUIRE(worker_backend->emit_render(render_format, 32U));
    REQUIRE(worker_session.state().position_ms == 500);
    {
        std::scoped_lock lock(decoder_state->mutex);
        REQUIRE(!decoder_state->seeks.empty());
        REQUIRE(decoder_state->seeks.back() == 500);
    }

    // Close обязан вызвать cancellation seam и дождаться blocked decoder worker.
    {
        std::scoped_lock lock(decoder_state->mutex);
        decoder_state->block_read = true;
        decoder_state->read_entered = false;
    }
    // Drain the bounded queue completely. A single callback is not a worker
    // barrier: the producer may refill that one slot before this thread waits.
    for (std::size_t index = 0; index < 8U; ++index) {
        REQUIRE(worker_backend->emit_render(render_format, 32U));
    }
    REQUIRE(decoder_state->wait_for_read());
    auto worker_close = std::async(std::launch::async, [&]() { return worker_session.close(); });
    REQUIRE(worker_close.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    REQUIRE(worker_close.get());
    {
        std::scoped_lock lock(decoder_state->mutex);
        REQUIRE(decoder_state->cancel_count > 0U);
    }
    sonotide::detail::playback::reset_decoder_factory_for_testing();

    // A synchronous decoder call may ignore cancellation completely. A newer
    // generation must still supersede its result, and close with a minimal wait
    // budget must detach safely instead of blocking or destroying live state.
    auto uncooperative_state = std::make_shared<uncooperative_decoder_state>();
    sonotide::detail::playback::set_decoder_factory_for_testing([uncooperative_state]() {
        return std::make_unique<uncooperative_decoder>(uncooperative_state);
    });
    auto bounded_backend = std::make_shared<fake_runtime_backend>();
    sonotide::runtime bounded_runtime(bounded_backend);
    sonotide::playback_session_config bounded_config;
    bounded_config.decoder_shutdown_timeout_ms = 1;
    auto bounded_session_result = bounded_runtime.open_playback_session(bounded_config);
    REQUIRE(bounded_session_result);
    auto bounded_session = std::move(bounded_session_result.value());
    REQUIRE(bounded_session.load("stale-open"));
    REQUIRE(bounded_backend->emit_render(render_format, 32U));
    REQUIRE(uncooperative_state->wait_for_open_count(1U));
    REQUIRE(bounded_session.load("current-open"));
    {
        std::scoped_lock lock(uncooperative_state->mutex);
        uncooperative_state->release_first_open = true;
    }
    uncooperative_state->condition.notify_all();
    REQUIRE(uncooperative_state->wait_for_open_count(2U));
    REQUIRE(uncooperative_state->wait_for_read());
    REQUIRE(bounded_session.state().source_uri == "current-open");
    const std::size_t close_count_before_shutdown = [&]() {
        std::scoped_lock lock(uncooperative_state->mutex);
        return uncooperative_state->decoder_close_count;
    }();

    // No sleep is required: read_frames remains held at the deterministic gate
    // beyond the configured one-millisecond shutdown budget.
    auto bounded_close_result = bounded_session.close();
    REQUIRE(!bounded_close_result);
    REQUIRE(bounded_close_result.error().code == sonotide::error_code::operation_timed_out);
    REQUIRE(bounded_close_result.error().recoverable);
    REQUIRE(!bounded_session.is_open());
    REQUIRE(bounded_backend->handle()->close_count() == 1U);
    REQUIRE(is_invalid_state_result(bounded_session.load("after-close")));
    REQUIRE(is_invalid_state_result(bounded_session.play()));
    REQUIRE(is_invalid_state_result(bounded_session.pause()));
    REQUIRE(is_invalid_state_result(bounded_session.seek_to(10)));
    REQUIRE(is_invalid_state_result(bounded_session.set_volume_percent(50)));
    REQUIRE(is_invalid_state_result(bounded_session.set_equalizer_enabled(true)));
    REQUIRE(is_invalid_state_result(
        bounded_session.select_equalizer_preset(sonotide::equalizer_preset_id::flat)));
    REQUIRE(is_invalid_state_result(bounded_session.set_equalizer_band_gain(0U, 1.0F)));
    REQUIRE(is_invalid_state_result(bounded_session.set_equalizer_band_q(0U, 1.0F)));
    REQUIRE(is_invalid_state_result(bounded_session.add_equalizer_band(1000.0F)));
    REQUIRE(is_invalid_state_result(bounded_session.remove_equalizer_band(0U)));
    REQUIRE(is_invalid_state_result(
        bounded_session.set_equalizer_band_frequency(0U, 1000.0F)));
    REQUIRE(is_invalid_state_result(bounded_session.reset_equalizer()));
    REQUIRE(is_invalid_state_result(bounded_session.set_equalizer_output_gain(0.0F)));
    REQUIRE(is_invalid_state_result(
        bounded_session.apply_equalizer_state(sonotide::equalizer_state{})));
    REQUIRE(is_invalid_state_result(bounded_session.list_output_devices()));
    REQUIRE(is_invalid_state_result(bounded_session.select_output_device("")));
    const std::array<float, 1> closed_frequencies{{1000.0F}};
    REQUIRE(is_invalid_state_result(
        bounded_session.sample_equalizer_response(closed_frequencies)));
    REQUIRE(is_invalid_state_result(bounded_session.preview_equalizer_response(
        sonotide::equalizer_preview_state{}, closed_frequencies)));
    REQUIRE(!bounded_session.equalizer_band_frequency_range(0U).has_value());
    REQUIRE(bounded_session.state().source_uri.empty());
    REQUIRE(bounded_session.equalizer_state().bands.size() ==
        sonotide::equalizer_state{}.bands.size());
    REQUIRE(bounded_session.close());
    {
        std::scoped_lock lock(uncooperative_state->mutex);
        REQUIRE(uncooperative_state->decoder_close_count == close_count_before_shutdown);
        uncooperative_state->release_read = true;
    }
    uncooperative_state->condition.notify_all();
    REQUIRE(uncooperative_state->wait_for_decoder_close_count(close_count_before_shutdown + 1U));
    sonotide::detail::playback::reset_decoder_factory_for_testing();

    // Failed request is attempted once and remains dormant despite callback notifications.
    auto failure_state = std::make_shared<controlled_decoder_state>();
    failure_state->fail_open = true;
    sonotide::detail::playback::set_decoder_factory_for_testing([failure_state]() {
        return std::make_unique<controlled_decoder>(failure_state);
    });
    auto failure_backend = std::make_shared<fake_runtime_backend>();
    sonotide::runtime failure_runtime(failure_backend);
    auto failure_session_result = failure_runtime.open_playback_session({});
    REQUIRE(failure_session_result);
    auto failure_session = std::move(failure_session_result.value());
    REQUIRE(failure_session.load("broken-a"));
    REQUIRE(failure_backend->emit_render(render_format, 32U));
    REQUIRE(failure_state->wait_for_open_count(1U));
    for (std::size_t index = 0; index < 16U; ++index) {
        REQUIRE(failure_backend->emit_render(render_format, 32U));
    }
    {
        std::scoped_lock lock(failure_state->mutex);
        REQUIRE(failure_state->opened_uris.size() == 1U);
    }
    REQUIRE(failure_session.load("broken-b"));
    REQUIRE(failure_state->wait_for_open_count(2U));
    REQUIRE(failure_session.close());
    {
        std::scoped_lock lock(failure_state->mutex);
        REQUIRE(failure_state->opened_uris.size() == 2U);
    }
    sonotide::detail::playback::reset_decoder_factory_for_testing();

    auto seek_failure_state = std::make_shared<controlled_decoder_state>();
    seek_failure_state->fail_seek = true;
    sonotide::detail::playback::set_decoder_factory_for_testing([seek_failure_state]() {
        return std::make_unique<controlled_decoder>(seek_failure_state);
    });
    auto seek_failure_backend = std::make_shared<fake_runtime_backend>();
    sonotide::runtime seek_failure_runtime(seek_failure_backend);
    auto seek_failure_session_result = seek_failure_runtime.open_playback_session({});
    REQUIRE(seek_failure_session_result);
    auto seek_failure_session = std::move(seek_failure_session_result.value());
    REQUIRE(seek_failure_session.load("seek-failure"));
    REQUIRE(seek_failure_backend->emit_render(render_format, 32U));
    REQUIRE(seek_failure_state->wait_for_open_count(1U));
    REQUIRE(seek_failure_state->wait_for_read());
    REQUIRE(seek_failure_session.seek_to(700));
    REQUIRE(seek_failure_state->wait_for_seek_count(1U));
    const std::size_t opens_after_failed_seek = [&]() {
        std::scoped_lock lock(seek_failure_state->mutex);
        return seek_failure_state->opened_uris.size();
    }();
    for (std::size_t index = 0; index < 16U; ++index) {
        REQUIRE(seek_failure_backend->emit_render(render_format, 32U));
    }
    {
        std::scoped_lock lock(seek_failure_state->mutex);
        REQUIRE(seek_failure_state->opened_uris.size() == opens_after_failed_seek);
        REQUIRE(seek_failure_state->seeks.size() == 1U);
    }
    REQUIRE(seek_failure_session.close());
    {
        std::scoped_lock lock(seek_failure_state->mutex);
        REQUIRE(seek_failure_state->opened_uris.size() == opens_after_failed_seek);
        REQUIRE(seek_failure_state->seeks.size() == 1U);
    }
    sonotide::detail::playback::reset_decoder_factory_for_testing();

    // Queue capacity is fixed and full state supplies explicit producer backpressure.
    sonotide::detail::playback::decoded_audio_queue bounded_queue(2U, 4U);
    std::array<float, 4> queue_samples{{0.1F, 0.2F, 0.3F, 0.4F}};
    const sonotide::detail::playback::decoded_metadata first_metadata{
        .generation = 7U, .seek_sequence = 3U, .position_ms = 500,
        .duration_ms = 1000, .end_of_stream = false};
    const sonotide::detail::playback::decoded_metadata eos_metadata{
        .generation = 7U, .seek_sequence = 3U, .position_ms = 1000,
        .duration_ms = 1000, .end_of_stream = true};
    static_assert(noexcept(bounded_queue.try_push(queue_samples, first_metadata)));
    REQUIRE(bounded_queue.try_push(queue_samples, first_metadata));
    REQUIRE(bounded_queue.try_push(queue_samples, eos_metadata));
    REQUIRE(bounded_queue.full());
    REQUIRE(!bounded_queue.try_push(queue_samples, first_metadata));
    std::array<float, 4> popped_samples{};
    sonotide::detail::playback::decoded_metadata popped_metadata;
    REQUIRE(bounded_queue.try_pop(popped_samples, popped_metadata));
    REQUIRE(!popped_metadata.end_of_stream);
    REQUIRE(bounded_queue.try_pop(popped_samples, popped_metadata));
    REQUIRE(popped_metadata.end_of_stream);
    REQUIRE(!bounded_queue.try_pop(popped_samples, popped_metadata));

    return 0;
}
