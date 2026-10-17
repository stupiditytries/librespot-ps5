// SPDX-License-Identifier: GPL-3.0-or-later
// Real software decode with a synthetic H.264 clip; GPU operations are counted.
#include "canvas_video.hpp"
#include "perf.hpp"
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
static int uploads=0,deleted=0;
int nvgCreateImageRGBA(NVGcontext*,int w,int h,int,const unsigned char*){assert(w>0&&h>0&&w<=960&&h<=960);++uploads;return 1;}
void nvgUpdateImage(NVGcontext*,int,const unsigned char*){++uploads;}
void nvgDeleteImage(NVGcontext*,int){++deleted;}
void nvgBeginPath(NVGcontext*){}
void nvgRect(NVGcontext*,float,float,float,float){}
NVGpaint nvgImagePattern(NVGcontext*,float,float,float,float,float,int,float){return {};}
void nvgFillPaint(NVGcontext*,NVGpaint){}
void nvgFill(NVGcontext*){}
int main(){
 player::Model m;m.live=true;m.has_canvas=true;m.canvas_kind="video";m.canvas_path="/media/canvas/"+std::string(40,'a');m.moving=true;
 const auto tick=[&](int ms){for(int n=0;n<ms;n+=10){player::canvas::update(nullptr,m,true);std::this_thread::sleep_for(std::chrono::milliseconds(10));}};
 tick(2400);assert(uploads>=20&&player::canvas::paint(nullptr,0,0,1920,1080));
 const auto resets=player::perf::canvas_resets.load(),loads=player::perf::canvas_loads.load();
 int hidden_uploads=uploads;
 for(int i=0;i<30;++i){player::canvas::update(nullptr,m,false);std::this_thread::sleep_for(std::chrono::milliseconds(10));}
 assert(uploads==hidden_uploads&&player::canvas::paint(nullptr,0,0,1920,1080));
 tick(200);assert(player::perf::canvas_resets==resets&&player::perf::canvas_loads==loads);
 m.moving=false;tick(100);int paused=uploads;tick(250);assert(uploads==paused);
 m.moving=true;tick(400);assert(uploads>paused);
 m.has_canvas=false;tick(100);int stopped=uploads;tick(100);assert(stopped==uploads&&deleted>0);
 m.has_canvas=true;m.canvas_path="/media/canvas/"+std::string(40,'b');tick(450);assert(uploads>stopped);
 m.canvas_path="/media/canvas/"+std::string(40,'c');tick(250);int bad=uploads;tick(200);assert(uploads==bad&&!player::canvas::paint(nullptr,0,0,100,100));
 player::canvas::shutdown();std::cout<<"Canvas decode, loop, heard pause, cancellation and truncated-body checks passed; uploads="<<uploads<<"\n";
}
