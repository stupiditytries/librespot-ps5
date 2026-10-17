// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "model.hpp"
#include <nanovg.h>
namespace player::art {
void prefetch(const Model &);
void upload(NVGcontext *); // bounded GPU uploads, once per frame
int image(NVGcontext *, const std::string &);
Rgb colour(const std::string &, Rgb fallback);
float opacity(const std::string &);
bool live();
} // namespace player::art
