#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace sonotide {

/// Группа ошибок, которую Sonotide использует для классификации сбоев.
enum class error_category {
    /// Неверный ввод или конфигурация.
    configuration,
    /// Ошибка инициализации или запуска.
    initialization,
    /// Ошибка поиска или выбора устройства.
    device,
    /// Ошибка negotiation формата.
    format,
    /// Ошибка lifecycle потока или обработки callback.
    stream,
    /// Нарушение контракта callback.
    callback,
    /// Ошибка, специфичная для платформы.
    platform,
};

/// Конкретный код ошибки, возвращаемый API Sonotide.
enum class error_code {
    /// Провал валидации ввода.
    invalid_argument,
    /// Операция была вызвана в неверном состоянии.
    invalid_state,
    /// Текущая платформа не поддерживает запрошенную операцию.
    unsupported_platform,
    /// Ошибка инициализации.
    initialization_failed,
    /// Запрошенное устройство не найдено.
    device_not_found,
    /// Ошибка перечисления устройств.
    device_enumeration_failed,
    /// Ошибка negotiation формата.
    format_negotiation_failed,
    /// Ошибка открытия потока.
    stream_open_failed,
    /// Ошибка запуска потока.
    stream_start_failed,
    /// Ошибка остановки потока.
    stream_stop_failed,
    /// Callback завершился ошибкой.
    callback_failed,
    /// Платформенная ошибка.
    platform_failure,
    /// Возможность ещё не реализована.
    not_implemented,
};

/// Подробный объект ошибки, возвращаемый операциями Sonotide.
struct error {
    /// Категория ошибки.
    error_category category = error_category::platform;
    /// Код ошибки внутри категории.
    error_code code = error_code::platform_failure;
    /// Человекочитаемое сообщение об ошибке.
    std::string message;
    /// Операция, которая породила ошибку.
    std::string operation;
    /// Нативный HRESULT или platform code, если доступен.
    std::optional<std::int64_t> native_code;
    /// `true`, когда вызывающий код может повторить попытку или восстановиться.
    bool recoverable = false;
};

/// Преобразует категорию ошибки в стабильное строковое представление.
[[nodiscard]] std::string_view to_string(error_category category) noexcept;
/// Преобразует код ошибки в стабильное строковое представление.
[[nodiscard]] std::string_view to_string(error_code code) noexcept;

}  // namespace sonotide
