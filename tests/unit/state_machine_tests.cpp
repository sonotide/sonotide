#include "internal/state_machine.h"
#include "test_support/test_harness.h"

int main() {
    // Новая машина состояний стартует из created, до какой-либо подготовки.
    sonotide::detail::stream_state_machine machine;
    REQUIRE(machine.state() == sonotide::stream_state::created);

    // Prepare переводит поток в состояние prepared.
    auto prepared = machine.transition(sonotide::detail::stream_transition::prepare);
    REQUIRE(prepared.has_value());
    REQUIRE(prepared.value() == sonotide::stream_state::prepared);
    REQUIRE(machine.state() == sonotide::stream_state::prepared);

    // Start допускается только после подготовки и переводит поток в running.
    auto running = machine.transition(sonotide::detail::stream_transition::start);
    REQUIRE(running.has_value());
    REQUIRE(running.value() == sonotide::stream_state::running);
    REQUIRE(machine.state() == sonotide::stream_state::running);

    // Stop должен перевести поток в stopped и оставить путь к повторному запуску.
    auto stopped = machine.transition(sonotide::detail::stream_transition::stop);
    REQUIRE(stopped.has_value());
    REQUIRE(stopped.value() == sonotide::stream_state::stopped);

    // Reset возвращает машину обратно в prepared без пересборки объекта.
    auto reset = machine.transition(sonotide::detail::stream_transition::reset);
    REQUIRE(reset.has_value());
    REQUIRE(reset.value() == sonotide::stream_state::prepared);

    // Остановка из prepared незаконна и обязана быть отклонена.
    auto illegal = machine.transition(sonotide::detail::stream_transition::stop);
    REQUIRE(!illegal.has_value());

    // Повторный prepare из prepared тоже должен отклоняться.
    auto duplicate_prepare = machine.transition(sonotide::detail::stream_transition::prepare);
    REQUIRE(!duplicate_prepare.has_value());

    // Fault из running должен быть допустим и позволять subsequent reset.
    auto running_again = machine.transition(sonotide::detail::stream_transition::start);
    REQUIRE(running_again.has_value());
    auto faulted = machine.transition(sonotide::detail::stream_transition::fault);
    REQUIRE(faulted.has_value());
    REQUIRE(faulted.value() == sonotide::stream_state::faulted);
    REQUIRE(machine.state() == sonotide::stream_state::faulted);

    auto reset_from_fault = machine.transition(sonotide::detail::stream_transition::reset);
    REQUIRE(reset_from_fault.has_value());
    REQUIRE(reset_from_fault.value() == sonotide::stream_state::prepared);

    // Close должен завершать автомат и запрещать дальнейшие переходы.
    auto closed = machine.transition(sonotide::detail::stream_transition::close);
    REQUIRE(closed.has_value());
    REQUIRE(closed.value() == sonotide::stream_state::closed);
    REQUIRE(machine.state() == sonotide::stream_state::closed);
    auto start_after_close = machine.transition(sonotide::detail::stream_transition::start);
    REQUIRE(!start_after_close.has_value());

    // can_transition должен отражать и happy-path, и отказные сценарии.
    REQUIRE(sonotide::detail::stream_state_machine::can_transition(
        sonotide::stream_state::created,
        sonotide::detail::stream_transition::prepare));
    REQUIRE(!sonotide::detail::stream_state_machine::can_transition(
        sonotide::stream_state::created,
        sonotide::detail::stream_transition::start));
    REQUIRE(sonotide::detail::stream_state_machine::can_transition(
        sonotide::stream_state::running,
        sonotide::detail::stream_transition::fault));
    REQUIRE(!sonotide::detail::stream_state_machine::can_transition(
        sonotide::stream_state::closed,
        sonotide::detail::stream_transition::reset));
    return 0;
}
