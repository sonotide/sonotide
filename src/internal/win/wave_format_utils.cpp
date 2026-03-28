#include "internal/win/wave_format_utils.h"

#include <ksmedia.h>

#include <cstring>

namespace sonotide::detail::win {
namespace {

sample_type detect_sample_type(const WAVEFORMATEX& format) {
    if (format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        return sample_type::float32;
    }

    if (format.wFormatTag == WAVE_FORMAT_PCM) {
        if (format.wBitsPerSample == 16) {
            return sample_type::pcm_i16;
        }
        if (format.wBitsPerSample == 32) {
            return sample_type::pcm_i32;
        }
    }

    if (format.wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        format.cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const auto* extensible =
            reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(&format);
        if (extensible->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) {
            return sample_type::float32;
        }
        if (extensible->SubFormat == KSDATAFORMAT_SUBTYPE_PCM) {
            if (extensible->Samples.wValidBitsPerSample == 24 &&
                format.wBitsPerSample == 32) {
                return sample_type::pcm_i24_in_32;
            }
            if (format.wBitsPerSample == 16) {
                return sample_type::pcm_i16;
            }
            if (format.wBitsPerSample == 32) {
                return sample_type::pcm_i32;
            }
        }
    }

    return sample_type::unknown;
}

result<unique_wave_format> make_requested_wave_format(const format_request& request) {
    if (!request.preferred_sample || !request.preferred_sample_rate ||
        !request.preferred_channel_count) {
        error failure;
        failure.category = error_category::configuration;
        failure.code = error_code::invalid_argument;
        failure.operation = "make_requested_wave_format";
        failure.message =
            "Explicit format negotiation requires preferred sample type, sample rate, and channel count.";
        return result<unique_wave_format>::failure(std::move(failure));
    }

    auto format = unique_wave_format(
        reinterpret_cast<WAVEFORMATEX*>(CoTaskMemAlloc(sizeof(WAVEFORMATEXTENSIBLE))));
    if (!format) {
        error failure;
        failure.category = error_category::platform;
        failure.code = error_code::platform_failure;
        failure.operation = "CoTaskMemAlloc";
        failure.message = "Failed to allocate WAVEFORMATEXTENSIBLE.";
        return result<unique_wave_format>::failure(std::move(failure));
    }

    std::memset(format.get(), 0, sizeof(WAVEFORMATEXTENSIBLE));
    auto* extensible = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(format.get());
    extensible->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    extensible->Format.nChannels = *request.preferred_channel_count;
    extensible->Format.nSamplesPerSec = *request.preferred_sample_rate;
    extensible->Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    extensible->dwChannelMask = (*request.preferred_channel_count >= 2) ? KSAUDIO_SPEAKER_STEREO : SPEAKER_FRONT_CENTER;

    switch (*request.preferred_sample) {
    case sample_type::float32:
        extensible->Format.wBitsPerSample = 32;
        extensible->Samples.wValidBitsPerSample = 32;
        extensible->SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        break;
    case sample_type::pcm_i16:
        extensible->Format.wBitsPerSample = 16;
        extensible->Samples.wValidBitsPerSample = 16;
        extensible->SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
        break;
    case sample_type::pcm_i24_in_32:
        extensible->Format.wBitsPerSample = 32;
        extensible->Samples.wValidBitsPerSample = 24;
        extensible->SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
        break;
    case sample_type::pcm_i32:
        extensible->Format.wBitsPerSample = 32;
        extensible->Samples.wValidBitsPerSample = 32;
        extensible->SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
        break;
    case sample_type::unknown:
        break;
    }

    extensible->Format.nBlockAlign =
        static_cast<WORD>(extensible->Format.nChannels * (extensible->Format.wBitsPerSample / 8));
    extensible->Format.nAvgBytesPerSec =
        extensible->Format.nBlockAlign * extensible->Format.nSamplesPerSec;

    return result<unique_wave_format>::success(std::move(format));
}

}  // namespace

void cotaskmem_deleter::operator()(WAVEFORMATEX* format) const noexcept {
    if (format != nullptr) {
        CoTaskMemFree(format);
    }
}

audio_format to_audio_format(const WAVEFORMATEX& format) {
    audio_format public_format;
    public_format.sample = detect_sample_type(format);
    public_format.sample_rate = format.nSamplesPerSec;
    public_format.channel_count = format.nChannels;
    public_format.bits_per_sample = format.wBitsPerSample;
    public_format.valid_bits_per_sample = format.wBitsPerSample;
    public_format.interleaved = true;

    if (format.wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        format.cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const auto* extensible =
            reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(&format);
        public_format.valid_bits_per_sample = extensible->Samples.wValidBitsPerSample;
        public_format.channel_mask = extensible->dwChannelMask;
    }

    return public_format;
}

result<unique_wave_format> clone_wave_format(const WAVEFORMATEX& source) {
    const auto total_size = sizeof(WAVEFORMATEX) + source.cbSize;
    auto cloned = unique_wave_format(
        reinterpret_cast<WAVEFORMATEX*>(CoTaskMemAlloc(total_size)));
    if (!cloned) {
        error failure;
        failure.category = error_category::platform;
        failure.code = error_code::platform_failure;
        failure.operation = "CoTaskMemAlloc";
        failure.message = "Failed to clone WAVEFORMATEX.";
        return result<unique_wave_format>::failure(std::move(failure));
    }

    std::memcpy(cloned.get(), &source, total_size);
    return result<unique_wave_format>::success(std::move(cloned));
}

result<negotiated_format> negotiate_shared_mode_format(
    IAudioClient& audio_client,
    const format_request& request) {
    WAVEFORMATEX* mix_format_raw = nullptr;
    HRESULT hr = audio_client.GetMixFormat(&mix_format_raw);
    if (FAILED(hr) || mix_format_raw == nullptr) {
        error failure;
        failure.category = error_category::format;
        failure.code = error_code::format_negotiation_failed;
        failure.message = "Failed to query device mix format.";
        failure.operation = "IAudioClient::GetMixFormat";
        failure.native_code = static_cast<long>(hr);
        return result<negotiated_format>::failure(std::move(failure));
    }

    unique_wave_format mix_format(mix_format_raw);
    unique_wave_format selected_format;

    if (request.preferred_sample && request.preferred_sample_rate &&
        request.preferred_channel_count) {
        auto requested_result = make_requested_wave_format(request);
        if (!requested_result) {
            return result<negotiated_format>::failure(requested_result.error());
        }

        WAVEFORMATEX* closest_raw = nullptr;
        hr = audio_client.IsFormatSupported(
            AUDCLNT_SHAREMODE_SHARED,
            requested_result.value().get(),
            &closest_raw);

        if (hr == S_OK) {
            selected_format = std::move(requested_result.value());
        } else if (hr == S_FALSE && closest_raw != nullptr && request.allow_mix_format_fallback) {
            selected_format.reset(closest_raw);
        } else if (request.allow_mix_format_fallback) {
            auto cloned_mix_result = clone_wave_format(*mix_format.get());
            if (!cloned_mix_result) {
                return result<negotiated_format>::failure(cloned_mix_result.error());
            }
            selected_format = std::move(cloned_mix_result.value());
        } else {
            if (closest_raw != nullptr) {
                CoTaskMemFree(closest_raw);
            }
            error failure;
            failure.category = error_category::format;
            failure.code = error_code::format_negotiation_failed;
            failure.message = "Requested format is not supported by the selected endpoint.";
            failure.operation = "IAudioClient::IsFormatSupported";
            failure.native_code = static_cast<long>(hr);
            return result<negotiated_format>::failure(std::move(failure));
        }
    } else {
        auto cloned_mix_result = clone_wave_format(*mix_format.get());
        if (!cloned_mix_result) {
            return result<negotiated_format>::failure(cloned_mix_result.error());
        }
        selected_format = std::move(cloned_mix_result.value());
    }

    negotiated_format negotiated;
    negotiated.block_align = selected_format->nBlockAlign;
    negotiated.public_format = to_audio_format(*selected_format.get());
    negotiated.wave_format = std::move(selected_format);
    return result<negotiated_format>::success(std::move(negotiated));
}

}  // namespace sonotide::detail::win
