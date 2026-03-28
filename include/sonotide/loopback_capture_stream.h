#pragma once

#include <memory>

#include "sonotide/result.h"
#include "sonotide/stream_status.h"

namespace sonotide {
namespace detail {
class stream_handle;
}

class loopback_capture_stream {
public:
    loopback_capture_stream() = default;
    ~loopback_capture_stream() = default;

    loopback_capture_stream(loopback_capture_stream&&) noexcept = default;
    loopback_capture_stream& operator=(loopback_capture_stream&&) noexcept = default;

    loopback_capture_stream(const loopback_capture_stream&) = delete;
    loopback_capture_stream& operator=(const loopback_capture_stream&) = delete;

    [[nodiscard]] bool is_open() const noexcept;
    result<void> start();
    result<void> stop();
    result<void> reset();
    result<void> close();
    [[nodiscard]] stream_status status() const;

private:
    explicit loopback_capture_stream(std::shared_ptr<detail::stream_handle> handle) noexcept;

    std::shared_ptr<detail::stream_handle> handle_;

    friend class runtime;
};

}  // namespace sonotide

