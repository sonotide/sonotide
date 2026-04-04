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
    assert(machine.state() == sonotide::stream_state::prepared);

    // Start допускается только после подготовки и переводит поток в running.
    auto running = machine.transition(sonotide::detail::stream_transition::start);
    assert(running.has_value());
    assert(running.value() == sonotide::stream_state::running);
    assert(machine.state() == sonotide::stream_state::running);

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

    // Повторный prepare из prepared тоже должен отклоняться.
    auto duplicate_prepare = machine.transition(sonotide::detail::stream_transition::prepare);
    assert(!duplicate_prepare.has_value());

    // Fault из running должен быть допустим и позволять subsequent reset.
    auto running_again = machine.transition(sonotide::detail::stream_transition::start);
    assert(running_again.has_value());
    auto faulted = machine.transition(sonotide::detail::stream_transition::fault);
    assert(faulted.has_value());
    assert(faulted.value() == sonotide::stream_state::faulted);
    assert(machine.state() == sonotide::stream_state::faulted);

    auto reset_from_fault = machine.transition(sonotide::detail::stream_transition::reset);
    assert(reset_from_fault.has_value());
    assert(reset_from_fault.value() == sonotide::stream_state::prepared);

    // Close должен завершать автомат и запрещать дальнейшие переходы.
    auto closed = machine.transition(sonotide::detail::stream_transition::close);
    assert(closed.has_value());
    assert(closed.value() == sonotide::stream_state::closed);
    assert(machine.state() == sonotide::stream_state::closed);
    auto start_after_close = machine.transition(sonotide::detail::stream_transition::start);
    assert(!start_after_close.has_value());

    // can_transition должен отражать и happy-path, и отказные сценарии.
    assert(sonotide::detail::stream_state_machine::can_transition(
        sonotide::stream_state::created,
        sonotide::detail::stream_transition::prepare));
    assert(!sonotide::detail::stream_state_machine::can_transition(
        sonotide::stream_state::created,
        sonotide::detail::stream_transition::start));
    assert(sonotide::detail::stream_state_machine::can_transition(
        sonotide::stream_state::running,
        sonotide::detail::stream_transition::fault));
    assert(!sonotide::detail::stream_state_machine::can_transition(
        sonotide::stream_state::closed,
        sonotide::detail::stream_transition::reset));
    return 0;
}
