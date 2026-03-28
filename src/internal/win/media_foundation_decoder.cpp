#include "internal/win/media_foundation_decoder.h"

#include <mfapi.h>
#include <mfobjects.h>
#include <mfreadwrite.h>
#include <propidl.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <utility>

#include "internal/win/hresult_utils.h"

namespace sonotide::detail::win {
namespace {

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
        value_100ns = static_cast<std::int64_t>(duration.uhVal.QuadPart);
    } else if (duration.vt == VT_I8) {
        value_100ns = duration.hVal.QuadPart;
    }
    PropVariantClear(&duration);
    return value_100ns;
}

}  // namespace

/// Открывает новый Media Foundation source reader для переданного URI.
result<void> media_foundation_decoder::open(const std::string& source_uri, const audio_format& output_format) {
    // Повторное открытие всегда начинается с чистого состояния, чтобы устаревшее состояние не протекало между треками.
    close();

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
    hr = MFCreateSourceReaderFromURL(wide_source_uri.c_str(), attributes.Get(), &source_reader_);
    if (FAILED(hr)) {
        return result<void>::failure(map_hresult(
            "MFCreateSourceReaderFromURL",
            hr,
            error_category::initialization,
            error_code::stream_open_failed));
    }

    auto configure_result = configure_output_type(*source_reader_.Get(), output_format);
    if (!configure_result) {
        close();
        return configure_result;
    }

    output_format_ = output_format;
    decoded_samples_.clear();
    decoded_sample_offset_ = 0;
    next_sample_time_100ns_ = 0;
    duration_100ns_ = query_duration_100ns(*source_reader_.Get());
    end_of_stream_ = false;
    return result<void>::success();
}

/// Выполняет seek в source reader и отбрасывает все буферизованные decoded sample-ы.
result<void> media_foundation_decoder::seek_to(const std::int64_t position_ms) {
    if (!source_reader_) {
        error failure;
        failure.category = error_category::stream;
        failure.code = error_code::invalid_state;
        failure.operation = "media_foundation_decoder::seek_to";
        failure.message = "Cannot seek before a source has been opened.";
        return result<void>::failure(std::move(failure));
    }

    PROPVARIANT position;
    PropVariantInit(&position);
    position.vt = VT_I8;
    position.hVal.QuadPart = position_ms * 10000;
    const HRESULT hr = source_reader_->SetCurrentPosition(GUID_NULL, position);
    PropVariantClear(&position);
    if (FAILED(hr)) {
        return result<void>::failure(map_hresult(
            "IMFSourceReader::SetCurrentPosition",
            hr,
            error_category::stream,
            error_code::stream_open_failed));
    }

    decoded_samples_.clear();
    decoded_sample_offset_ = 0;
    next_sample_time_100ns_ = position_ms * 10000;
    end_of_stream_ = false;
    return result<void>::success();
}

/// Возвращает decoded PCM block с точным числом frame-ов, запрошенным render path.
result<decoded_audio_block> media_foundation_decoder::read_frames(const std::uint32_t frame_count) {
    // Декодируем достаточно sample-ов, чтобы удовлетворить запрос, прежде чем собирать выходной блок.
    auto ensure_result = ensure_decoded_frames(frame_count);
    if (!ensure_result) {
        return result<decoded_audio_block>::failure(ensure_result.error());
    }

    decoded_audio_block block;
    block.samples.assign(
        static_cast<std::size_t>(frame_count) * output_format_.channel_count,
        0.0F);

    const std::size_t available_samples = decoded_samples_.size() - decoded_sample_offset_;
    const std::size_t requested_samples =
        static_cast<std::size_t>(frame_count) * output_format_.channel_count;
    const std::size_t copied_samples = (std::min)(available_samples, requested_samples);
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

    next_sample_time_100ns_ +=
        static_cast<std::int64_t>(frame_count) * 10000000LL /
        static_cast<std::int64_t>(output_format_.sample_rate);

    block.position_ms = to_milliseconds(next_sample_time_100ns_);
    block.duration_ms = to_milliseconds(duration_100ns_);
    block.end_of_stream = end_of_stream_ && decoded_samples_.empty();
    return result<decoded_audio_block>::success(std::move(block));
}

/// Освобождает source reader и очищает всё кешированное decode-состояние.
void media_foundation_decoder::close() {
    source_reader_.Reset();
    decoded_samples_.clear();
    decoded_sample_offset_ = 0;
    next_sample_time_100ns_ = 0;
    duration_100ns_ = 0;
    end_of_stream_ = false;
    output_format_ = {};
}

/// Сообщает, активен ли сейчас source reader.
bool media_foundation_decoder::is_open() const noexcept {
    return static_cast<bool>(source_reader_);
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
result<void> media_foundation_decoder::ensure_decoded_frames(const std::uint32_t frame_count) {
    // Если source reader исчез, decode больше невозможен.
    if (!source_reader_) {
        error failure;
        failure.category = error_category::stream;
        failure.code = error_code::invalid_state;
        failure.operation = "media_foundation_decoder::ensure_decoded_frames";
        failure.message = "Cannot decode before a source has been opened.";
        return result<void>::failure(std::move(failure));
    }

    const std::size_t required_samples =
        static_cast<std::size_t>(frame_count) * output_format_.channel_count;
    // Продолжаем чтение, пока в буфере не хватит sample-ов или source не сообщит EOS.
    while ((decoded_samples_.size() - decoded_sample_offset_) < required_samples &&
           !end_of_stream_) {
        // Каждая итерация забирает следующий sample из Media Foundation и добавляет его в PCM cache.
        DWORD stream_index = 0;
        DWORD stream_flags = 0;
        LONGLONG sample_time = 0;
        Microsoft::WRL::ComPtr<IMFSample> sample;
        const HRESULT hr = source_reader_->ReadSample(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM),
            0,
            &stream_index,
            &stream_flags,
            &sample_time,
            &sample);
        if (FAILED(hr)) {
            return result<void>::failure(map_hresult(
                "IMFSourceReader::ReadSample",
                hr,
                error_category::stream,
                error_code::stream_open_failed));
        }

        if ((stream_flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0U) {
            end_of_stream_ = true;
            break;
        }

        if (!sample) {
            continue;
        }

        if (decoded_samples_.empty()) {
            next_sample_time_100ns_ = sample_time;
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

        const std::size_t float_count = current_length / sizeof(float);
        const auto* samples = reinterpret_cast<const float*>(bytes);
        decoded_samples_.insert(decoded_samples_.end(), samples, samples + float_count);
        media_buffer->Unlock();
    }

    return result<void>::success();
}

}  // namespace sonotide::detail::win
