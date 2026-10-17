// The player's screens as one borealis view: Now Playing, Lyrics, the queue card, and the idle
// and not-connected screens. It draws from the Model alone, so the PC preview and the console
// run the same code.

#pragma once

#include "model.hpp"

#include <borealis/core/view.hpp>

#include <functional>

namespace player
{

// The looks a mock-up can compare.
struct Look
{
    enum class Background { tint, canvas } background = Background::tint;
    // A focused control: a filled white shape with a dark glyph (chosen in the approved mock-up), or a ring.
    enum class Focus { fill, ring } focus = Focus::fill;
    bool pill = true; // the translucent pill behind the transport controls
    float hide_after = 6.0f; // seconds without input before the controls fade away; 0: never
};

class Screen;

// `quit` runs on Circle from the base screen (the app's exit behaviour).
Screen *make_screen(App *app, Look look, std::function<void()> quit);
brls::View *as_view(Screen *screen);
// Focuses the screen so it receives the pad.
void give_focus(Screen *screen);
// Advances the heard clock and asks for nothing else: the preview calls it when it wants the
// position to run (a live window). Scripted shots leave the position where it is.
void set_clock_running(Screen *screen, bool running);

} // namespace player
