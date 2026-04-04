#pragma once

#include <span>

#include "sonotide/equalizer.h"

namespace sonotide::detail::dsp {

/// Оценивает безопасную компенсацию предусиления для многополосной EQ-кривой.
class output_headroom_controller {
public:
    /// Возвращает рекомендуемое ослабление предусиления в децибелах.
    [[nodiscard]] float compute_target_preamp_db(
        std::span<const equalizer_band> bands,
        float sample_rate) const;
};

}  // пространство имён sonotide::detail::dsp
