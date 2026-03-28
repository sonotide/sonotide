#pragma once

#include "sonotide/result.h"
#include "sonotide/stream_state.h"

namespace sonotide::detail {

enum class stream_transition {
    prepare,
    start,
    stop,
    reset,
    fault,
    close,
};

class stream_state_machine {
public:
    [[nodiscard]] stream_state state() const noexcept {
        return state_;
    }

    [[nodiscard]] result<stream_state> transition(stream_transition transition);
    [[nodiscard]] static bool can_transition(
        stream_state current,
        stream_transition transition) noexcept;

private:
    stream_state state_ = stream_state::created;
};

}  // namespace sonotide::detail

