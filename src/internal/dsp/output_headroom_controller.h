#pragma once

#include <array>

#include "sonotide/equalizer.h"

namespace sonotide::detail::dsp {

/// Оценивает безопасную компенсацию предусиления для многополосной EQ-кривой.
class output_headroom_controller {
public:
    /// Возвращает рекомендуемое ослабление предусиления в децибелах.
    [[nodiscard]] float compute_target_preamp_db(
        const std::array<float, equalizer_band_count>& band_gains_db,
        float sample_rate) const;
};

}  // пространство имён sonotide::detail::dsp
