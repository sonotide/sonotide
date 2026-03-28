#pragma once

#include "sonotide/audio_buffer.h"
#include "sonotide/error.h"
#include "sonotide/result.h"

namespace sonotide {

/// Базовый интерфейс для render-side stream callback.
class render_callback {
public:
    /// Виртуальный деструктор для derived callback.
    virtual ~render_callback() = default;

    /// Заполняет render buffer для текущего tick.
    virtual result<void> on_render(audio_buffer_view buffer, stream_timestamp timestamp) = 0;
    /// Получает асинхронное уведомление об ошибке потока.
    virtual void on_stream_error(const error& stream_error) {
        (void)stream_error;
    }
};

/// Базовый интерфейс для capture-side stream callback.
class capture_callback {
public:
    /// Виртуальный деструктор для derived callback.
    virtual ~capture_callback() = default;

    /// Потребляет captured buffer для текущего tick.
    virtual result<void> on_capture(const_audio_buffer_view buffer, stream_timestamp timestamp) = 0;
    /// Получает асинхронное уведомление об ошибке потока.
    virtual void on_stream_error(const error& stream_error) {
        (void)stream_error;
    }
};

}  // namespace sonotide
