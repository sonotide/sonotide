#pragma once

#include <Audioclient.h>

#include <memory>

#include "sonotide/audio_format.h"
#include "sonotide/result.h"

namespace sonotide::detail::win {

/// RAII deleter для WAVEFORMATEX, выделенного через `CoTaskMemAlloc`.
struct cotaskmem_deleter {
    /// Освобождает формат, если указатель не пустой.
    void operator()(WAVEFORMATEX* format) const noexcept;
};

/// Удобный alias для владения `WAVEFORMATEX` через `std::unique_ptr`.
using unique_wave_format = std::unique_ptr<WAVEFORMATEX, cotaskmem_deleter>;

/// Результат выбора конкретного формата для shared-mode WASAPI.
struct negotiated_format {
    /// Владение нативным `WAVEFORMATEX` / `WAVEFORMATEXTENSIBLE`.
    unique_wave_format wave_format;
    /// Публичное представление формата для API и callbacks.
    audio_format public_format;
    /// Размер одного interleaved frame в bytes.
    std::uint32_t block_align = 0;
};

/// Преобразует нативный WAVEFORMATEX в публичный `sonotide::audio_format`.
[[nodiscard]] audio_format to_audio_format(const WAVEFORMATEX& format);
/// Клонирует WAVEFORMATEX в управляемую буферную копию.
[[nodiscard]] result<unique_wave_format> clone_wave_format(const WAVEFORMATEX& source);
/// Подбирает формат для shared-mode `IAudioClient`.
[[nodiscard]] result<negotiated_format> negotiate_shared_mode_format(
    IAudioClient& audio_client,
    const format_request& request);

}  // namespace sonotide::detail::win
