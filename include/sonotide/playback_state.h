#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "sonotide/audio_format.h"

namespace sonotide {

/// Высокоуровневый статус жизненного цикла воспроизведения.
enum class playback_status {
    /// Источник не загружен или воспроизведение завершено.
    idle,
    /// Источник открывается, декодируется или иначе готовится.
    loading,
    /// Аудио активно воспроизводится.
    playing,
    /// Воспроизведение приостановлено, но источник остаётся загруженным.
    paused,
    /// Произошла фатальная ошибка.
    error,
};

/// Снимок transport, источника и метаданных устройства воспроизведения.
struct playback_state {
    /// Текущий статус воспроизведения.
    playback_status status = playback_status::idle;
    /// Активный URI или путь источника.
    std::string source_uri;
    /// Человекочитаемое сообщение об ошибке.
    std::string error_message;
    /// Идентификатор предпочтительного выходного устройства, запрошенный вызывающим кодом.
    std::string preferred_output_device_id;
    /// Идентификатор endpoint-а, который сейчас активен в backend-е, если известен.
    std::string active_output_device_id;
    /// Человекочитаемое имя активного endpoint-а.
    std::string active_output_device_name;
    /// Текущая позиция воспроизведения в миллисекундах.
    std::int64_t position_ms = 0;
    /// Длительность источника в миллисекундах.
    std::int64_t duration_ms = 0;
    /// Пользовательская громкость в процентах.
    int volume_percent = 100;
    /// Монотонный token, увеличиваемый при достижении конца воспроизведения.
    std::uint64_t completion_token = 0;
    /// Согласованный выходной формат, если воспроизведение уже совпало с форматом устройства.
    std::optional<audio_format> negotiated_format;
    /// Истина, если активное устройство является системным устройством по умолчанию.
    bool active_output_device_is_default = false;
    /// Истина, если текущий endpoint был инвалидирован или сейчас недоступен.
    bool device_lost = false;
};

}  // namespace sonotide
