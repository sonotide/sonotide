#include "internal/win/wave_format_utils.h"

#include <ksmedia.h>

#include <bit>
#include <cstring>
#include <limits>

namespace sonotide::detail::win {
namespace {

constexpr std::size_t kMaximumWaveFormatExtraBytes = 4096U;

result<void> invalid_wave_format(std::string operation, std::string message) {
    error failure;
    failure.category = error_category::format;
    failure.code = error_code::invalid_argument;
    failure.operation = std::move(operation);
    failure.message = std::move(message);
    return result<void>::failure(std::move(failure));
}

/// Определяет публичный тип sample-ов, который соответствует нативному WAVE формату.
sample_type detect_sample_type(const WAVEFORMATEX& format) {
    if (format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT && format.wBitsPerSample == 32U) {
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
        if (extensible->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT &&
            format.wBitsPerSample == 32U &&
            extensible->Samples.wValidBitsPerSample == 32U) {
            return sample_type::float32;
        }
        if (extensible->SubFormat == KSDATAFORMAT_SUBTYPE_PCM) {
            if (extensible->Samples.wValidBitsPerSample == 24 &&
                format.wBitsPerSample == 32) {
                return sample_type::pcm_i24_in_32;
            }
            if (format.wBitsPerSample == 16 &&
                extensible->Samples.wValidBitsPerSample == 16) {
                return sample_type::pcm_i16;
            }
            if (format.wBitsPerSample == 32 &&
                extensible->Samples.wValidBitsPerSample == 32) {
                return sample_type::pcm_i32;
            }
        }
    }

    return sample_type::unknown;
}

}  // namespace

result<void> validate_wave_format(const WAVEFORMATEX& format) {
    if (format.cbSize > kMaximumWaveFormatExtraBytes) {
        return invalid_wave_format(
            "validate_wave_format",
            "WAVE format extra data is unreasonably large.");
    }
    if (format.nChannels == 0U || format.nSamplesPerSec == 0U) {
        return invalid_wave_format(
            "validate_wave_format",
            "WAVE format channel count and sample rate must be non-zero.");
    }
    if (format.wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        format.cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        return invalid_wave_format(
            "validate_wave_format",
            "WAVE_FORMAT_EXTENSIBLE does not contain the required extension.");
    }
    if (format.wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(&format);
        if (extensible->dwChannelMask != 0U &&
            std::popcount(extensible->dwChannelMask) != format.nChannels) {
            return invalid_wave_format(
                "validate_wave_format",
                "WAVE channel-mask population does not match the channel count.");
        }
    }

    const sample_type detected_sample = detect_sample_type(format);
    if (detected_sample == sample_type::unknown) {
        return invalid_wave_format(
            "validate_wave_format",
            "WAVE format uses an unknown or unsupported sample layout.");
    }
    if (format.wBitsPerSample == 0U || format.wBitsPerSample % 8U != 0U) {
        return invalid_wave_format(
            "validate_wave_format",
            "WAVE format bits per sample must be a non-zero whole number of bytes.");
    }
    if ((detected_sample == sample_type::pcm_i16 && format.wBitsPerSample != 16U) ||
        (detected_sample != sample_type::pcm_i16 && format.wBitsPerSample != 32U)) {
        return invalid_wave_format(
            "validate_wave_format",
            "WAVE format sample type and container width are inconsistent.");
    }

    const std::uint64_t bytes_per_sample = format.wBitsPerSample / 8U;
    const std::uint64_t expected_block_align =
        static_cast<std::uint64_t>(format.nChannels) * bytes_per_sample;
    if (expected_block_align == 0U ||
        expected_block_align > (std::numeric_limits<WORD>::max)() ||
        format.nBlockAlign != expected_block_align) {
        return invalid_wave_format(
            "validate_wave_format",
            "WAVE format block alignment is zero, inconsistent, or overflows WORD.");
    }

    const std::uint64_t expected_average_bytes =
        expected_block_align * format.nSamplesPerSec;
    if (expected_average_bytes == 0U ||
        expected_average_bytes > (std::numeric_limits<DWORD>::max)() ||
        format.nAvgBytesPerSec != expected_average_bytes) {
        return invalid_wave_format(
            "validate_wave_format",
            "WAVE format average byte rate is zero, inconsistent, or overflows DWORD.");
    }

    return result<void>::success();
}

/// Строит extensible WAVE format на основе запрошенных публичных hints формата.
result<unique_wave_format> make_requested_wave_format(const format_request& request) {
    // Явная negotiation работает только тогда, когда вызывающий код передал все обязательные hints.
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

    if (*request.preferred_sample == sample_type::unknown ||
        *request.preferred_sample_rate == 0U ||
        *request.preferred_channel_count == 0U) {
        error failure;
        failure.category = error_category::configuration;
        failure.code = error_code::invalid_argument;
        failure.operation = "make_requested_wave_format";
        failure.message = "Requested sample type must be known and rate/channels must be non-zero.";
        return result<unique_wave_format>::failure(std::move(failure));
    }

    const std::uint16_t bits_per_sample =
        *request.preferred_sample == sample_type::pcm_i16 ? 16U : 32U;
    const std::uint64_t block_align =
        static_cast<std::uint64_t>(*request.preferred_channel_count) * (bits_per_sample / 8U);
    const std::uint64_t average_bytes_per_second =
        block_align * *request.preferred_sample_rate;
    if (block_align > (std::numeric_limits<WORD>::max)() ||
        average_bytes_per_second > (std::numeric_limits<DWORD>::max)()) {
        error failure;
        failure.category = error_category::configuration;
        failure.code = error_code::invalid_argument;
        failure.operation = "make_requested_wave_format";
        failure.message = "Requested WAVE format overflows block alignment or average byte rate.";
        return result<unique_wave_format>::failure(std::move(failure));
    }

    // Выделяем extensible format, потому что он может представлять float, PCM и valid-bit раскладки.
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
    /// Для mono/stereo известны однозначные стандартные раскладки. Для большего числа
    /// каналов нулевая маска честно оставляет endpoint-у выбор порядка каналов вместо
    /// неверной stereo-маски с несовпадающим popcount.
    extensible->dwChannelMask = *request.preferred_channel_count == 1
        ? SPEAKER_FRONT_CENTER
        : (*request.preferred_channel_count == 2 ? KSAUDIO_SPEAKER_STEREO : 0U);

    // Сопоставляем публичный тип sample-ов с соответствующим нативным subtype.
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
        return result<unique_wave_format>::failure(
            invalid_wave_format("make_requested_wave_format", "Unknown sample type.").error());
    }

    extensible->Format.nBlockAlign = static_cast<WORD>(block_align);
    extensible->Format.nAvgBytesPerSec = static_cast<DWORD>(average_bytes_per_second);

    auto validation_result = validate_wave_format(extensible->Format);
    if (!validation_result) {
        return result<unique_wave_format>::failure(validation_result.error());
    }

    return result<unique_wave_format>::success(std::move(format));
}

/// Освобождает WAVEFORMATEX, выделенный через `CoTaskMemAlloc`.
void cotaskmem_deleter::operator()(WAVEFORMATEX* format) const noexcept {
    if (format != nullptr) {
        CoTaskMemFree(format);
    }
}

/// Преобразует нативный WAVE format в публичную модель audio format Sonotide.
audio_format to_audio_format(const WAVEFORMATEX& format) {
    if (!validate_wave_format(format)) {
        return {};
    }
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

/// Клонирует нативный WAVE format в отдельно владеющее выделение.
result<unique_wave_format> clone_wave_format(const WAVEFORMATEX& source) {
    auto validation_result = validate_wave_format(source);
    if (!validation_result) {
        return result<unique_wave_format>::failure(validation_result.error());
    }

    if (source.cbSize > (std::numeric_limits<std::size_t>::max)() - sizeof(WAVEFORMATEX)) {
        return result<unique_wave_format>::failure(
            invalid_wave_format("clone_wave_format", "WAVE format size overflows size_t.").error());
    }
    const std::size_t total_size = sizeof(WAVEFORMATEX) + source.cbSize;
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

/// Согласовывает shared-mode format и возвращает и нативное, и публичное представления.
result<negotiated_format> negotiate_shared_mode_format(
    IAudioClient& audio_client,
    const format_request& request) {
    // Device mix format используется как запасной вариант, когда вызывающий код не запросил что-то явное.
    WAVEFORMATEX* mix_format_raw = nullptr;
    HRESULT hr = audio_client.GetMixFormat(&mix_format_raw);
    unique_wave_format mix_format(mix_format_raw);
    if (FAILED(hr) || mix_format_raw == nullptr) {
        error failure;
        failure.category = error_category::format;
        failure.code = error_code::format_negotiation_failed;
        failure.message = "Failed to query device mix format.";
        failure.operation = "IAudioClient::GetMixFormat";
        failure.native_code = static_cast<long>(hr);
        return result<negotiated_format>::failure(std::move(failure));
    }

    auto mix_validation_result = validate_wave_format(*mix_format);
    if (!mix_validation_result) {
        return result<negotiated_format>::failure(mix_validation_result.error());
    }
    unique_wave_format selected_format;

    // Если вызывающий код передал явные hints формата, сначала пытаемся выполнить именно их.
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
        unique_wave_format closest_format(closest_raw);

        if (hr == S_OK) {
            selected_format = std::move(requested_result.value());
        } else if (hr == S_FALSE && closest_format && request.allow_mix_format_fallback) {
            selected_format = std::move(closest_format);
        } else if (request.allow_mix_format_fallback) {
            // Переход на device mix format сохраняет работоспособность playback на endpoint-ах,
            // которые не могут точно принять запрошенную раскладку.
            auto cloned_mix_result = clone_wave_format(*mix_format.get());
            if (!cloned_mix_result) {
                return result<negotiated_format>::failure(cloned_mix_result.error());
            }
            selected_format = std::move(cloned_mix_result.value());
        } else {
            error failure;
            failure.category = error_category::format;
            failure.code = error_code::format_negotiation_failed;
            failure.message = "Requested format is not supported by the selected endpoint.";
            failure.operation = "IAudioClient::IsFormatSupported";
            failure.native_code = static_cast<long>(hr);
            return result<negotiated_format>::failure(std::move(failure));
        }
    } else {
        // Без явных hints мы просто переиспользуем device mix format.
        auto cloned_mix_result = clone_wave_format(*mix_format.get());
        if (!cloned_mix_result) {
            return result<negotiated_format>::failure(cloned_mix_result.error());
        }
        selected_format = std::move(cloned_mix_result.value());
    }

    auto selected_validation_result = validate_wave_format(*selected_format);
    if (!selected_validation_result) {
        return result<negotiated_format>::failure(selected_validation_result.error());
    }

    negotiated_format negotiated;
    negotiated.block_align = selected_format->nBlockAlign;
    negotiated.public_format = to_audio_format(*selected_format.get());
    negotiated.wave_format = std::move(selected_format);
    return result<negotiated_format>::success(std::move(negotiated));
}

}  // namespace sonotide::detail::win
