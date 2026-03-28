#pragma once

#include <cstdint>
#include <optional>

#include "sonotide/audio_format.h"
#include "sonotide/stream_state.h"

namespace sonotide {

/// Статистика выполнения, собираемая stream handle.
struct stream_statistics {
    /// Количество вызовов callback на данный момент.
    std::uint64_t callback_count = 0;
    /// Общее число кадров, обработанных worker потока.
    std::uint64_t frames_processed = 0;
    /// Количество разрывов или discontinuity во время стрима.
    std::uint64_t discontinuity_count = 0;
};

/// Снимок текущего жизненного цикла и состояния согласования потока.
struct stream_status {
    /// Текущее состояние жизненного цикла.
    stream_state state = stream_state::created;
    /// Формат, запрошенный пользователем до negotiation.
    audio_format requested_format;
    /// Фактический формат, согласованный с устройством, если он есть.
    std::optional<audio_format> negotiated_format;
    /// Runtime-статистика для диагностики и телеметрии.
    stream_statistics statistics;
    /// `true`, когда endpoint исчез или был invalidated.
    bool device_lost = false;
};

}  // namespace sonotide
