#pragma once

namespace sonotide {

enum class stream_state {
    created,
    prepared,
    running,
    stopped,
    faulted,
    closed,
};

}  // namespace sonotide

