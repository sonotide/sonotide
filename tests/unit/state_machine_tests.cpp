#include <cassert>

#include "internal/state_machine.h"

int main() {
    sonotide::detail::stream_state_machine machine;
    assert(machine.state() == sonotide::stream_state::created);

    auto prepared = machine.transition(sonotide::detail::stream_transition::prepare);
    assert(prepared.has_value());
    assert(prepared.value() == sonotide::stream_state::prepared);

    auto running = machine.transition(sonotide::detail::stream_transition::start);
    assert(running.has_value());
    assert(running.value() == sonotide::stream_state::running);

    auto stopped = machine.transition(sonotide::detail::stream_transition::stop);
    assert(stopped.has_value());
    assert(stopped.value() == sonotide::stream_state::stopped);

    auto reset = machine.transition(sonotide::detail::stream_transition::reset);
    assert(reset.has_value());
    assert(reset.value() == sonotide::stream_state::prepared);

    auto illegal = machine.transition(sonotide::detail::stream_transition::stop);
    assert(!illegal.has_value());
    return 0;
}
