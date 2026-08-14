#include <string_view>

#include "sonotide/audio_buffer.h"
#include "sonotide/audio_format.h"
#include "sonotide/capture_stream.h"
#include "sonotide/device_info.h"
#include "sonotide/device_selector.h"
#include "sonotide/equalizer.h"
#include "sonotide/error.h"
#include "sonotide/loopback_capture_stream.h"
#include "sonotide/playback_session.h"
#include "sonotide/playback_state.h"
#include "sonotide/render_stream.h"
#include "sonotide/result.h"
#include "sonotide/runtime.h"
#include "sonotide/stream_callback.h"
#include "sonotide/stream_config.h"
#include "sonotide/stream_state.h"
#include "sonotide/stream_status.h"
#include "sonotide/version.h"

int main() {
    if (sonotide::version_string != std::string_view(SONOTIDE_EXPECTED_VERSION)) {
        return 1;
    }

    // Calling an out-of-line implementation function verifies that the imported
    // target resolves the installed library without initializing Windows audio.
    return sonotide::to_string(sonotide::error_category::initialization).empty() ? 1 : 0;
}
