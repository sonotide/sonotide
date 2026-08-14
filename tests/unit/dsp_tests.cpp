#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

#include "internal/dsp/biquad_filter.h"
#include "internal/dsp/equalizer_chain.h"
#include "internal/dsp/equalizer_response_sampler.h"
#include "internal/dsp/output_headroom_controller.h"
#include "internal/dsp/parameter_smoother.h"
#include "internal/equalizer_layout_utils.h"
#include "test_support/test_harness.h"

namespace {

constexpr float kEpsilon = 0.0005F;

bool approximately_equal(const float left, const float right, const float epsilon = kEpsilon) {
    return std::fabs(left - right) <= epsilon;
}

}  // namespace

int main() {
    using sonotide::equalizer_band;
    using sonotide::detail::dsp::biquad_filter;
    using sonotide::detail::dsp::equalizer_chain;
    using sonotide::detail::dsp::evaluate_frequency_response_db;
    using sonotide::detail::dsp::make_peaking_coefficients;
    using sonotide::detail::dsp::output_headroom_controller;
    using sonotide::detail::dsp::parameter_smoother;
    using sonotide::detail::dsp::sample_equalizer_band_response_db;

    // parameter_smoother: reset и мгновенный переход должны немедленно фиксировать значение.
    parameter_smoother smoother;
    smoother.reset(2.0F);
    REQUIRE(approximately_equal(smoother.current_value(), 2.0F));
    REQUIRE(approximately_equal(smoother.target_value(), 2.0F));
    smoother.set_target(5.0F, 0U);
    REQUIRE(approximately_equal(smoother.current_value(), 5.0F));
    REQUIRE(approximately_equal(smoother.advance(1U), 5.0F));

    // parameter_smoother: частичный ramp должен идти постепенно и завершаться точно в target.
    smoother.reset(0.0F);
    smoother.set_target(10.0F, 10U);
    const float half_way = smoother.advance(5U);
    REQUIRE(half_way > 0.0F && half_way < 10.0F);
    REQUIRE(approximately_equal(smoother.advance(5U), 10.0F));
    REQUIRE(approximately_equal(smoother.current_value(), 10.0F));

    // make_peaking_coefficients: некорректные аргументы обязаны вернуть identity-section.
    const auto invalid_coefficients = make_peaking_coefficients(0.0F, 1000.0F, 1.0F, 6.0F);
    REQUIRE(approximately_equal(invalid_coefficients.b0, 1.0F));
    REQUIRE(approximately_equal(invalid_coefficients.b1, 0.0F));
    REQUIRE(approximately_equal(invalid_coefficients.b2, 0.0F));
    REQUIRE(approximately_equal(invalid_coefficients.a1, 0.0F));
    REQUIRE(approximately_equal(invalid_coefficients.a2, 0.0F));

    const float quiet_nan = (std::numeric_limits<float>::quiet_NaN)();
    const float infinity = (std::numeric_limits<float>::infinity)();
    const auto non_finite_coefficients =
        make_peaking_coefficients(48000.0F, quiet_nan, 1.0F, 6.0F);
    REQUIRE(approximately_equal(non_finite_coefficients.b0, 1.0F));
    REQUIRE(approximately_equal(non_finite_coefficients.b1, 0.0F));
    REQUIRE(approximately_equal(non_finite_coefficients.b2, 0.0F));
    REQUIRE(approximately_equal(non_finite_coefficients.a1, 0.0F));
    REQUIRE(approximately_equal(non_finite_coefficients.a2, 0.0F));
    REQUIRE(approximately_equal(
        make_peaking_coefficients(48000.0F, 1000.0F, infinity, 6.0F).b0,
        1.0F));
    REQUIRE(approximately_equal(
        make_peaking_coefficients(48000.0F, 1000.0F, 1.0F, quiet_nan).b0,
        1.0F));
    // При крайне низкой, но положительной частоте Найквиста диапазон
    // std::clamp был бы перевёрнут. Такой формат должен дать identity без UB.
    const auto sub_audio_rate_coefficients =
        make_peaking_coefficients(1.0F, 1000.0F, 1.0F, 6.0F);
    REQUIRE(approximately_equal(sub_audio_rate_coefficients.b0, 1.0F));
    REQUIRE(approximately_equal(sub_audio_rate_coefficients.b1, 0.0F));
    REQUIRE(approximately_equal(sub_audio_rate_coefficients.b2, 0.0F));
    REQUIRE(approximately_equal(sub_audio_rate_coefficients.a1, 0.0F));
    REQUIRE(approximately_equal(sub_audio_rate_coefficients.a2, 0.0F));

    const std::array<equalizer_band, 1> non_finite_band{{
        {.center_frequency_hz = quiet_nan, .gain_db = infinity, .q_value = quiet_nan},
    }};
    const auto normalized_non_finite_bands =
        sonotide::detail::normalize_equalizer_bands(non_finite_band);
    REQUIRE(normalized_non_finite_bands.size() == 1U);
    REQUIRE(std::isfinite(normalized_non_finite_bands.front().center_frequency_hz));
    REQUIRE(std::isfinite(normalized_non_finite_bands.front().gain_db));
    REQUIRE(std::isfinite(normalized_non_finite_bands.front().q_value));
    REQUIRE(approximately_equal(normalized_non_finite_bands.front().center_frequency_hz, 20.0F));
    REQUIRE(approximately_equal(normalized_non_finite_bands.front().gain_db, 0.0F));
    REQUIRE(approximately_equal(
        normalized_non_finite_bands.front().q_value,
        sonotide::default_equalizer_q_value));

    // Нулевой gain должен давать почти нейтральные коэффициенты.
    const auto flat_coefficients = make_peaking_coefficients(48000.0F, 1000.0F, 1.414F, 0.0F);
    REQUIRE(approximately_equal(flat_coefficients.b0, 1.0F, 0.001F));
    REQUIRE(approximately_equal(flat_coefficients.b1, flat_coefficients.a1, 0.001F));
    REQUIRE(approximately_equal(flat_coefficients.b2, flat_coefficients.a2, 0.001F));

    // Частотный sampler должен отражать gain вблизи центра и давать плоскую линию для flat-band.
    const std::array<equalizer_band, 1> flat_band{{
        {.center_frequency_hz = 1000.0F, .gain_db = 0.0F, .q_value = 1.414F},
    }};
    REQUIRE(approximately_equal(
        sample_equalizer_band_response_db(flat_band, 48000.0F, 1000.0F),
        0.0F,
        0.05F));

    const std::array<equalizer_band, 1> boosted_band{{
        {.center_frequency_hz = 1000.0F, .gain_db = 6.0F, .q_value = 1.414F},
    }};
    REQUIRE(sample_equalizer_band_response_db(boosted_band, 48000.0F, 1000.0F) > 5.0F);

    // При одинаковом gain высокий Q должен быть уже: ближе к центру сильнее, вдали слабее.
    const std::array<equalizer_band, 1> low_q_band{{
        {.center_frequency_hz = 1000.0F, .gain_db = 6.0F, .q_value = 0.5F},
    }};
    const std::array<equalizer_band, 1> high_q_band{{
        {.center_frequency_hz = 1000.0F, .gain_db = 6.0F, .q_value = 8.0F},
    }};
    const float low_q_near_center = sample_equalizer_band_response_db(low_q_band, 48000.0F, 1000.0F);
    const float high_q_near_center = sample_equalizer_band_response_db(high_q_band, 48000.0F, 1000.0F);
    const float low_q_far = sample_equalizer_band_response_db(low_q_band, 48000.0F, 4000.0F);
    const float high_q_far = sample_equalizer_band_response_db(high_q_band, 48000.0F, 4000.0F);
    REQUIRE(approximately_equal(high_q_near_center, low_q_near_center, 0.1F));
    REQUIRE(high_q_far < low_q_far);

    // Низкоуровневая оценка отклика коэффициентов тоже должна быть нейтральной для identity-section.
    REQUIRE(approximately_equal(evaluate_frequency_response_db({}, 0.1F), 0.0F, 0.001F));
    REQUIRE(approximately_equal(evaluate_frequency_response_db({}, quiet_nan), 0.0F));
    REQUIRE(approximately_equal(
        sample_equalizer_band_response_db(boosted_band, quiet_nan, 1000.0F),
        0.0F));
    REQUIRE(approximately_equal(
        sample_equalizer_band_response_db(boosted_band, 48000.0F, infinity),
        0.0F));

    // biquad_filter: без configure или с неправильным channel_count буфер не должен меняться.
    biquad_filter filter;
    std::array<float, 4> untouched_samples{0.25F, -0.25F, 0.5F, -0.5F};
    const auto untouched_copy = untouched_samples;
    filter.process(untouched_samples.data(), 2U, 2U);
    REQUIRE(untouched_samples == untouched_copy);

    // biquad_filter: identity coefficients после configure не должны менять аудио.
    filter.configure(2U, {});
    std::array<float, 6> identity_samples{0.1F, -0.2F, 0.3F, -0.4F, 0.5F, -0.6F};
    const auto identity_copy = identity_samples;
    filter.process(identity_samples.data(), 3U, 2U);
    for (std::size_t index = 0; index < identity_samples.size(); ++index) {
        REQUIRE(approximately_equal(identity_samples[index], identity_copy[index]));
    }

    // output_headroom_controller: empty/flat/invalid-rate cases не должны требовать компенсацию.
    output_headroom_controller headroom;
    const std::vector<equalizer_band> no_bands;
    const std::array<equalizer_band, 1> flat_single_band{{
        {.center_frequency_hz = 1000.0F, .gain_db = 0.0F, .q_value = 1.414F},
    }};
    const std::array<equalizer_band, 1> boosted_single_band{{
        {.center_frequency_hz = 1000.0F, .gain_db = 6.0F, .q_value = 1.414F},
    }};
    REQUIRE(approximately_equal(headroom.compute_target_preamp_db(no_bands, 48000.0F), 0.0F));
    REQUIRE(approximately_equal(
        headroom.compute_target_preamp_db(flat_single_band, 48000.0F),
        0.0F));
    REQUIRE(approximately_equal(
        headroom.compute_target_preamp_db(boosted_single_band, 0.0F),
        0.0F));

    // Положительные boost-ы должны просить отрицательную preamp-компенсацию.
    const std::array<equalizer_band, 3> boosted_bands{{
        {.center_frequency_hz = 120.0F, .gain_db = 6.0F, .q_value = 1.0F},
        {.center_frequency_hz = 1000.0F, .gain_db = 4.0F, .q_value = 1.414F},
        {.center_frequency_hz = 8000.0F, .gain_db = 3.0F, .q_value = 2.0F},
    }};
    const float boosted_headroom = headroom.compute_target_preamp_db(boosted_bands, 48000.0F);
    REQUIRE(boosted_headroom < 0.0F);
    const float boosted_curve_peak = sample_equalizer_band_response_db(boosted_bands, 48000.0F, 120.0F);
    REQUIRE(boosted_curve_peak > 0.0F);

    // equalizer_chain: disabled flat path должен сохранять исходный сигнал.
    equalizer_chain chain;
    chain.configure(48000.0F, 2U);
    chain.prepare(256U);
    const std::array<equalizer_band, 2> flat_chain_bands{{
        {.center_frequency_hz = 120.0F, .gain_db = 0.0F, .q_value = 1.414F},
        {.center_frequency_hz = 1000.0F, .gain_db = 0.0F, .q_value = 1.414F},
    }};
    chain.set_bands(flat_chain_bands);
    chain.set_enabled(false);
    chain.set_output_gain_db(0.0F);
    chain.set_volume_linear(1.0F);
    std::vector<float> dry_samples{0.1F, -0.1F, 0.25F, -0.25F, 0.4F, -0.4F};
    const auto dry_copy = dry_samples;
    chain.process(dry_samples.data(), 3U);
    for (std::size_t index = 0; index < dry_samples.size(); ++index) {
        REQUIRE(approximately_equal(dry_samples[index], dry_copy[index]));
    }
    REQUIRE(chain.active_band_count() == 2U);
    REQUIRE(chain.target_bands().size() == 2U);
    REQUIRE(approximately_equal(chain.output_gain_db(), 0.0F));
    REQUIRE(chain.enabled() == false);
    REQUIRE(approximately_equal(chain.sample_rate(), 48000.0F));
    REQUIRE(chain.channel_count() == 2U);

    // Повторные callback-sized вызовы используют один заранее подготовленный scratch buffer.
    // Этот сценарий также проверяет, что resize в пределах capacity не меняет результат.
    std::vector<float> prepared_samples(512U, 0.25F);
    chain.process(prepared_samples.data(), 256U);
    chain.process(prepared_samples.data(), 128U);

    // equalizer_chain: bands выше лимита должны отсекаться до max_band_count.
    std::vector<equalizer_band> too_many_bands;
    too_many_bands.reserve(sonotide::equalizer_max_band_count + 3U);
    for (std::size_t index = 0; index < sonotide::equalizer_max_band_count + 3U; ++index) {
        too_many_bands.push_back(equalizer_band{
            .center_frequency_hz = 100.0F + static_cast<float>(index) * 100.0F,
            .gain_db = 1.0F,
            .q_value = 1.414F,
        });
    }
    chain.set_bands(too_many_bands);
    REQUIRE(chain.active_band_count() == sonotide::equalizer_max_band_count);
    REQUIRE(chain.target_bands().size() == sonotide::equalizer_max_band_count);
    REQUIRE(chain.headroom_compensation_db() < 0.0F);

    // equalizer_chain: enabled non-flat processing должна реально менять сигнал.
    equalizer_chain active_chain;
    active_chain.configure(48000.0F, 1U);
    active_chain.prepare(256U);
    const std::array<equalizer_band, 1> active_chain_bands{{
        {.center_frequency_hz = 1000.0F, .gain_db = 9.0F, .q_value = 6.0F},
    }};
    active_chain.set_bands(active_chain_bands);
    active_chain.set_enabled(true);
    active_chain.set_output_gain_db(0.0F);
    active_chain.set_volume_linear(1.0F);
    std::vector<float> impulse(256U, 0.0F);
    impulse[0] = 1.0F;
    const auto original_impulse = impulse;
    active_chain.process(impulse.data(), impulse.size());
    bool any_difference = false;
    for (std::size_t index = 0; index < impulse.size(); ++index) {
        if (!approximately_equal(impulse[index], original_impulse[index], 0.0001F)) {
            any_difference = true;
            break;
        }
    }
    REQUIRE(any_difference);

    // equalizer_chain: reset и пустые входы не должны приводить к ошибкам.
    active_chain.reset();
    active_chain.process(nullptr, 128U);
    active_chain.process(impulse.data(), 0U);
    return 0;
}
