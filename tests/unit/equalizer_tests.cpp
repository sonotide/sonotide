#include <cassert>
#include <cmath>
#include <string_view>
#include <vector>

#include "sonotide/equalizer.h"

int main() {
    // Чтобы тест был компактным, используем локальный псевдоним для перечисления пресетов.
    using sonotide::equalizer_preset_id;

    // Строковые представления должны быть стабильными, потому что их используют документация и сохранение состояния.
    assert(sonotide::to_string(equalizer_preset_id::rock) == "rock");
    assert(sonotide::to_string(sonotide::equalizer_status::ready) == "ready");

    // Проверяем обратное преобразование известного идентификатора пресета через парсер.
    const auto parsed_rock = sonotide::equalizer_preset_id_from_string("rock");
    assert(parsed_rock.has_value());
    assert(parsed_rock.value() == equalizer_preset_id::rock);

    // Неизвестные строки должны корректно отклоняться без запасного пресета.
    const auto parsed_unknown = sonotide::equalizer_preset_id_from_string("not_a_preset");
    assert(!parsed_unknown.has_value());

    // Публичные лимиты должны отражать новый гибкий EQ с максимумом 10 полос.
    const auto band_count_limits = sonotide::supported_equalizer_band_count_limits();
    assert(band_count_limits.min_band_count == 0U);
    assert(band_count_limits.max_band_count == sonotide::equalizer_max_band_count);

    const auto frequency_limits = sonotide::supported_equalizer_frequency_limits();
    assert(frequency_limits.min_frequency_hz == 20.0F);
    assert(frequency_limits.max_frequency_hz == 20000.0F);
    assert(frequency_limits.min_band_spacing_hz == 10.0F);

    // Дефолтные раскладки должны быть доступны для любого количества полос от 0 до 10.
    for (std::size_t band_count = 0; band_count <= sonotide::equalizer_max_band_count; ++band_count) {
        const auto bands = sonotide::make_default_equalizer_bands(band_count);
        assert(bands.size() == band_count);
        for (std::size_t index = 0; index < bands.size(); ++index) {
            assert(bands[index].gain_db == 0.0F);
            assert(bands[index].center_frequency_hz >= frequency_limits.min_frequency_hz);
            assert(bands[index].center_frequency_hz <= frequency_limits.max_frequency_hz);
            if (index > 0U) {
                assert(
                    bands[index].center_frequency_hz - bands[index - 1U].center_frequency_hz >=
                    frequency_limits.min_band_spacing_hz);
            }
        }
    }

    // Значения выше максимума должны безопасно зажиматься к допустимому числу полос.
    const auto clamped_layout =
        sonotide::make_default_equalizer_bands(sonotide::equalizer_max_band_count + 7U);
    assert(clamped_layout.size() == sonotide::equalizer_max_band_count);

    // Крайние полосы должны опираться на глобальные лимиты частот.
    const auto default_ten_band_layout =
        sonotide::make_default_equalizer_bands(sonotide::equalizer_max_band_count);
    const auto first_band_range =
        sonotide::equalizer_band_editable_frequency_range(default_ten_band_layout, 0U);
    const auto last_band_range =
        sonotide::equalizer_band_editable_frequency_range(
            default_ten_band_layout,
            default_ten_band_layout.size() - 1U);
    assert(first_band_range.has_value());
    assert(last_band_range.has_value());
    assert(first_band_range->min_frequency_hz == frequency_limits.min_frequency_hz);
    assert(last_band_range->max_frequency_hz == frequency_limits.max_frequency_hz);

    // Диапазон перемещения полосы должен учитывать соседей и глобальные ограничения.
    const auto default_five_band_layout = sonotide::make_default_equalizer_bands(5);
    const auto editable_middle_band_range =
        sonotide::equalizer_band_editable_frequency_range(default_five_band_layout, 2U);
    assert(editable_middle_band_range.has_value());
    assert(
        editable_middle_band_range->min_frequency_hz >=
        default_five_band_layout[1].center_frequency_hz + frequency_limits.min_band_spacing_hz);
    assert(
        editable_middle_band_range->max_frequency_hz <=
        default_five_band_layout[3].center_frequency_hz - frequency_limits.min_band_spacing_hz);

    // Неверный индекс обязан возвращать nullopt.
    const auto invalid_index_range =
        sonotide::equalizer_band_editable_frequency_range(default_five_band_layout, 99U);
    assert(!invalid_index_range.has_value());

    // Слишком плотная раскладка должна корректно сообщать, что полосу нельзя двигать.
    const std::vector<sonotide::equalizer_band> impossible_layout{
        {.center_frequency_hz = 1000.0F, .gain_db = 0.0F},
        {.center_frequency_hz = 1005.0F, .gain_db = 0.0F},
        {.center_frequency_hz = 1010.0F, .gain_db = 0.0F},
    };
    const auto impossible_middle_range =
        sonotide::equalizer_band_editable_frequency_range(impossible_layout, 1U);
    assert(!impossible_middle_range.has_value());

    // Состояние по умолчанию по-прежнему должно быть удобным для старого 10-band UI.
    sonotide::equalizer_state state;
    assert(state.status == sonotide::equalizer_status::loading);
    assert(state.enabled == false);
    assert(state.active_preset_id == sonotide::equalizer_preset_id::flat);
    assert(state.bands.size() == sonotide::equalizer_max_band_count);
    assert(state.last_nonflat_band_gains_db.size() == sonotide::equalizer_max_band_count);
    for (std::size_t index = 0; index < state.bands.size(); ++index) {
        assert(std::fabs(state.bands[index].gain_db) < 0.0001F);
        assert(std::fabs(state.last_nonflat_band_gains_db[index]) < 0.0001F);
    }

    // Текстовые токены должны оставаться стабильными для UI и сериализации.
    assert(
        sonotide::to_string(sonotide::equalizer_status::unsupported_audio_path) ==
        std::string_view("unsupported_audio_path"));
    assert(
        sonotide::to_string(sonotide::equalizer_preset_id::spoken_podcast) ==
        std::string_view("spoken_podcast"));
    return 0;
}
