// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <string>
#include <vector>
namespace player {
inline std::string backdrop_key(const std::string& path){return path.empty()?path:"still-blur:"+path;}
// Two separable box passes approximate a gentle Gaussian. Runs only on the art worker.
inline void soften_still(std::vector<unsigned char>& pixels,int w,int h,int radius=3){
 if(w<=0||h<=0||pixels.size()!=std::size_t(w)*h*4||radius<=0)return;
 std::vector<unsigned char> tmp(pixels.size());const int span=radius*2+1;
 for(int pass=0;pass<2;++pass){
  for(int y=0;y<h;++y)for(int c=0;c<4;++c){int sum=0;for(int d=-radius;d<=radius;++d)sum+=pixels[(std::size_t(y)*w+std::clamp(d,0,w-1))*4+c];
   for(int x=0;x<w;++x){tmp[(std::size_t(y)*w+x)*4+c]=sum/span;sum+=pixels[(std::size_t(y)*w+std::clamp(x+radius+1,0,w-1))*4+c]-pixels[(std::size_t(y)*w+std::clamp(x-radius,0,w-1))*4+c];}}
  for(int x=0;x<w;++x)for(int c=0;c<4;++c){int sum=0;for(int d=-radius;d<=radius;++d)sum+=tmp[(std::size_t(std::clamp(d,0,h-1))*w+x)*4+c];
   for(int y=0;y<h;++y){pixels[(std::size_t(y)*w+x)*4+c]=sum/span;sum+=tmp[(std::size_t(std::clamp(y+radius+1,0,h-1))*w+x)*4+c]-tmp[(std::size_t(std::clamp(y-radius,0,h-1))*w+x)*4+c];}}
 }
}
}
