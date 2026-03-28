#include <cassert>

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

    // Состояние по умолчанию должно содержать фиксированную 10-полосную схему, которую ожидает движок воспроизведения.
    sonotide::equalizer_state state;
    assert(state.bands.size() == sonotide::equalizer_band_count);
    assert(state.last_nonflat_band_gains_db.size() == sonotide::equalizer_band_count);
    return 0;
}
