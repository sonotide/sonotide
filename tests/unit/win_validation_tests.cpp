#include <Audioclient.h>

#include <chrono>
#include <cstdint>
#include <limits>
#include <string_view>

#include "internal/win/hresult_utils.h"
#include "internal/win/media_foundation_decoder_validation.h"
#include "internal/win/wasapi_stream_validation.h"
#include "internal/win/wave_format_utils.h"
#include "test_support/test_harness.h"

int main() {
    using namespace sonotide;
    using namespace sonotide::detail::win;

    decoder_cancellation_epoch cancellation;
    const std::uint64_t first_operation = cancellation.snapshot();
    REQUIRE(!cancellation.changed_since(first_operation));
    const std::uint64_t first_command_epoch = cancellation.request();
    REQUIRE(first_command_epoch != first_operation);
    REQUIRE(cancellation.changed_since(first_operation));

    // Отмена предыдущей операции не должна автоматически отменять новую:
    // новая операция фиксирует уже увеличенную эпоху как свою начальную точку.
    const std::uint64_t second_operation = cancellation.snapshot();
    REQUIRE(!cancellation.changed_since(second_operation));
    const std::uint64_t second_command_epoch = cancellation.request();
    REQUIRE(second_command_epoch != second_operation);
    REQUIRE(cancellation.changed_since(second_operation));

    audio_format decoder_format{
        .sample = sample_type::float32,
        .sample_rate = 48000U,
        .channel_count = 2U,
        .bits_per_sample = 32U,
        .valid_bits_per_sample = 32U,
        .channel_mask = 0U,
        .interleaved = true,
    };
    REQUIRE(is_valid_decoder_output_format(decoder_format));
    decoder_format.sample = sample_type::unknown;
    REQUIRE(!is_valid_decoder_output_format(decoder_format));
    decoder_format.sample = sample_type::float32;
    decoder_format.channel_count = 0U;
    REQUIRE(!is_valid_decoder_output_format(decoder_format));
    decoder_format.channel_count = 2U;
    decoder_format.interleaved = false;
    REQUIRE(!is_valid_decoder_output_format(decoder_format));
    decoder_format.interleaved = true;
    decoder_format.sample_rate = (std::numeric_limits<std::uint32_t>::max)();
    decoder_format.channel_count = (std::numeric_limits<std::uint16_t>::max)();
    REQUIRE(!is_valid_decoder_output_format(decoder_format));

    REQUIRE(checked_sample_count(128U, 2U).value() == 256U);
    REQUIRE(!checked_sample_count(3U, 2U, 5U));
    REQUIRE(milliseconds_to_100ns(250).value() == 2500000);
    REQUIRE(!milliseconds_to_100ns(-1));
    REQUIRE(!milliseconds_to_100ns((std::numeric_limits<std::int64_t>::max)()));
    REQUIRE(advance_timestamp_100ns(0, 480U, 48000U).value() == 100000);
    REQUIRE(!advance_timestamp_100ns(0, 1U, 0U));
    REQUIRE(!advance_timestamp_100ns(
        (std::numeric_limits<std::int64_t>::max)(),
        1U,
        48000U));

    REQUIRE(milliseconds_to_reference_time(std::chrono::milliseconds(1)).value() == 10000);
    REQUIRE(!milliseconds_to_reference_time(std::chrono::milliseconds(0)));
    REQUIRE(!milliseconds_to_reference_time(std::chrono::milliseconds(-1)));
    constexpr auto maximum_latency_ms =
        (std::numeric_limits<std::int64_t>::max)() / reference_ticks_per_millisecond;
    REQUIRE(milliseconds_to_reference_time(std::chrono::milliseconds(maximum_latency_ms)));
    REQUIRE(!milliseconds_to_reference_time(std::chrono::milliseconds(maximum_latency_ms + 1)));

    REQUIRE(scale_qpc_to_100ns(0, 1).value() == 0U);
    REQUIRE(scale_qpc_to_100ns(15, 10).value() == 15'000'000U);
    REQUIRE(scale_qpc_to_100ns((std::numeric_limits<std::int64_t>::max)(),
                               (std::numeric_limits<std::int64_t>::max)()).value() ==
            qpc_ticks_per_second);
    REQUIRE(!scale_qpc_to_100ns(-1, 1));
    REQUIRE(!scale_qpc_to_100ns(1, 0));

    REQUIRE(checked_audio_byte_count(1024U, 8U).value() == 8192U);
    REQUIRE(!checked_audio_byte_count(1U, 0U));
    REQUIRE(!checked_audio_byte_count(
        (std::numeric_limits<std::uint64_t>::max)(), 2U));
    REQUIRE(is_valid_callback_mode(callback_mode::event_driven));
    REQUIRE(!is_valid_callback_mode(static_cast<callback_mode>(255)));
    REQUIRE(is_known_share_mode(share_mode::shared));
    REQUIRE(is_known_share_mode(share_mode::exclusive));
    REQUIRE(!is_known_share_mode(static_cast<share_mode>(255)));

    WAVEFORMATEX pcm_format{};
    pcm_format.wFormatTag = WAVE_FORMAT_PCM;
    pcm_format.nChannels = 2U;
    pcm_format.nSamplesPerSec = 48000U;
    pcm_format.wBitsPerSample = 16U;
    pcm_format.nBlockAlign = 4U;
    pcm_format.nAvgBytesPerSec = 192000U;
    REQUIRE(validate_wave_format(pcm_format));
    REQUIRE(clone_wave_format(pcm_format));

    WAVEFORMATEX invalid_format = pcm_format;
    invalid_format.wFormatTag = 0x7777U;
    REQUIRE(!validate_wave_format(invalid_format));
    REQUIRE(!clone_wave_format(invalid_format));
    invalid_format = pcm_format;
    invalid_format.nChannels = 0U;
    REQUIRE(!validate_wave_format(invalid_format));
    invalid_format = pcm_format;
    invalid_format.nBlockAlign = 0U;
    REQUIRE(!validate_wave_format(invalid_format));
    invalid_format = pcm_format;
    invalid_format.nAvgBytesPerSec = 1U;
    REQUIRE(!validate_wave_format(invalid_format));
    invalid_format = pcm_format;
    invalid_format.cbSize = (std::numeric_limits<WORD>::max)();
    REQUIRE(!validate_wave_format(invalid_format));
    REQUIRE(!clone_wave_format(invalid_format));

    format_request request;
    request.preferred_sample = sample_type::float32;
    request.preferred_sample_rate = 48000U;
    request.preferred_channel_count = 2U;
    auto requested_format = make_requested_wave_format(request);
    REQUIRE(requested_format);
    REQUIRE(validate_wave_format(*requested_format.value()));

    format_request surround_request = request;
    surround_request.preferred_channel_count = 6U;
    auto surround_format = make_requested_wave_format(surround_request);
    REQUIRE(surround_format);
    const auto* surround_extensible =
        reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(surround_format.value().get());
    REQUIRE(surround_extensible->dwChannelMask == 0U);

    auto invalid_mask_format = *surround_extensible;
    invalid_mask_format.dwChannelMask = KSAUDIO_SPEAKER_STEREO;
    REQUIRE(!validate_wave_format(invalid_mask_format.Format));

    request.preferred_sample = sample_type::unknown;
    REQUIRE(!make_requested_wave_format(request));
    request.preferred_sample = sample_type::float32;
    request.preferred_sample_rate = 0U;
    REQUIRE(!make_requested_wave_format(request));
    request.preferred_sample_rate = (std::numeric_limits<std::uint32_t>::max)();
    request.preferred_channel_count = (std::numeric_limits<std::uint16_t>::max)();
    REQUIRE(!make_requested_wave_format(request));

    REQUIRE(utf16_from_utf8("Sonotide") == L"Sonotide");
    REQUIRE(utf8_from_utf16(L"Sonotide") == "Sonotide");
    const std::string_view oversized_utf8(
        "x",
        static_cast<std::size_t>((std::numeric_limits<int>::max)()) + 1U);
    const std::wstring_view oversized_utf16(
        L"x",
        static_cast<std::size_t>((std::numeric_limits<int>::max)()) + 1U);
    REQUIRE(utf16_from_utf8(oversized_utf8).empty());
    REQUIRE(utf8_from_utf16(oversized_utf16).empty());

    return 0;
}
