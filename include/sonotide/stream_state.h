#pragma once

namespace sonotide {

/// Состояние жизненного цикла дескриптора потока в Sonotide.
enum class stream_state {
    /// Объект потока существует, но ещё не подготовлен.
    created,
    /// Поток подготовлен и может быть запущен.
    prepared,
    /// Рабочий поток активно работает.
    running,
    /// Рабочий поток остановлен.
    stopped,
    /// Поток столкнулся с runtime fault.
    faulted,
    /// Дескриптор потока закрыт навсегда.
    closed,
};

}  // namespace sonotide
