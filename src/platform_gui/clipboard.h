// Clipboard text.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "base/error.h"

namespace kt::platform {

enum class Selection : uint8_t { clipboard, primary };  // primary: X11/Wayland only

class IClipboard {
public:
    virtual ~IClipboard() = default;
    // Async because X11 and Wayland deliver data through the event loop. The callback always
    // runs on the UI thread from the loop, never from inside read_text().
    virtual void read_text(Selection, std::function<void(Result<std::u8string>)>) = 0;
    virtual Result<void> write_text(Selection, std::u8string_view text,
                                    bool sensitive = false) = 0;  // excluded from history
    virtual bool has_primary() const = 0;
    // The clipboard's image as PNG bytes; empty when it holds none. Same callback rules as
    // read_text(). Backends without image support answer empty.
    virtual void read_image(std::function<void(Result<std::vector<uint8_t>>)> cb) { cb(std::vector<uint8_t>{}); }
    static constexpr size_t kMaxBytes = size_t{64} << 20;  // reads beyond this are truncated with an error
};

}  // namespace kt::platform
