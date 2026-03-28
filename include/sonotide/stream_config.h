#pragma once

#include <chrono>
#include <optional>

#include "sonotide/audio_format.h"
#include "sonotide/device_selector.h"

namespace sonotide {

enum class share_mode {
    shared,
    exclusive,
};

enum class callback_mode {
    event_driven,
};

struct stream_timing {
    std::chrono::milliseconds target_latency{20};
    std::optional<std::chrono::microseconds> engine_period;
};

struct render_stream_config {
    device_selector device = device_selector::system_default(device_direction::render);
    share_mode mode = share_mode::shared;
    callback_mode callback = callback_mode::event_driven;
    format_request format;
    stream_timing timing;
    bool auto_recover_device_loss = true;
    bool prefill_with_silence = true;
};

struct capture_stream_config {
    device_selector device = device_selector::system_default(device_direction::capture);
    share_mode mode = share_mode::shared;
    callback_mode callback = callback_mode::event_driven;
    format_request format;
    stream_timing timing;
    bool auto_recover_device_loss = true;
    bool deliver_silence_on_glitch = false;
};

struct loopback_stream_config {
    device_selector device = device_selector::system_default(device_direction::render);
    share_mode mode = share_mode::shared;
    callback_mode callback = callback_mode::event_driven;
    format_request format;
    stream_timing timing;
    bool auto_recover_device_loss = true;
};

}  // namespace sonotide

