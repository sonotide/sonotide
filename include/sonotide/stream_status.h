#pragma once

#include <cstdint>
#include <optional>

#include "sonotide/audio_format.h"
#include "sonotide/stream_state.h"

namespace sonotide {

struct stream_statistics {
    std::uint64_t callback_count = 0;
    std::uint64_t frames_processed = 0;
    std::uint64_t discontinuity_count = 0;
};

struct stream_status {
    stream_state state = stream_state::created;
    audio_format requested_format;
    std::optional<audio_format> negotiated_format;
    stream_statistics statistics;
    bool device_lost = false;
};

}  // namespace sonotide

