#pragma once

#include <chrono>
#include <optional>

#include "sonotide/audio_format.h"
#include "sonotide/device_selector.h"

namespace sonotide {

/// Режим совместного использования WASAPI, запрошенный вызывающим кодом.
enum class share_mode {
    /// Использовать shared mode и device mix format.
    shared,
    /// Использовать exclusive mode, когда backend это поддерживает.
    exclusive,
};

/// Модель планирования callback для потока.
enum class callback_mode {
    /// Событийные уведомления через callback.
    event_driven,
};

/// Тайминговые предпочтения потока.
struct stream_timing {
    /// Желаемая end-to-end latency.
    std::chrono::milliseconds target_latency{20};
    /// Необязательный override периода engine для продвинутых сценариев.
    std::optional<std::chrono::microseconds> engine_period;
};

/// Конфигурация render stream.
struct render_stream_config {
    /// Целевой output endpoint.
    device_selector device = device_selector::system_default(device_direction::render);
    /// Запрошенный WASAPI share mode.
    share_mode mode = share_mode::shared;
    /// Запрошенная модель callback.
    callback_mode callback = callback_mode::event_driven;
    /// Предпочтения по аудиоформату.
    format_request format;
    /// Тайминговые предпочтения потока.
    stream_timing timing;
    /// Пытаться восстановиться после потери устройства.
    bool auto_recover_device_loss = true;
    /// Предзаполнять output buffer тишиной до старта аудио.
    bool prefill_with_silence = true;
};

/// Конфигурация capture stream.
struct capture_stream_config {
    /// Целевой capture endpoint.
    device_selector device = device_selector::system_default(device_direction::capture);
    /// Запрошенный WASAPI share mode.
    share_mode mode = share_mode::shared;
    /// Запрошенная модель callback.
    callback_mode callback = callback_mode::event_driven;
    /// Предпочтения по аудиоформату.
    format_request format;
    /// Тайминговые предпочтения потока.
    stream_timing timing;
    /// Пытаться восстановиться после потери устройства.
    bool auto_recover_device_loss = true;
    /// Отдавать тишину вместо ошибки при capture glitch.
    bool deliver_silence_on_glitch = false;
};

/// Конфигурация loopback capture stream.
struct loopback_stream_config {
    /// Целевой render endpoint, output которого нужно захватывать.
    device_selector device = device_selector::system_default(device_direction::render);
    /// Запрошенный WASAPI share mode.
    share_mode mode = share_mode::shared;
    /// Запрошенная модель callback.
    callback_mode callback = callback_mode::event_driven;
    /// Предпочтения по аудиоформату.
    format_request format;
    /// Тайминговые предпочтения потока.
    stream_timing timing;
    /// Пытаться восстановиться после потери устройства.
    bool auto_recover_device_loss = true;
};

}  // namespace sonotide
