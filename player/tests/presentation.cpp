#include "presentation.hpp"
#include "still_blur.hpp"
#include <cassert>
int main(){
 auto square=player::fitted_image(1080,1080,0,0,1920,1080,true);
 assert(square.x==420&&square.y==0&&square.w==1080&&square.h==1080);
 auto portrait=player::fitted_image(600,1200,0,0,1920,1080,true);
 assert(portrait.w==540&&portrait.h==1080&&portrait.x==690);
 auto wide=player::fitted_image(1920,1080,0,0,1920,1080,true);
 assert(wide.x==0&&wide.y==0&&wide.w==1920&&wide.h==1080);
 auto crop=player::fitted_image(1080,1080,0,0,1920,1080,false);
 assert(crop.w==1920&&crop.h==1920&&crop.y==-420);
 std::vector<unsigned char> solid(7*5*4,117);player::soften_still(solid,7,5);for(auto c:solid)assert(c==117);
 std::vector<unsigned char> impulse(9*9*4,0);for(int c=0;c<4;++c)impulse[(4*9+4)*4+c]=255;
 player::soften_still(impulse,9,9,2);assert(impulse[(4*9+4)*4]>0&&impulse[(4*9+4)*4]<255);assert(impulse[(4*9+3)*4]>0);
 assert(player::backdrop_key("cover")=="still-blur:cover"&&player::backdrop_key("").empty());
 assert(player::tile_window(0,5,20)==0);
 assert(player::tile_window(0,6,20)==1);
 assert(player::tile_window(1,5,20)==1); // reversing direction moves focus, not the row
 assert(player::tile_window(1,1,20)==1);
 assert(player::tile_window(1,0,20)==0);
 assert(player::tile_window(99,1,3)==0);
 player::FadeSnapshot<int> f;f.update("a",1,.1f);f.update("a",1,.1f);assert(f.value==1&&f.alpha==1);
 f.update("b",2,.05f);assert(f.value==1&&f.alpha>0&&f.alpha<1);
 f.update("c",3,.1f,false);f.update("c",3,.1f,false);assert(f.alpha==0&&f.value==1);
 f.update("c",3,.1f,true);assert(f.alpha==0&&f.value==3);f.update("c",3,.1f);assert(f.alpha>0);
}
