// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "model.hpp"
#include <nanovg.h>
namespace player::canvas {
void update(NVGcontext*,const Model&,bool visible);
bool paint(NVGcontext*,float x,float y,float w,float h,float alpha=1.f);
void shutdown();
}
