#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "sonotide/audio_format.h"

namespace sonotide {

/// Снимок timestamp, связанный с вызовом stream callback.
struct stream_timestamp {
    /// Позиция устройства в кадрах.
    std::uint64_t device_position_frames = 0;
    /// Время high-resolution performance counter в единицах 100ns.
    std::uint64_t qpc_position_100ns = 0;
};

/// Изменяемый buffer view, передаваемый render callback.
struct audio_buffer_view {
    /// Сырые interleaved bytes, которыми владеет реализация потока.
    std::span<std::byte> bytes;
    /// Количество аудиокадров, описываемых span.
    std::uint32_t frame_count = 0;
    /// Согласованный формат для view.
    audio_format format;
};

/// Только для чтения buffer view, передаваемый capture callback.
struct const_audio_buffer_view {
    /// Сырые interleaved bytes, которыми владеет реализация потока.
    std::span<const std::byte> bytes;
    /// Количество аудиокадров, описываемых span.
    std::uint32_t frame_count = 0;
    /// Согласованный формат для view.
    audio_format format;
};

}  // namespace sonotide
