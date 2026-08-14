#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "sonotide/audio_format.h"
#include "sonotide/result.h"

namespace sonotide::detail::playback {

struct decoded_block {
    std::vector<float> samples;
    std::int64_t position_ms = 0;
    std::int64_t duration_ms = 0;
    bool end_of_stream = false;
};

class decoder {
public:
    virtual ~decoder() = default;
    virtual result<void> open(
        const std::string& source_uri,
        const audio_format& output_format,
        std::uint64_t command_epoch) = 0;
    virtual result<void> seek_to(std::int64_t position_ms, std::uint64_t command_epoch) = 0;
    virtual result<decoded_block> read_frames(
        std::uint32_t frame_count,
        std::uint64_t command_epoch) = 0;
    /// Cancels the previous command and returns the epoch owned by the next command.
    [[nodiscard]] virtual std::uint64_t request_cancel() noexcept = 0;
    virtual void close() noexcept = 0;
    [[nodiscard]] virtual std::int64_t duration_ms() const noexcept = 0;
};

using decoder_factory = std::function<std::unique_ptr<decoder>()>;

[[nodiscard]] std::unique_ptr<decoder> make_decoder();
void set_decoder_factory_for_testing(decoder_factory factory);
void reset_decoder_factory_for_testing();

}  // namespace sonotide::detail::playback
