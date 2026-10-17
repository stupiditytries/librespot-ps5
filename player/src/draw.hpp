// Drawing helpers: fonts, text, our own icons (nanovg paths, sharp at any size) and covers.

#pragma once

#include "model.hpp"

#include <nanovg.h>

#include <string>

namespace player::draw
{

enum class Weight { regular, medium, semibold, bold };

// Loads Plus Jakarta Sans from `dir` into borealis' font table. False if a file is missing.
bool load_fonts(const std::string &dir);

NVGcolor rgba(int r, int g, int b, float a = 1.0f);
NVGcolor rgb(Rgb c, float a = 1.0f);
NVGcolor white(float a = 1.0f);
NVGcolor grey(float a = 1.0f);
NVGcolor mix(NVGcolor a, NVGcolor b, float t);
Rgb mix(Rgb a, Rgb b, float t);

// Colours the way the plan describes (3.3): the cover's hue at a given brightness.
Rgb from_hsv(float h, float s, float v);
void to_hsv(Rgb c, float *h, float *s, float *v);
Rgb dark_tint(Rgb cover);     // the no-Canvas background
Rgb lyrics_colour(Rgb cover); // brightness clamped so white text stays readable
Rgb dominant_colour(const std::string &image_file, Rgb fallback);

float ease_out(float t);

// Text. `x` is the left edge (or right/centre per align), `base` the baseline.
enum class Align { left, right, centre };
void text(NVGcontext *vg, Weight w, float size, NVGcolor colour, float x, float base, const std::string &s,
          Align align = Align::left, float tracking = 0.0f);
float text_width(NVGcontext *vg, Weight w, float size, const std::string &s, float tracking = 0.0f);
// The text cut with an ellipsis to fit `max_width`.
std::string fit(NVGcontext *vg, Weight w, float size, const std::string &s, float max_width);
std::vector<std::string> title_lines(NVGcontext*,Weight,float,const std::string&,float);

enum class Icon { shuffle, previous, next, play, pause, repeat, repeat_one, heart, heart_filled, lyrics, queue, note, search, plus, check };
// Draws the icon centred at (cx, cy), `size` px across.
void icon(NVGcontext *vg, Icon i, float cx, float cy, float size, NVGcolor colour);

// A cover: the image if `file` loads, otherwise a painted sample cover from `c`.
void cover(NVGcontext *vg, const std::string &file, Rgb c, float x, float y, float size, float radius,
           float alpha = 1.0f);
// An image filling a rect, cropped to fill (the Canvas framing the captures show).
bool image_fill(NVGcontext *vg, const std::string &file, float x, float y, float w, float h, float alpha, bool contain=false);
// A painted stand-in for a Canvas frame when no still is given.
void canvas_stand_in(NVGcontext *vg, Rgb c, float x, float y, float w, float h);

void equaliser(NVGcontext *vg, float x, float y, float h, float t, NVGcolor colour);
void spinner(NVGcontext *vg, float cx, float cy, float r, float t, NVGcolor colour);

} // namespace player::draw
