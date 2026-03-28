#include <cassert>

#include "internal/state_machine.h"

int main() {
    // Новая машина состояний стартует из created, до какой-либо подготовки.
    sonotide::detail::stream_state_machine machine;
    assert(machine.state() == sonotide::stream_state::created);

    // Prepare переводит поток в состояние prepared.
    auto prepared = machine.transition(sonotide::detail::stream_transition::prepare);
    assert(prepared.has_value());
    assert(prepared.value() == sonotide::stream_state::prepared);

    // Start допускается только после подготовки и переводит поток в running.
    auto running = machine.transition(sonotide::detail::stream_transition::start);
    assert(running.has_value());
    assert(running.value() == sonotide::stream_state::running);

    // Stop должен перевести поток в stopped и оставить путь к повторному запуску.
    auto stopped = machine.transition(sonotide::detail::stream_transition::stop);
    assert(stopped.has_value());
    assert(stopped.value() == sonotide::stream_state::stopped);

    // Reset возвращает машину обратно в prepared без пересборки объекта.
    auto reset = machine.transition(sonotide::detail::stream_transition::reset);
    assert(reset.has_value());
    assert(reset.value() == sonotide::stream_state::prepared);

    // Остановка из prepared незаконна и обязана быть отклонена.
    auto illegal = machine.transition(sonotide::detail::stream_transition::stop);
    assert(!illegal.has_value());
    return 0;
}
