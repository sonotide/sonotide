#include <array>
#include <cassert>
#include <cmath>
#include <string_view>
#include <vector>

#include "sonotide/equalizer.h"

int main() {
    // Чтобы тест был компактным, используем локальный псевдоним для перечисления пресетов.
    using sonotide::equalizer_preset_id;
    constexpr float kEpsilon = 0.01F;

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

    const auto q_limits = sonotide::supported_equalizer_q_limits();
    assert(q_limits.min_q_value == 0.1F);
    assert(q_limits.max_q_value == 12.0F);
    assert(sonotide::default_equalizer_q_value >= q_limits.min_q_value);
    assert(sonotide::default_equalizer_q_value <= q_limits.max_q_value);

    // Дефолтные раскладки должны быть доступны для любого количества полос от 0 до 10.
    for (std::size_t band_count = 0; band_count <= sonotide::equalizer_max_band_count; ++band_count) {
        const auto bands = sonotide::make_default_equalizer_bands(band_count);
        assert(bands.size() == band_count);
        for (std::size_t index = 0; index < bands.size(); ++index) {
            assert(bands[index].gain_db == 0.0F);
            assert(std::fabs(bands[index].q_value - sonotide::default_equalizer_q_value) < kEpsilon);
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
        assert(std::fabs(state.bands[index].q_value - sonotide::default_equalizer_q_value) < 0.0001F);
        assert(std::fabs(state.last_nonflat_band_gains_db[index]) < 0.0001F);
    }

    // Public response sampling: disabled EQ должен возвращать плоскую линию 0 dB.
    const std::array<float, 3> frequencies_hz{{100.0F, 1000.0F, 8000.0F}};
    auto disabled_curve_result = sonotide::sample_equalizer_response(state, 48000.0F, frequencies_hz);
    assert(disabled_curve_result);
    assert(disabled_curve_result.value().enabled == false);
    assert(std::fabs(disabled_curve_result.value().applied_headroom_compensation_db) < kEpsilon);
    assert(std::fabs(disabled_curve_result.value().applied_output_gain_db) < kEpsilon);
    assert(disabled_curve_result.value().points.size() == frequencies_hz.size());
    for (std::size_t index = 0; index < frequencies_hz.size(); ++index) {
        assert(std::fabs(disabled_curve_result.value().points[index].frequency_hz - frequencies_hz[index]) < kEpsilon);
        assert(std::fabs(disabled_curve_result.value().points[index].response_db) < kEpsilon);
    }

    // Invalid sampling inputs must be rejected explicitly.
    auto empty_curve_result =
        sonotide::sample_equalizer_response(state, 48000.0F, std::span<const float>{});
    assert(!empty_curve_result);
    assert(empty_curve_result.error().code == sonotide::error_code::invalid_argument);

    auto invalid_rate_result = sonotide::sample_equalizer_response(state, 0.0F, frequencies_hz);
    assert(!invalid_rate_result);
    assert(invalid_rate_result.error().code == sonotide::error_code::invalid_argument);

    const std::array<float, 1> invalid_frequency_hz{{48000.0F}};
    auto invalid_frequency_result =
        sonotide::sample_equalizer_response(state, 48000.0F, invalid_frequency_hz);
    assert(!invalid_frequency_result);
    assert(invalid_frequency_result.error().code == sonotide::error_code::invalid_argument);

    // Enabled EQ should apply a real curve, auto headroom compensation, output gain, and q clamping.
    sonotide::equalizer_state active_state;
    active_state.enabled = true;
    active_state.output_gain_db = 2.0F;
    active_state.bands = {{
        {.center_frequency_hz = 1000.0F, .gain_db = 6.0F, .q_value = 2.0F},
        {.center_frequency_hz = 6000.0F, .gain_db = -3.0F, .q_value = 100.0F},
    }};
    const std::array<float, 4> active_frequencies_hz{{200.0F, 1000.0F, 6000.0F, 12000.0F}};
    auto active_curve_result =
        sonotide::sample_equalizer_response(active_state, 48000.0F, active_frequencies_hz);
    assert(active_curve_result);
    assert(active_curve_result.value().enabled == true);
    assert(active_curve_result.value().applied_headroom_compensation_db < 0.0F);
    assert(std::fabs(active_curve_result.value().applied_output_gain_db - 2.0F) < kEpsilon);
    assert(active_curve_result.value().points.size() == active_frequencies_hz.size());
    assert(active_curve_result.value().points[1].response_db >
        active_curve_result.value().points[0].response_db);
    assert(active_curve_result.value().points[1].response_db >
        active_curve_result.value().points[3].response_db);
    assert(active_curve_result.value().points[2].response_db <
        active_curve_result.value().points[1].response_db);

    // Helper должен нормализовать временный layout так же, как это делает playback path:
    // сортировать полосы, выдерживать минимальный зазор и clamp-ить q/gain.
    sonotide::equalizer_state raw_state;
    raw_state.enabled = true;
    raw_state.output_gain_db = 1.5F;
    raw_state.bands = {{
        {.center_frequency_hz = 1005.0F, .gain_db = -3.0F, .q_value = 100.0F},
        {.center_frequency_hz = 1000.0F, .gain_db = 6.0F, .q_value = 0.05F},
    }};
    sonotide::equalizer_state normalized_state;
    normalized_state.enabled = true;
    normalized_state.output_gain_db = 1.5F;
    normalized_state.bands = {{
        {.center_frequency_hz = 1000.0F, .gain_db = 6.0F, .q_value = 0.1F},
        {.center_frequency_hz = 1010.0F, .gain_db = -3.0F, .q_value = 12.0F},
    }};

    const std::array<float, 3> normalized_frequencies_hz{{500.0F, 1000.0F, 4000.0F}};
    auto raw_curve_result =
        sonotide::sample_equalizer_response(raw_state, 48000.0F, normalized_frequencies_hz);
    auto normalized_curve_result =
        sonotide::sample_equalizer_response(normalized_state, 48000.0F, normalized_frequencies_hz);
    assert(raw_curve_result);
    assert(normalized_curve_result);
    assert(std::fabs(
        raw_curve_result.value().applied_headroom_compensation_db -
        normalized_curve_result.value().applied_headroom_compensation_db) < kEpsilon);
    assert(std::fabs(
        raw_curve_result.value().applied_output_gain_db -
        normalized_curve_result.value().applied_output_gain_db) < kEpsilon);
    assert(raw_curve_result.value().points.size() == normalized_curve_result.value().points.size());
    for (std::size_t index = 0; index < raw_curve_result.value().points.size(); ++index) {
        assert(std::fabs(
            raw_curve_result.value().points[index].response_db -
            normalized_curve_result.value().points[index].response_db) < kEpsilon);
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
