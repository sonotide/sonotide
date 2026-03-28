#include "internal/state_machine.h"

#include <string>

namespace sonotide::detail {
namespace {

/// Формирует ошибку потока, поясняющую, какой переход был отклонён.
error make_invalid_transition_error(
    const stream_state state,
    const stream_transition transition) {
    /// В сообщении сохраняем текущее состояние и запрошенный переход, чтобы диагностика оставалась полезной.
    error failure;
    failure.category = error_category::stream;
    failure.code = error_code::invalid_state;
    failure.operation = "stream_state_machine::transition";
    failure.message = "Illegal stream transition from state " +
        std::to_string(static_cast<int>(state)) +
        " using transition " +
        std::to_string(static_cast<int>(transition)) + ".";
    return failure;
}

/// Сопоставляет событие перехода с конкретным следующим состоянием потока.
stream_state next_state(const stream_state current, const stream_transition transition) {
    switch (transition) {
    case stream_transition::prepare:
        return stream_state::prepared;
    case stream_transition::start:
        return stream_state::running;
    case stream_transition::stop:
        return stream_state::stopped;
    case stream_transition::reset:
        return stream_state::prepared;
    case stream_transition::fault:
        return stream_state::faulted;
    case stream_transition::close:
        return stream_state::closed;
    }

    return current;
}

}  // namespace

/// Проверяет, допустим ли переход для указанного состояния жизненного цикла.
bool stream_state_machine::can_transition(
    const stream_state current,
    const stream_transition transition) noexcept {
    switch (current) {
    case stream_state::created:
        return transition == stream_transition::prepare ||
               transition == stream_transition::fault ||
               transition == stream_transition::close;
    case stream_state::prepared:
        return transition == stream_transition::start ||
               transition == stream_transition::reset ||
               transition == stream_transition::fault ||
               transition == stream_transition::close;
    case stream_state::running:
        return transition == stream_transition::stop ||
               transition == stream_transition::fault ||
               transition == stream_transition::close;
    case stream_state::stopped:
        return transition == stream_transition::start ||
               transition == stream_transition::reset ||
               transition == stream_transition::fault ||
               transition == stream_transition::close;
    case stream_state::faulted:
        return transition == stream_transition::reset ||
               transition == stream_transition::close;
    case stream_state::closed:
        return false;
    }

    return false;
}

/// Применяет допустимый переход и сохраняет получившийся снимок состояния.
result<stream_state> stream_state_machine::transition(const stream_transition transition) {
    if (!can_transition(state_, transition)) {
        return result<stream_state>::failure(make_invalid_transition_error(state_, transition));
    }

    /// Переход допустим, поэтому можно сдвинуть автомат вперёд за один шаг.
    state_ = next_state(state_, transition);
    return result<stream_state>::success(state_);
}

}  // пространство имён sonotide::detail
