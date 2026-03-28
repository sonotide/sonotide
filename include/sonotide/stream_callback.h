#pragma once

#include "sonotide/audio_buffer.h"
#include "sonotide/error.h"
#include "sonotide/result.h"

namespace sonotide {

class render_callback {
public:
    virtual ~render_callback() = default;

    virtual result<void> on_render(audio_buffer_view buffer, stream_timestamp timestamp) = 0;
    virtual void on_stream_error(const error& stream_error) {
        (void)stream_error;
    }
};

class capture_callback {
public:
    virtual ~capture_callback() = default;

    virtual result<void> on_capture(const_audio_buffer_view buffer, stream_timestamp timestamp) = 0;
    virtual void on_stream_error(const error& stream_error) {
        (void)stream_error;
    }
};

}  // namespace sonotide

