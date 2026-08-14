#pragma once

#include <mfidl.h>
#include <mfobjects.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "sonotide/audio_format.h"
#include "sonotide/result.h"
#include "internal/win/media_foundation_decoder_validation.h"

namespace sonotide::detail::win {

/// Результат декодирования очередного блока PCM из Media Foundation.
struct decoded_audio_block {
    /// Интерливированный float PCM, готовый к отдаче в render path.
    std::vector<float> samples;
    /// Текущая позиция декодера в миллисекундах после выдачи блока.
    std::int64_t position_ms = 0;
    /// Общая длительность source, если её удалось определить.
    std::int64_t duration_ms = 0;
    /// Признак того, что декодер дошёл до конца источника.
    bool end_of_stream = false;
};

/// Декодер Media Foundation только для Windows, настроенный на float PCM output.
class media_foundation_decoder {
public:
    /// Создаёт пустой декодер без открытого источника.
    media_foundation_decoder() = default;

    /// Открывает источник и конфигурирует output format под render path.
    [[nodiscard]] result<void> open(
        const std::string& source_uri,
        const audio_format& output_format,
        std::uint64_t command_epoch);
    /// Выполняет seek в текущем source reader.
    [[nodiscard]] result<void> seek_to(std::int64_t position_ms, std::uint64_t command_epoch);
    /// Возвращает очередной блок PCM, декодируя дополнительные sample-ы по необходимости.
    [[nodiscard]] result<decoded_audio_block> read_frames(
        std::uint32_t frame_count,
        std::uint64_t command_epoch);

    /// Полностью освобождает source reader и промежуточные буферы.
    void close();
    /// Прерывает потенциально блокирующее чтение при shutdown decode worker.
    [[nodiscard]] std::uint64_t request_cancel() noexcept;

    /// Сообщает, открыт ли сейчас source reader.
    [[nodiscard]] bool is_open() const noexcept;
    /// Возвращает negotiated output format, в котором работает декодер.
    [[nodiscard]] const audio_format& output_format() const noexcept;
    /// Возвращает длительность source в миллисекундах, если она известна.
    [[nodiscard]] std::int64_t duration_ms() const noexcept;

private:
    /// Гарантирует наличие достаточного числа decoded sample-ов в буфере.
    [[nodiscard]] result<void> ensure_decoded_frames(
        std::uint32_t frame_count,
        std::uint64_t cancellation_epoch);

    /// Source reader Media Foundation, которым владеет декодер.
    /// Любое копирование/публикация/reset выполняются под `source_reader_mutex_`:
    /// локальный ComPtr удерживает COM-объект живым во время ReadSample/Flush.
    mutable std::mutex source_reader_mutex_;
    Microsoft::WRL::ComPtr<IMFSourceReader> source_reader_;
    decoder_cancellation_epoch cancellation_;
    /// Выходной формат, под который настраивался `source_reader_`.
    audio_format output_format_{};
    /// Буфер уже прочитанных float sample-ов.
    std::vector<float> decoded_samples_;
    /// Смещение чтения внутри `decoded_samples_`.
    std::size_t decoded_sample_offset_ = 0;
    /// Следующий time stamp sample-а в 100ns ticks.
    std::int64_t next_sample_time_100ns_ = 0;
    /// Общая длительность source в 100ns ticks.
    std::int64_t duration_100ns_ = 0;
    /// Флаг конца source, полученный из `MF_SOURCE_READERF_ENDOFSTREAM`.
    bool end_of_stream_ = false;
};

}  // namespace sonotide::detail::win
