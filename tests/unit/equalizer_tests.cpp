#include <array>
#include <cmath>
#include <limits>
#include <string_view>
#include <vector>

#include "sonotide/equalizer.h"
#include "test_support/test_harness.h"

int main() {
    // Чтобы тест был компактным, используем локальный псевдоним для перечисления пресетов.
    using sonotide::equalizer_preset_id;
    constexpr float kEpsilon = 0.01F;

    // Строковые представления должны быть стабильными, потому что их используют документация и сохранение состояния.
    REQUIRE(sonotide::to_string(equalizer_preset_id::rock) == "rock");
    REQUIRE(sonotide::to_string(sonotide::equalizer_status::ready) == "ready");

    // Проверяем обратное преобразование известного идентификатора пресета через парсер.
    const auto parsed_rock = sonotide::equalizer_preset_id_from_string("rock");
    REQUIRE(parsed_rock.has_value());
    REQUIRE(parsed_rock.value() == equalizer_preset_id::rock);

    // Неизвестные строки должны корректно отклоняться без запасного пресета.
    const auto parsed_unknown = sonotide::equalizer_preset_id_from_string("not_a_preset");
    REQUIRE(!parsed_unknown.has_value());

    // Публичные лимиты должны отражать новый гибкий EQ с максимумом 10 полос.
    const auto band_count_limits = sonotide::supported_equalizer_band_count_limits();
    REQUIRE(band_count_limits.min_band_count == 0U);
    REQUIRE(band_count_limits.max_band_count == sonotide::equalizer_max_band_count);

    const auto frequency_limits = sonotide::supported_equalizer_frequency_limits();
    REQUIRE(frequency_limits.min_frequency_hz == 20.0F);
    REQUIRE(frequency_limits.max_frequency_hz == 20000.0F);
    REQUIRE(frequency_limits.min_band_spacing_hz == 10.0F);

    const auto q_limits = sonotide::supported_equalizer_q_limits();
    REQUIRE(q_limits.min_q_value == 0.1F);
    REQUIRE(q_limits.max_q_value == 12.0F);
    REQUIRE(sonotide::default_equalizer_q_value >= q_limits.min_q_value);
    REQUIRE(sonotide::default_equalizer_q_value <= q_limits.max_q_value);

    // Дефолтные раскладки должны быть доступны для любого количества полос от 0 до 10.
    for (std::size_t band_count = 0; band_count <= sonotide::equalizer_max_band_count; ++band_count) {
        const auto bands = sonotide::make_default_equalizer_bands(band_count);
        REQUIRE(bands.size() == band_count);
        for (std::size_t index = 0; index < bands.size(); ++index) {
            REQUIRE(bands[index].gain_db == 0.0F);
            REQUIRE(std::fabs(bands[index].q_value - sonotide::default_equalizer_q_value) < kEpsilon);
            REQUIRE(bands[index].center_frequency_hz >= frequency_limits.min_frequency_hz);
            REQUIRE(bands[index].center_frequency_hz <= frequency_limits.max_frequency_hz);
            if (index > 0U) {
                REQUIRE(
                    bands[index].center_frequency_hz - bands[index - 1U].center_frequency_hz >=
                    frequency_limits.min_band_spacing_hz);
            }
        }
    }

    // Значения выше максимума должны безопасно зажиматься к допустимому числу полос.
    const auto clamped_layout =
        sonotide::make_default_equalizer_bands(sonotide::equalizer_max_band_count + 7U);
    REQUIRE(clamped_layout.size() == sonotide::equalizer_max_band_count);

    // Крайние полосы должны опираться на глобальные лимиты частот.
    const auto default_ten_band_layout =
        sonotide::make_default_equalizer_bands(sonotide::equalizer_max_band_count);
    const auto first_band_range =
        sonotide::equalizer_band_editable_frequency_range(default_ten_band_layout, 0U);
    const auto last_band_range =
        sonotide::equalizer_band_editable_frequency_range(
            default_ten_band_layout,
            default_ten_band_layout.size() - 1U);
    REQUIRE(first_band_range.has_value());
    REQUIRE(last_band_range.has_value());
    REQUIRE(first_band_range->min_frequency_hz == frequency_limits.min_frequency_hz);
    REQUIRE(last_band_range->max_frequency_hz == frequency_limits.max_frequency_hz);

    // Диапазон перемещения полосы должен учитывать соседей и глобальные ограничения.
    const auto default_five_band_layout = sonotide::make_default_equalizer_bands(5);
    const auto editable_middle_band_range =
        sonotide::equalizer_band_editable_frequency_range(default_five_band_layout, 2U);
    REQUIRE(editable_middle_band_range.has_value());
    REQUIRE(
        editable_middle_band_range->min_frequency_hz >=
        default_five_band_layout[1].center_frequency_hz + frequency_limits.min_band_spacing_hz);
    REQUIRE(
        editable_middle_band_range->max_frequency_hz <=
        default_five_band_layout[3].center_frequency_hz - frequency_limits.min_band_spacing_hz);

    // Неверный индекс обязан возвращать nullopt.
    const auto invalid_index_range =
        sonotide::equalizer_band_editable_frequency_range(default_five_band_layout, 99U);
    REQUIRE(!invalid_index_range.has_value());

    // Слишком плотная раскладка должна корректно сообщать, что полосу нельзя двигать.
    const std::vector<sonotide::equalizer_band> impossible_layout{
        {.center_frequency_hz = 1000.0F, .gain_db = 0.0F},
        {.center_frequency_hz = 1005.0F, .gain_db = 0.0F},
        {.center_frequency_hz = 1010.0F, .gain_db = 0.0F},
    };
    const auto impossible_middle_range =
        sonotide::equalizer_band_editable_frequency_range(impossible_layout, 1U);
    REQUIRE(!impossible_middle_range.has_value());

    // Состояние по умолчанию по-прежнему должно быть удобным для старого 10-band UI.
    sonotide::equalizer_state state;
    REQUIRE(state.status == sonotide::equalizer_status::loading);
    REQUIRE(state.enabled == false);
    REQUIRE(state.active_preset_id == sonotide::equalizer_preset_id::flat);
    REQUIRE(state.bands.size() == sonotide::equalizer_max_band_count);
    REQUIRE(state.last_nonflat_band_gains_db.size() == sonotide::equalizer_max_band_count);
    for (std::size_t index = 0; index < state.bands.size(); ++index) {
        REQUIRE(std::fabs(state.bands[index].gain_db) < 0.0001F);
        REQUIRE(std::fabs(state.bands[index].q_value - sonotide::default_equalizer_q_value) < 0.0001F);
        REQUIRE(std::fabs(state.last_nonflat_band_gains_db[index]) < 0.0001F);
    }

    // Public response sampling: disabled EQ должен возвращать плоскую линию 0 dB.
    const std::array<float, 3> frequencies_hz{{100.0F, 1000.0F, 8000.0F}};
    auto disabled_curve_result = sonotide::sample_equalizer_response(state, 48000.0F, frequencies_hz);
    REQUIRE(disabled_curve_result);
    REQUIRE(disabled_curve_result.value().enabled == false);
    REQUIRE(std::fabs(disabled_curve_result.value().applied_headroom_compensation_db) < kEpsilon);
    REQUIRE(std::fabs(disabled_curve_result.value().applied_output_gain_db) < kEpsilon);
    REQUIRE(disabled_curve_result.value().points.size() == frequencies_hz.size());
    for (std::size_t index = 0; index < frequencies_hz.size(); ++index) {
        REQUIRE(std::fabs(disabled_curve_result.value().points[index].frequency_hz - frequencies_hz[index]) < kEpsilon);
        REQUIRE(std::fabs(disabled_curve_result.value().points[index].response_db) < kEpsilon);
    }

    // Invalid sampling inputs must be rejected explicitly.
    auto empty_curve_result =
        sonotide::sample_equalizer_response(state, 48000.0F, std::span<const float>{});
    REQUIRE(!empty_curve_result);
    REQUIRE(empty_curve_result.error().code == sonotide::error_code::invalid_argument);

    auto invalid_rate_result = sonotide::sample_equalizer_response(state, 0.0F, frequencies_hz);
    REQUIRE(!invalid_rate_result);
    REQUIRE(invalid_rate_result.error().code == sonotide::error_code::invalid_argument);

    const std::array<float, 1> invalid_frequency_hz{{48000.0F}};
    auto invalid_frequency_result =
        sonotide::sample_equalizer_response(state, 48000.0F, invalid_frequency_hz);
    REQUIRE(!invalid_frequency_result);
    REQUIRE(invalid_frequency_result.error().code == sonotide::error_code::invalid_argument);

    const float quiet_nan = (std::numeric_limits<float>::quiet_NaN)();
    const float infinity = (std::numeric_limits<float>::infinity)();
    REQUIRE(!sonotide::sample_equalizer_response(state, quiet_nan, frequencies_hz));
    REQUIRE(!sonotide::sample_equalizer_response(state, infinity, frequencies_hz));

    const std::array<float, 1> non_finite_frequency_hz{{quiet_nan}};
    REQUIRE(!sonotide::sample_equalizer_response(state, 48000.0F, non_finite_frequency_hz));

    sonotide::equalizer_state non_finite_state = state;
    non_finite_state.output_gain_db = infinity;
    REQUIRE(!sonotide::sample_equalizer_response(
        non_finite_state,
        48000.0F,
        frequencies_hz));

    non_finite_state = state;
    non_finite_state.bands.front().center_frequency_hz = quiet_nan;
    REQUIRE(!sonotide::sample_equalizer_response(
        non_finite_state,
        48000.0F,
        frequencies_hz));

    non_finite_state = state;
    non_finite_state.bands.front().gain_db = infinity;
    REQUIRE(!sonotide::sample_equalizer_response(
        non_finite_state,
        48000.0F,
        frequencies_hz));

    non_finite_state = state;
    non_finite_state.bands.front().q_value = quiet_nan;
    REQUIRE(!sonotide::sample_equalizer_response(
        non_finite_state,
        48000.0F,
        frequencies_hz));

    const std::array<sonotide::equalizer_band, 1> invalid_edit_layout{{
        {.center_frequency_hz = quiet_nan, .gain_db = 0.0F, .q_value = 1.0F},
    }};
    REQUIRE(!sonotide::equalizer_band_editable_frequency_range(invalid_edit_layout, 0U));

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
    REQUIRE(active_curve_result);
    REQUIRE(active_curve_result.value().enabled == true);
    REQUIRE(active_curve_result.value().applied_headroom_compensation_db < 0.0F);
    REQUIRE(std::fabs(active_curve_result.value().applied_output_gain_db - 2.0F) < kEpsilon);
    REQUIRE(active_curve_result.value().points.size() == active_frequencies_hz.size());
    REQUIRE(active_curve_result.value().points[1].response_db >
        active_curve_result.value().points[0].response_db);
    REQUIRE(active_curve_result.value().points[1].response_db >
        active_curve_result.value().points[3].response_db);
    REQUIRE(active_curve_result.value().points[2].response_db <
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
    REQUIRE(raw_curve_result);
    REQUIRE(normalized_curve_result);
    REQUIRE(std::fabs(
        raw_curve_result.value().applied_headroom_compensation_db -
        normalized_curve_result.value().applied_headroom_compensation_db) < kEpsilon);
    REQUIRE(std::fabs(
        raw_curve_result.value().applied_output_gain_db -
        normalized_curve_result.value().applied_output_gain_db) < kEpsilon);
    REQUIRE(raw_curve_result.value().points.size() == normalized_curve_result.value().points.size());
    for (std::size_t index = 0; index < raw_curve_result.value().points.size(); ++index) {
        REQUIRE(std::fabs(
            raw_curve_result.value().points[index].response_db -
            normalized_curve_result.value().points[index].response_db) < kEpsilon);
    }

    // Текстовые токены должны оставаться стабильными для UI и сериализации.
    REQUIRE(
        sonotide::to_string(sonotide::equalizer_status::unsupported_audio_path) ==
        std::string_view("unsupported_audio_path"));
    REQUIRE(
        sonotide::to_string(sonotide::equalizer_preset_id::spoken_podcast) ==
        std::string_view("spoken_podcast"));
    return 0;
}
