#pragma once

#include <Audioclient.h>

#include <memory>

#include "sonotide/audio_format.h"
#include "sonotide/result.h"

namespace sonotide::detail::win {

struct cotaskmem_deleter {
    void operator()(WAVEFORMATEX* format) const noexcept;
};

using unique_wave_format = std::unique_ptr<WAVEFORMATEX, cotaskmem_deleter>;

struct negotiated_format {
    unique_wave_format wave_format;
    audio_format public_format;
    std::uint32_t block_align = 0;
};

[[nodiscard]] audio_format to_audio_format(const WAVEFORMATEX& format);
[[nodiscard]] result<unique_wave_format> clone_wave_format(const WAVEFORMATEX& source);
[[nodiscard]] result<negotiated_format> negotiate_shared_mode_format(
    IAudioClient& audio_client,
    const format_request& request);

}  // namespace sonotide::detail::win

