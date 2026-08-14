#include "internal/win/media_foundation_decoder.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfobjects.h>
#include <mfreadwrite.h>
#include <propidl.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

#include "internal/win/hresult_utils.h"
#include "internal/win/media_foundation_decoder_validation.h"

namespace sonotide::detail::win {
namespace {

error decoder_error(
    const error_category category,
    const error_code code,
    const char* operation,
    const char* message) {
    error failure;
    failure.category = category;
    failure.code = code;
    failure.operation = operation;
    failure.message = message;
    return failure;
}

class media_buffer_unlock_guard {
public:
    explicit media_buffer_unlock_guard(IMFMediaBuffer& buffer) noexcept
        : buffer_(&buffer) {}

    media_buffer_unlock_guard(const media_buffer_unlock_guard&) = delete;
    media_buffer_unlock_guard& operator=(const media_buffer_unlock_guard&) = delete;

    ~media_buffer_unlock_guard() {
        if (buffer_ != nullptr) {
            (void)buffer_->Unlock();
        }
    }

    HRESULT unlock() noexcept {
        IMFMediaBuffer* buffer = std::exchange(buffer_, nullptr);
        return buffer != nullptr ? buffer->Unlock() : S_OK;
    }

private:
    IMFMediaBuffer* buffer_;
};

/// Преобразует временные метки Media Foundation в 100ns в миллисекунды для публичных снимков состояния.
std::int64_t to_milliseconds(const std::int64_t value_100ns) {
    return value_100ns > 0 ? value_100ns / 10000 : 0;
}

/// Настраивает source reader на выдачу float PCM с целевой раскладкой каналов.
result<void> configure_output_type(IMFSourceReader& source_reader, const audio_format& output_format) {
    // Media Foundation выполнит декодирование и преобразование за нас, как только тип media будет зафиксирован.
    Microsoft::WRL::ComPtr<IMFMediaType> media_type;
    HRESULT hr = MFCreateMediaType(&media_type);
    if (FAILED(hr)) {
        return result<void>::failure(map_hresult(
            "MFCreateMediaType",
            hr,
            error_category::initialization,
            error_code::initialization_failed));
    }

    hr = media_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    if (FAILED(hr)) {
        return result<void>::failure(map_hresult(
            "IMFMediaType::SetGUID(MF_MT_MAJOR_TYPE)",
            hr,
            error_category::format,
            error_code::format_negotiation_failed));
    }

    hr = media_type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
    if (FAILED(hr)) {
        return result<void>::failure(map_hresult(
            "IMFMediaType::SetGUID(MF_MT_SUBTYPE)",
            hr,
            error_category::format,
            error_code::format_negotiation_failed));
    }

    hr = media_type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, output_format.channel_count);
    if (FAILED(hr)) {
        return result<void>::failure(map_hresult(
            "IMFMediaType::SetUINT32(MF_MT_AUDIO_NUM_CHANNELS)",
            hr,
            error_category::format,
            error_code::format_negotiation_failed));
    }

    hr = media_type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, output_format.sample_rate);
    if (FAILED(hr)) {
        return result<void>::failure(map_hresult(
            "IMFMediaType::SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND)",
            hr,
            error_category::format,
            error_code::format_negotiation_failed));
    }

    hr = media_type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 32);
    if (FAILED(hr)) {
        return result<void>::failure(map_hresult(
            "IMFMediaType::SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE)",
            hr,
            error_category::format,
            error_code::format_negotiation_failed));
    }

    hr = media_type->SetUINT32(
        MF_MT_AUDIO_BLOCK_ALIGNMENT,
        output_format.channel_count * static_cast<std::uint16_t>(sizeof(float)));
    if (FAILED(hr)) {
        return result<void>::failure(map_hresult(
            "IMFMediaType::SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT)",
            hr,
            error_category::format,
            error_code::format_negotiation_failed));
    }

    hr = media_type->SetUINT32(
        MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
        output_format.sample_rate * output_format.channel_count * static_cast<std::uint32_t>(sizeof(float)));
    if (FAILED(hr)) {
        return result<void>::failure(map_hresult(
            "IMFMediaType::SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND)",
            hr,
            error_category::format,
            error_code::format_negotiation_failed));
    }

    hr = media_type->SetUINT32(MF_MT_AUDIO_VALID_BITS_PER_SAMPLE, 32);
    if (FAILED(hr)) {
        return result<void>::failure(map_hresult(
            "IMFMediaType::SetUINT32(MF_MT_AUDIO_VALID_BITS_PER_SAMPLE)",
            hr,
            error_category::format,
            error_code::format_negotiation_failed));
    }

    hr = source_reader.SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), TRUE);
    if (FAILED(hr)) {
        return result<void>::failure(map_hresult(
            "IMFSourceReader::SetStreamSelection",
            hr,
            error_category::initialization,
            error_code::stream_open_failed));
    }

    hr = source_reader.SetCurrentMediaType(
        static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM),
        nullptr,
        media_type.Get());
    if (FAILED(hr)) {
        return result<void>::failure(map_hresult(
            "IMFSourceReader::SetCurrentMediaType",
            hr,
            error_category::format,
            error_code::format_negotiation_failed));
    }

    return result<void>::success();
}

/// Запрашивает длительность source, если media её предоставляет.
std::int64_t query_duration_100ns(IMFSourceReader& source_reader) {
    // Длительность необязательна, поэтому ошибки превращаются в "unknown duration", а не в жёсткий сбой.
    PROPVARIANT duration;
    PropVariantInit(&duration);
    const HRESULT hr = source_reader.GetPresentationAttribute(
        static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE),
        MF_PD_DURATION,
        &duration);
    if (FAILED(hr)) {
        PropVariantClear(&duration);
        return 0;
    }

    std::int64_t value_100ns = 0;
    if (duration.vt == VT_UI8) {
        if (duration.uhVal.QuadPart <=
            static_cast<ULONGLONG>((std::numeric_limits<std::int64_t>::max)())) {
            value_100ns = static_cast<std::int64_t>(duration.uhVal.QuadPart);
        }
    } else if (duration.vt == VT_I8) {
        value_100ns = (std::max)(duration.hVal.QuadPart, static_cast<LONGLONG>(0));
    }
    PropVariantClear(&duration);
    return value_100ns;
}

}  // namespace

/// Открывает новый Media Foundation source reader для переданного URI.
result<void> media_foundation_decoder::open(
    const std::string& source_uri,
    const audio_format& output_format,
    const std::uint64_t operation_epoch) {
    // Повторное открытие всегда начинается с чистого состояния, чтобы устаревшее состояние не протекало между треками.
    close();

    if (source_uri.empty()) {
        return result<void>::failure(decoder_error(
            error_category::configuration,
            error_code::invalid_argument,
            "media_foundation_decoder::open",
            "Source URI must not be empty."));
    }
    if (!is_valid_decoder_output_format(output_format)) {
        return result<void>::failure(decoder_error(
            error_category::format,
            error_code::invalid_argument,
            "media_foundation_decoder::open",
            "Output format must be a supported interleaved PCM layout with non-zero, overflow-safe rate and channels."));
    }

    if (cancellation_.changed_since(operation_epoch)) {
        return result<void>::failure(decoder_error(
            error_category::stream,
            error_code::invalid_state,
            "media_foundation_decoder::open",
            "Decode operation was cancelled while opening the source."));
    }
    // Атрибуты низкой задержки сохраняют отзывчивость воспроизведения, когда буфер render небольшой.
    Microsoft::WRL::ComPtr<IMFAttributes> attributes;
    HRESULT hr = MFCreateAttributes(&attributes, 2);
    if (FAILED(hr)) {
        return result<void>::failure(map_hresult(
            "MFCreateAttributes",
            hr,
            error_category::initialization,
            error_code::initialization_failed));
    }

    hr = attributes->SetUINT32(MF_LOW_LATENCY, TRUE);
    if (FAILED(hr)) {
        return result<void>::failure(map_hresult(
            "IMFAttributes::SetUINT32(MF_LOW_LATENCY)",
            hr,
            error_category::initialization,
            error_code::initialization_failed));
    }

    hr = attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, FALSE);
    if (FAILED(hr)) {
        return result<void>::failure(map_hresult(
            "IMFAttributes::SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS)",
            hr,
            error_category::initialization,
            error_code::initialization_failed));
    }

    // Media Foundation ожидает путь или URL в UTF-16.
    const std::wstring wide_source_uri = utf16_from_utf8(source_uri);
    if (wide_source_uri.empty()) {
        return result<void>::failure(decoder_error(
            error_category::configuration,
            error_code::invalid_argument,
            "media_foundation_decoder::open",
            "Source URI is not valid UTF-8 or is too long for the Windows API."));
    }
    Microsoft::WRL::ComPtr<IMFSourceReader> new_source_reader;
    hr = MFCreateSourceReaderFromURL(
        wide_source_uri.c_str(),
        attributes.Get(),
        &new_source_reader);
    if (FAILED(hr)) {
        if (cancellation_.changed_since(operation_epoch)) {
            return result<void>::failure(decoder_error(
                error_category::stream,
                error_code::invalid_state,
                "media_foundation_decoder::open",
                "Decode operation was cancelled while creating the source reader."));
        }
        return result<void>::failure(map_hresult(
            "MFCreateSourceReaderFromURL",
            hr,
            error_category::initialization,
            error_code::stream_open_failed));
    }

    auto configure_result = configure_output_type(*new_source_reader.Get(), output_format);
    if (!configure_result) {
        if (cancellation_.changed_since(operation_epoch)) {
            return result<void>::failure(decoder_error(
                error_category::stream,
                error_code::invalid_state,
                "media_foundation_decoder::open",
                "Decode operation was cancelled while configuring the source reader."));
        }
        return configure_result;
    }

    const std::int64_t source_duration_100ns =
        query_duration_100ns(*new_source_reader.Get());
    {
        std::scoped_lock lock(source_reader_mutex_);
        if (cancellation_.changed_since(operation_epoch)) {
            return result<void>::failure(decoder_error(
                error_category::stream,
                error_code::invalid_state,
                "media_foundation_decoder::open",
                "Decode operation was cancelled while opening the source."));
        }
        source_reader_ = std::move(new_source_reader);
    }

    output_format_ = output_format;
    decoded_samples_.clear();
    decoded_sample_offset_ = 0;
    next_sample_time_100ns_ = 0;
    duration_100ns_ = source_duration_100ns;
    end_of_stream_ = false;
    return result<void>::success();
}

/// Выполняет seek в source reader и отбрасывает все буферизованные decoded sample-ы.
result<void> media_foundation_decoder::seek_to(
    const std::int64_t position_ms,
    const std::uint64_t operation_epoch) {
    if (cancellation_.changed_since(operation_epoch)) {
        return result<void>::failure(decoder_error(
            error_category::stream,
            error_code::invalid_state,
            "media_foundation_decoder::seek_to",
            "Decode command was cancelled before seeking started."));
    }
    Microsoft::WRL::ComPtr<IMFSourceReader> source_reader;
    {
        std::scoped_lock lock(source_reader_mutex_);
        source_reader = source_reader_;
    }
    if (!source_reader) {
        error failure;
        failure.category = error_category::stream;
        failure.code = error_code::invalid_state;
        failure.operation = "media_foundation_decoder::seek_to";
        failure.message = "Cannot seek before a source has been opened.";
        return result<void>::failure(std::move(failure));
    }

    const auto position_100ns = milliseconds_to_100ns(position_ms);
    if (!position_100ns) {
        return result<void>::failure(decoder_error(
            error_category::configuration,
            error_code::invalid_argument,
            "media_foundation_decoder::seek_to",
            "Seek position must be non-negative and fit in a Media Foundation timestamp."));
    }

    PROPVARIANT position;
    PropVariantInit(&position);
    position.vt = VT_I8;
    position.hVal.QuadPart = *position_100ns;
    const HRESULT hr = source_reader->SetCurrentPosition(GUID_NULL, position);
    PropVariantClear(&position);
    if (FAILED(hr)) {
        if (cancellation_.changed_since(operation_epoch) || hr == MF_E_NOTACCEPTING) {
            return result<void>::failure(decoder_error(
                error_category::stream,
                error_code::invalid_state,
                "media_foundation_decoder::seek_to",
                "Decode operation was cancelled while seeking."));
        }
        return result<void>::failure(map_hresult(
            "IMFSourceReader::SetCurrentPosition",
            hr,
            error_category::stream,
            error_code::stream_open_failed));
    }
    if (cancellation_.changed_since(operation_epoch)) {
        return result<void>::failure(decoder_error(
            error_category::stream,
            error_code::invalid_state,
            "media_foundation_decoder::seek_to",
            "Decode operation was cancelled while seeking."));
    }

    decoded_samples_.clear();
    decoded_sample_offset_ = 0;
    next_sample_time_100ns_ = *position_100ns;
    end_of_stream_ = false;
    return result<void>::success();
}

/// Возвращает decoded PCM block с точным числом frame-ов, запрошенным render path.
result<decoded_audio_block> media_foundation_decoder::read_frames(
    const std::uint32_t frame_count,
    const std::uint64_t operation_epoch) {
    if (cancellation_.changed_since(operation_epoch)) {
        return result<decoded_audio_block>::failure(decoder_error(
            error_category::stream,
            error_code::invalid_state,
            "media_foundation_decoder::read_frames",
            "Decode command was cancelled before reading started."));
    }
    const auto requested_samples = checked_sample_count(
        frame_count,
        output_format_.channel_count,
        decoded_samples_.max_size());
    if (!requested_samples) {
        return result<decoded_audio_block>::failure(decoder_error(
            error_category::configuration,
            error_code::invalid_argument,
            "media_foundation_decoder::read_frames",
            "Requested frame and channel count exceeds the decoder sample capacity."));
    }
    // Декодируем достаточно sample-ов, чтобы удовлетворить запрос, прежде чем собирать выходной блок.
    auto ensure_result = ensure_decoded_frames(frame_count, operation_epoch);
    if (!ensure_result) {
        return result<decoded_audio_block>::failure(ensure_result.error());
    }
    if (cancellation_.changed_since(operation_epoch)) {
        return result<decoded_audio_block>::failure(decoder_error(
            error_category::stream,
            error_code::invalid_state,
            "media_foundation_decoder::read_frames",
            "Decode operation was cancelled."));
    }
    const auto next_timestamp = advance_timestamp_100ns(
        next_sample_time_100ns_,
        frame_count,
        output_format_.sample_rate);
    if (!next_timestamp) {
        return result<decoded_audio_block>::failure(decoder_error(
            error_category::stream,
            error_code::invalid_state,
            "media_foundation_decoder::read_frames",
            "Decoded stream timestamp would overflow."));
    }

    decoded_audio_block block;
    try {
        block.samples.assign(*requested_samples, 0.0F);
    } catch (const std::bad_alloc&) {
        return result<decoded_audio_block>::failure(decoder_error(
            error_category::platform,
            error_code::platform_failure,
            "media_foundation_decoder::read_frames",
            "Failed to allocate the decoded output block."));
    } catch (const std::length_error&) {
        return result<decoded_audio_block>::failure(decoder_error(
            error_category::configuration,
            error_code::invalid_argument,
            "media_foundation_decoder::read_frames",
            "Decoded output block exceeds vector limits."));
    }

    const std::size_t available_samples = decoded_samples_.size() - decoded_sample_offset_;
    const std::size_t copied_samples = (std::min)(available_samples, *requested_samples);
    if (copied_samples > 0) {
        std::copy_n(
            decoded_samples_.data() + decoded_sample_offset_,
            copied_samples,
            block.samples.data());
        decoded_sample_offset_ += copied_samples;
    }

    if (decoded_sample_offset_ >= decoded_samples_.size()) {
        decoded_samples_.clear();
        decoded_sample_offset_ = 0;
    }

    next_sample_time_100ns_ = *next_timestamp;

    block.position_ms = to_milliseconds(next_sample_time_100ns_);
    block.duration_ms = to_milliseconds(duration_100ns_);
    block.end_of_stream = end_of_stream_ && decoded_samples_.empty();
    return result<decoded_audio_block>::success(std::move(block));
}

/// Освобождает source reader и очищает всё кешированное decode-состояние.
void media_foundation_decoder::close() noexcept {
    try {
        Microsoft::WRL::ComPtr<IMFSourceReader> source_reader_to_release;
        {
            std::scoped_lock lock(source_reader_mutex_);
            source_reader_.Swap(source_reader_to_release);
        }
        source_reader_to_release.Reset();
        decoded_samples_.clear();
        decoded_sample_offset_ = 0;
        next_sample_time_100ns_ = 0;
        duration_100ns_ = 0;
        end_of_stream_ = false;
        output_format_ = {};
    } catch (...) {
        // This method is the worker-apartment cleanup boundary. Cleanup must
        // never terminate a noexcept playback-session shutdown.
    }
}

std::uint64_t media_foundation_decoder::request_cancel() noexcept {
    // Flush itself is a synchronous Media Foundation call and is permitted to
    // block in a third-party byte-stream handler. Public load/seek/close paths
    // therefore perform only epoch invalidation; the worker checks this epoch
    // before publishing any data or state.
    return cancellation_.request();
}

/// Сообщает, активен ли сейчас source reader.
bool media_foundation_decoder::is_open() const noexcept {
    try {
        std::scoped_lock lock(source_reader_mutex_);
        return static_cast<bool>(source_reader_);
    } catch (...) {
        return false;
    }
}

/// Возвращает negotiated output format для текущего source.
const audio_format& media_foundation_decoder::output_format() const noexcept {
    return output_format_;
}

/// Возвращает текущую длительность source в миллисекундах.
std::int64_t media_foundation_decoder::duration_ms() const noexcept {
    return to_milliseconds(duration_100ns_);
}

/// Подтягивает из Media Foundation достаточно decoded sample-ов, чтобы удовлетворить запрос.
result<void> media_foundation_decoder::ensure_decoded_frames(
    const std::uint32_t frame_count,
    const std::uint64_t operation_epoch) {
    // Если source reader исчез, decode больше невозможен.
    Microsoft::WRL::ComPtr<IMFSourceReader> source_reader;
    {
        std::scoped_lock lock(source_reader_mutex_);
        source_reader = source_reader_;
    }
    if (!source_reader) {
        error failure;
        failure.category = error_category::stream;
        failure.code = error_code::invalid_state;
        failure.operation = "media_foundation_decoder::ensure_decoded_frames";
        failure.message = "Cannot decode before a source has been opened.";
        return result<void>::failure(std::move(failure));
    }

    if (decoded_sample_offset_ > decoded_samples_.size()) {
        return result<void>::failure(decoder_error(
            error_category::stream,
            error_code::invalid_state,
            "media_foundation_decoder::ensure_decoded_frames",
            "Decoder sample offset is outside the decoded buffer."));
    }
    const auto required_samples = checked_sample_count(
        frame_count,
        output_format_.channel_count,
        decoded_samples_.max_size());
    if (!required_samples) {
        return result<void>::failure(decoder_error(
            error_category::configuration,
            error_code::invalid_argument,
            "media_foundation_decoder::ensure_decoded_frames",
            "Requested frame and channel count exceeds the decoder sample capacity."));
    }
    // Продолжаем чтение, пока в буфере не хватит sample-ов или source не сообщит EOS.
    while ((decoded_samples_.size() - decoded_sample_offset_) < *required_samples &&
           !end_of_stream_) {
        if (cancellation_.changed_since(operation_epoch)) {
            error failure;
            failure.category = error_category::stream;
            failure.code = error_code::invalid_state;
            failure.operation = "media_foundation_decoder::read_frames";
            failure.message = "Decode operation was cancelled.";
            return result<void>::failure(std::move(failure));
        }
        // Каждая итерация забирает следующий sample из Media Foundation и добавляет его в PCM cache.
        DWORD stream_index = 0;
        DWORD stream_flags = 0;
        LONGLONG sample_time = 0;
        Microsoft::WRL::ComPtr<IMFSample> sample;
        const HRESULT hr = source_reader->ReadSample(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM),
            0,
            &stream_index,
            &stream_flags,
            &sample_time,
            &sample);
        if (FAILED(hr)) {
            if (cancellation_.changed_since(operation_epoch) ||
                hr == MF_E_NOTACCEPTING) {
                return result<void>::failure(decoder_error(
                    error_category::stream,
                    error_code::invalid_state,
                    "media_foundation_decoder::read_frames",
                    "Decode operation was cancelled."));
            }
            return result<void>::failure(map_hresult(
                "IMFSourceReader::ReadSample",
                hr,
                error_category::stream,
                error_code::stream_open_failed));
        }
        if (cancellation_.changed_since(operation_epoch)) {
            return result<void>::failure(decoder_error(
                error_category::stream,
                error_code::invalid_state,
                "media_foundation_decoder::read_frames",
                "Decode operation was cancelled."));
        }

        if ((stream_flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0U) {
            end_of_stream_ = true;
            break;
        }

        if (!sample) {
            continue;
        }

        if (decoded_samples_.empty()) {
            next_sample_time_100ns_ = (std::max)(sample_time, static_cast<LONGLONG>(0));
        }

        Microsoft::WRL::ComPtr<IMFMediaBuffer> media_buffer;
        const HRESULT contiguous_hr = sample->ConvertToContiguousBuffer(&media_buffer);
        if (FAILED(contiguous_hr)) {
            return result<void>::failure(map_hresult(
                "IMFSample::ConvertToContiguousBuffer",
                contiguous_hr,
                error_category::stream,
                error_code::stream_open_failed));
        }
        if (!media_buffer) {
            return result<void>::failure(decoder_error(
                error_category::stream,
                error_code::stream_open_failed,
                "IMFSample::ConvertToContiguousBuffer",
                "Media Foundation returned a null contiguous buffer."));
        }

        BYTE* bytes = nullptr;
        DWORD max_length = 0;
        DWORD current_length = 0;
        const HRESULT lock_hr = media_buffer->Lock(&bytes, &max_length, &current_length);
        if (FAILED(lock_hr)) {
            return result<void>::failure(map_hresult(
                "IMFMediaBuffer::Lock",
                lock_hr,
                error_category::stream,
                error_code::stream_open_failed));
        }
        media_buffer_unlock_guard unlock_guard(*media_buffer.Get());

        if (current_length > max_length || current_length % sizeof(float) != 0U ||
            (current_length > 0U && bytes == nullptr)) {
            return result<void>::failure(decoder_error(
                error_category::stream,
                error_code::stream_open_failed,
                "IMFMediaBuffer::Lock",
                "Media Foundation returned an invalid float PCM buffer."));
        }

        const std::size_t float_count = current_length / sizeof(float);
        if (float_count % output_format_.channel_count != 0U ||
            float_count > decoded_samples_.max_size() - decoded_samples_.size()) {
            return result<void>::failure(decoder_error(
                error_category::stream,
                error_code::stream_open_failed,
                "media_foundation_decoder::ensure_decoded_frames",
                "Decoded PCM buffer is not frame-aligned or exceeds decoder capacity."));
        }

        const std::size_t previous_sample_count = decoded_samples_.size();
        try {
            if (float_count > 0U) {
                decoded_samples_.resize(previous_sample_count + float_count);
                std::memcpy(
                    decoded_samples_.data() + previous_sample_count,
                    bytes,
                    current_length);
            }
        } catch (const std::bad_alloc&) {
            return result<void>::failure(decoder_error(
                error_category::platform,
                error_code::platform_failure,
                "media_foundation_decoder::ensure_decoded_frames",
                "Failed to grow the decoded PCM buffer."));
        } catch (const std::length_error&) {
            return result<void>::failure(decoder_error(
                error_category::stream,
                error_code::stream_open_failed,
                "media_foundation_decoder::ensure_decoded_frames",
                "Decoded PCM buffer exceeds vector limits."));
        }

        const HRESULT unlock_hr = unlock_guard.unlock();
        if (FAILED(unlock_hr)) {
            decoded_samples_.resize(previous_sample_count);
            return result<void>::failure(map_hresult(
                "IMFMediaBuffer::Unlock",
                unlock_hr,
                error_category::stream,
                error_code::stream_open_failed));
        }
    }

    return result<void>::success();
}

}  // namespace sonotide::detail::win
