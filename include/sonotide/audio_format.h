#pragma once

#include <cstdint>
#include <optional>

namespace sonotide {

/// Описывает кодирование сэмплов, используемое потоком или форматом устройства.
enum class sample_type {
    /// Формат ещё не согласован.
    unknown,
    /// Знаковые 16-битные PCM сэмплы.
    pcm_i16,
    /// Упакованные 24-битные PCM сэмплы внутри 32-битных контейнеров.
    pcm_i24_in_32,
    /// Знаковые 32-битные PCM сэмплы.
    pcm_i32,
    /// 32-битные IEEE float PCM сэмплы.
    float32,
};

/// Полностью согласованный аудиоформат потока или устройства.
struct audio_format {
    /// Кодирование сэмплов.
    sample_type sample = sample_type::unknown;
    /// Частота дискретизации в Hz.
    std::uint32_t sample_rate = 0;
    /// Количество interleaved-каналов в кадре.
    std::uint16_t channel_count = 0;
    /// Количество бит в контейнере на сэмпл.
    std::uint16_t bits_per_sample = 0;
    /// Полезные биты на сэмпл, если контейнер шире полезной нагрузки.
    std::uint16_t valid_bits_per_sample = 0;
    /// Маска каналов, если устройство её сообщает.
    std::uint32_t channel_mask = 0;
    /// `true`, когда сэмплы уложены interleaved.
    bool interleaved = true;
};

/// Пользовательские предпочтения для согласования формата.
struct format_request {
    /// Предпочитаемое кодирование сэмплов, если negotiation нужно ограничить.
    std::optional<sample_type> preferred_sample;
    /// Предпочитаемая частота дискретизации, если negotiation нужно ограничить.
    std::optional<std::uint32_t> preferred_sample_rate;
    /// Предпочитаемое число каналов, если negotiation нужно ограничить.
    std::optional<std::uint16_t> preferred_channel_count;
    /// `true`, когда требуется interleaved-раскладка.
    bool interleaved = true;
    /// `true`, когда реализация может откатиться к device mix format.
    bool allow_mix_format_fallback = true;
};

}  // namespace sonotide
