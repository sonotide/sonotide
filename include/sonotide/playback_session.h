#pragma once

#include <cstdint>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "sonotide/device_info.h"
#include "sonotide/equalizer.h"
#include "sonotide/playback_state.h"
#include "sonotide/render_stream.h"
#include "sonotide/result.h"
#include "sonotide/stream_config.h"

namespace sonotide {
namespace detail {
class runtime_backend;
}

/// Конфигурация сессии воспроизведения, построенной поверх render stream.
struct playback_session_config {
    /// Настройки render stream, используемые сессией.
    render_stream_config render;
    /// Должно ли загрузка источника автоматически запускать воспроизведение.
    bool auto_play_on_load = true;
    /// Начальная громкость в процентах, применяемая до старта воспроизведения.
    int initial_volume_percent = 100;
    /// Необязательный начальный снимок эквалайзера.
    std::optional<equalizer_state> initial_equalizer_state;
};

    /// Высокоуровневая сессия воспроизведения, объединяющая транспорт, декодирование и EQ.
class playback_session {
public:
    /// Создаёт пустую не владеющую обёртку сессии.
    playback_session() = default;
    /// Освобождает ресурсы сессии при уничтожении обёртки.
    ~playback_session();

    /// Переносит владение базовой реализацией.
    playback_session(playback_session&&) noexcept;
    /// Переносит владение базовой реализацией.
    playback_session& operator=(playback_session&&) noexcept;

    /// Сессии воспроизведения не копируются.
    playback_session(const playback_session&) = delete;
    /// Сессии воспроизведения не копируются.
    playback_session& operator=(const playback_session&) = delete;

    /// Возвращает true, если сессия всё ещё владеет состоянием runtime.
    [[nodiscard]] bool is_open() const noexcept;

    /// Загружает новый URI или путь источника.
    result<void> load(std::string source_uri);
    /// Запрашивает transport play.
    result<void> play();
    /// Запрашивает transport pause.
    result<void> pause();
    /// Запрашивает seek в текущем источнике.
    result<void> seek_to(std::int64_t position_ms);
    /// Устанавливает пользовательскую громкость в процентах.
    result<void> set_volume_percent(int volume_percent);
    /// Включает или выключает путь эквалайзера.
    result<void> set_equalizer_enabled(bool enabled);
    /// Выбирает один из встроенных пресетов EQ.
    result<void> select_equalizer_preset(equalizer_preset_id preset_id);
    /// Устанавливает gain для одной полосы EQ.
    result<void> set_equalizer_band_gain(std::size_t band_index, float gain_db);
    /// Восстанавливает плоское состояние эквалайзера.
    result<void> reset_equalizer();
    /// Устанавливает выходной gain, применяемый после обработки EQ.
    result<void> set_equalizer_output_gain(float output_gain_db);
    /// Применяет полный снимок эквалайзера.
    result<void> apply_equalizer_state(const equalizer_state& state);
    /// Перечисляет выходные устройства, доступные сессии.
    result<std::vector<device_info>> list_output_devices() const;
    /// Перепривязывает воспроизведение к другому выходному устройству.
    result<void> select_output_device(std::string device_id);
    /// Возвращает текущий снимок воспроизведения.
    [[nodiscard]] playback_state state() const;
    /// Возвращает текущий снимок эквалайзера.
    [[nodiscard]] equalizer_state equalizer_state() const;
    /// Закрывает сессию и освобождает runtime resources.
    result<void> close();

private:
    /// Скрытый тип реализации.
    class implementation;

    /// Приватный конструктор, используемый фабричными методами runtime.
    explicit playback_session(std::unique_ptr<implementation> implementation) noexcept;
    /// Внутренняя фабрика, используемая runtime.
    static result<playback_session> create(
        std::shared_ptr<detail::runtime_backend> backend,
        const playback_session_config& config);

    /// Указатель на реализацию, которая владеет полным состоянием сессии.
    std::unique_ptr<implementation> implementation_;

    /// Runtime разрешено создавать сессии напрямую.
    friend class runtime;
};

}  // namespace sonotide
