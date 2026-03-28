#pragma once

#include "sonotide/result.h"
#include "sonotide/stream_state.h"

namespace sonotide::detail {

/// Событие конечного автомата, переводящее поток через этапы жизненного цикла.
enum class stream_transition {
    /// Переводит только что созданный поток в состояние prepared.
    prepare,
    /// Запускает рабочий поток и начинает обработку.
    start,
    /// Останавливает рабочий поток без уничтожения дескриптора.
    stop,
    /// Возвращает поток в состояниях faulted или stopped обратно в prepared.
    reset,
    /// Помечает поток как faulted после ошибки выполнения.
    fault,
    /// Полностью закрывает дескриптор потока.
    close,
};

/// Минимальный конечный автомат жизненного цикла для дескриптора потока на базе WASAPI.
class stream_state_machine {
public:
    /// Возвращает текущий снимок состояния жизненного цикла.
    [[nodiscard]] stream_state state() const noexcept {
        return state_;
    }

    /// Пытается перевести поток в новое состояние жизненного цикла.
    [[nodiscard]] result<stream_state> transition(stream_transition transition);
    /// Проверяет, допустим ли переход из указанного состояния.
    [[nodiscard]] static bool can_transition(
        stream_state current,
        stream_transition transition) noexcept;

private:
    /// Внутреннее состояние жизненного цикла, которое отслеживает реализация дескриптора.
    stream_state state_ = stream_state::created;
};

}  // namespace sonotide::detail
