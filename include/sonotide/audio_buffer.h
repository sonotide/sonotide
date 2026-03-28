#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "sonotide/audio_format.h"

namespace sonotide {

struct stream_timestamp {
    std::uint64_t device_position_frames = 0;
    std::uint64_t qpc_position_100ns = 0;
};

struct audio_buffer_view {
    std::span<std::byte> bytes;
    std::uint32_t frame_count = 0;
    audio_format format;
};

struct const_audio_buffer_view {
    std::span<const std::byte> bytes;
    std::uint32_t frame_count = 0;
    audio_format format;
};

}  // namespace sonotide

