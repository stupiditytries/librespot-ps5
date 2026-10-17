// The console's own on-screen keyboard (which also takes dictation), for typing a search.
// It is the system's dialog, drawn over the app; the app keeps drawing under it and asks
// each frame whether it has been closed.

#pragma once

#include <cstdint>
#include <string>

namespace ps5
{
class Keyboard
{
  public:
    enum class Result
    {
        None,      // not open, or still open
        Done,      // closed with what was typed
        Cancelled, // closed without
    };

    // Opens the keyboard with `text` already in it. Returns false when the console
    // would not open it.
    bool open(const std::string &text);
    bool active() const
    {
        return active_;
    }
    // Call every frame while it is open. When it has been closed, says how, and with Done
    // puts what was typed in `text`.
    Result poll(std::string &text);

  private:
    bool loaded_ = false, active_ = false;
    // The dialog writes what is typed here, as 16-bit characters.
    std::uint16_t buffer_[128] = {};
    std::uint16_t title_[16] = {};
};
} // namespace ps5
