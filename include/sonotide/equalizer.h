#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sonotide {

/// Количество фиксированных полос, которые экспонирует встроенный эквалайзер Sonotide.
inline constexpr std::size_t equalizer_band_count = 10;

/// Поддерживаемые пресеты эквалайзера, которые экспонирует framework.
enum class equalizer_preset_id {
    /// Плоская АЧХ со всеми полосами на 0 dB.
    flat,
    /// Пресет для усиления low-end.
    bass_boost,
    /// Пресет для усиления high-end.
    treble_boost,
    /// Пресет для разборчивости вокала.
    vocal,
    /// Пресет, ориентированный на pop.
    pop,
    /// Пресет, ориентированный на rock.
    rock,
    /// Пресет, ориентированный на electronic.
    electronic,
    /// Пресет, ориентированный на hip-hop.
    hip_hop,
    /// Пресет, ориентированный на jazz.
    jazz,
    /// Пресет, ориентированный на classical.
    classical,
    /// Пресет, ориентированный на acoustic.
    acoustic,
    /// Пресет, ориентированный на dance.
    dance,
    /// Пресет, ориентированный на piano.
    piano,
    /// Пресет для spoken-word или podcast.
    spoken_podcast,
    /// Пресет для ощущения loudness.
    loudness,
    /// Пользовательское состояние, полученное из ручных правок полос.
    custom,
};

/// Высокоуровневое состояние доступности equalizer pipeline.
enum class equalizer_status {
    /// Метаданные эквалайзера ещё готовятся.
    loading,
    /// Эквалайзер готов и может обрабатывать аудио.
    ready,
    /// Текущий audio path не поддерживает такую EQ-конфигурацию.
    unsupported_audio_path,
    /// Аудиодвижок недоступен.
    audio_engine_unavailable,
    /// Эквалайзер находится в состоянии ошибки.
    error,
};

/// Одна полоса 10-band эквалайзера.
struct equalizer_band {
    /// Фиксированная центральная частота полосы в Hz.
    float center_frequency_hz = 0.0F;
    /// Усиление, применяемое к полосе, в dB.
    float gain_db = 0.0F;
};

/// Определение встроенного preset с заголовком и 10 band gains.
struct equalizer_preset {
    /// Идентификатор preset.
    equalizer_preset_id id = equalizer_preset_id::flat;
    /// Человекочитаемый заголовок preset.
    const char* title = "Flat";
    /// Усиление по каждой полосе в dB.
    std::array<float, equalizer_band_count> gains_db{};
};

/// Снимок конфигурации эквалайзера и вычисленных метаданных.
struct equalizer_state {
    /// Текущее состояние доступности эквалайзера.
    equalizer_status status = equalizer_status::loading;
    /// `true`, когда EQ processing path включён.
    bool enabled = false;
    /// Текущий активный preset id.
    equalizer_preset_id active_preset_id = equalizer_preset_id::flat;
    /// Текущие gain полос.
    std::array<equalizer_band, equalizer_band_count> bands{};
    /// Последний пользовательский non-flat snapshot полос.
    std::array<float, equalizer_band_count> last_nonflat_band_gains_db{};
    /// Встроенные presets, доступные вызывающему коду.
    std::vector<equalizer_preset> available_presets;
    /// Дополнительный output level после EQ processing.
    float output_gain_db = 0.0F;
    /// Автоматическая preamp-компенсация, рассчитанная по кривой полос.
    float headroom_compensation_db = 0.0F;
    /// Человекочитаемое сообщение об ошибке, если EQ не готов.
    std::string error_message;
};

/// Преобразует идентификатор preset в стабильный строковый токен.
[[nodiscard]] inline std::string_view to_string(const equalizer_preset_id preset_id) {
    switch (preset_id) {
        case equalizer_preset_id::flat:
            return "flat";
        case equalizer_preset_id::bass_boost:
            return "bass_boost";
        case equalizer_preset_id::treble_boost:
            return "treble_boost";
        case equalizer_preset_id::vocal:
            return "vocal";
        case equalizer_preset_id::pop:
            return "pop";
        case equalizer_preset_id::rock:
            return "rock";
        case equalizer_preset_id::electronic:
            return "electronic";
        case equalizer_preset_id::hip_hop:
            return "hip_hop";
        case equalizer_preset_id::jazz:
            return "jazz";
        case equalizer_preset_id::classical:
            return "classical";
        case equalizer_preset_id::acoustic:
            return "acoustic";
        case equalizer_preset_id::dance:
            return "dance";
        case equalizer_preset_id::piano:
            return "piano";
        case equalizer_preset_id::spoken_podcast:
            return "spoken_podcast";
        case equalizer_preset_id::loudness:
            return "loudness";
        case equalizer_preset_id::custom:
            return "custom";
    }

    return "custom";
}

/// Преобразует EQ status в стабильный строковый токен.
[[nodiscard]] inline std::string_view to_string(const equalizer_status status) {
    switch (status) {
        case equalizer_status::loading:
            return "loading";
        case equalizer_status::ready:
            return "ready";
        case equalizer_status::unsupported_audio_path:
            return "unsupported_audio_path";
        case equalizer_status::audio_engine_unavailable:
            return "audio_engine_unavailable";
        case equalizer_status::error:
            return "error";
    }

    return "error";
}

/// Парсит идентификатор preset из строкового токена.
[[nodiscard]] inline std::optional<equalizer_preset_id> equalizer_preset_id_from_string(
    const std::string_view value) {
    if (value == "flat") {
        return equalizer_preset_id::flat;
    }
    if (value == "bass_boost") {
        return equalizer_preset_id::bass_boost;
    }
    if (value == "treble_boost") {
        return equalizer_preset_id::treble_boost;
    }
    if (value == "vocal") {
        return equalizer_preset_id::vocal;
    }
    if (value == "pop") {
        return equalizer_preset_id::pop;
    }
    if (value == "rock") {
        return equalizer_preset_id::rock;
    }
    if (value == "electronic") {
        return equalizer_preset_id::electronic;
    }
    if (value == "hip_hop") {
        return equalizer_preset_id::hip_hop;
    }
    if (value == "jazz") {
        return equalizer_preset_id::jazz;
    }
    if (value == "classical") {
        return equalizer_preset_id::classical;
    }
    if (value == "acoustic") {
        return equalizer_preset_id::acoustic;
    }
    if (value == "dance") {
        return equalizer_preset_id::dance;
    }
    if (value == "piano") {
        return equalizer_preset_id::piano;
    }
    if (value == "spoken_podcast") {
        return equalizer_preset_id::spoken_podcast;
    }
    if (value == "loudness") {
        return equalizer_preset_id::loudness;
    }
    if (value == "custom") {
        return equalizer_preset_id::custom;
    }

    return std::nullopt;
}

}  // namespace sonotide
