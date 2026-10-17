// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <string>
namespace player {
struct ImageRect {float x,y,w,h;};
inline ImageRect fitted_image(float iw,float ih,float x,float y,float w,float h,bool contain){
    if(iw<=0||ih<=0)return {x,y,0,0};
    const float k=contain?std::min(w/iw,h/ih):std::max(w/iw,h/ih);
    const float dw=iw*k,dh=ih*k;return {x+(w-dw)/2,y+(h-dh)/2,dw,dh};
}
// Keep the window stationary until selection crosses its left or right edge.
inline int tile_window(int first,int selected,int count,int visible=6){
    first=std::clamp(first,0,std::max(0,count-visible));
    selected=std::clamp(selected,0,std::max(0,count-1));
    if(selected<first)first=selected;
    if(selected>=first+visible)first=selected-visible+1;
    return first;
}
// Swap only at zero opacity. A late reply cannot mix old and new fields.
template<class T> struct FadeSnapshot {
    T value{};
    std::string key;
    float alpha=0;
    bool initialized=false;
    void update(const std::string& next,const T& data,float dt,bool ready=true) {
        const float step=std::clamp(dt,0.f,.1f)/.18f;
        if(!initialized){if(!ready)return;initialized=true;key=next;value=data;}
        if(next!=key) {
            alpha=std::max(0.f,alpha-step);
            if(alpha==0&&ready){key=next;value=data;}
        }else {value=data;alpha=std::min(1.f,alpha+step);}
    }
};
}
