#include "internal/playback/playback_decoder.h"

#include <atomic>
#include <mutex>
#include <utility>

#if defined(_WIN32)
#include "internal/win/media_foundation_decoder.h"
#endif

namespace sonotide::detail::playback {
namespace {

std::mutex factory_mutex;
decoder_factory test_factory;

#if defined(_WIN32)
class platform_decoder final : public decoder {
public:
    result<void> open(
        const std::string& uri,
        const audio_format& format,
        const std::uint64_t command_epoch) override {
        return decoder_.open(uri, format, command_epoch);
    }
    result<void> seek_to(
        const std::int64_t position_ms,
        const std::uint64_t command_epoch) override {
        return decoder_.seek_to(position_ms, command_epoch);
    }
    result<decoded_block> read_frames(
        const std::uint32_t frame_count,
        const std::uint64_t command_epoch) override {
        auto result_value = decoder_.read_frames(frame_count, command_epoch);
        if (!result_value) {
            return result<decoded_block>::failure(result_value.error());
        }
        auto source = std::move(result_value.value());
        return result<decoded_block>::success(decoded_block{
            .samples = std::move(source.samples),
            .position_ms = source.position_ms,
            .duration_ms = source.duration_ms,
            .end_of_stream = source.end_of_stream,
        });
    }
    std::uint64_t request_cancel() noexcept override { return decoder_.request_cancel(); }
    void close() noexcept override { decoder_.close(); }
    std::int64_t duration_ms() const noexcept override { return decoder_.duration_ms(); }

private:
    win::media_foundation_decoder decoder_;
};
#else
class platform_decoder final : public decoder {
public:
    result<void> open(const std::string&, const audio_format&, std::uint64_t) override {
        error failure;
        failure.category = error_category::platform;
        failure.code = error_code::unsupported_platform;
        failure.operation = "playback_decoder::open";
        failure.message = "Playback decoding is available only on Windows.";
        return result<void>::failure(std::move(failure));
    }
    result<void> seek_to(std::int64_t, std::uint64_t epoch) override { return open({}, {}, epoch); }
    result<decoded_block> read_frames(std::uint32_t, std::uint64_t epoch) override {
        return result<decoded_block>::failure(open({}, {}, epoch).error());
    }
    std::uint64_t request_cancel() noexcept override {
        return epoch_.fetch_add(1U, std::memory_order_acq_rel) + 1U;
    }
    void close() noexcept override {}
    std::int64_t duration_ms() const noexcept override { return 0; }
private:
    std::atomic<std::uint64_t> epoch_{0};
};
#endif

}  // namespace

std::unique_ptr<decoder> make_decoder() {
    std::scoped_lock lock(factory_mutex);
    return test_factory ? test_factory() : std::make_unique<platform_decoder>();
}

void set_decoder_factory_for_testing(decoder_factory factory) {
    std::scoped_lock lock(factory_mutex);
    test_factory = std::move(factory);
}

void reset_decoder_factory_for_testing() {
    std::scoped_lock lock(factory_mutex);
    test_factory = {};
}

}  // namespace sonotide::detail::playback
