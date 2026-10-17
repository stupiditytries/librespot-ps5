// SPDX-License-Identifier: GPL-3.0-or-later
#include "stream.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <array>

namespace usb_free {
static constexpr auto wav_header=[] {
    std::array<unsigned char,44> h{'R','I','F','F',0,0,0,0,'W','A','V','E',
        'f','m','t',' ',16,0,0,0,1,0,2,0,0,0,0,0,0,0,0,0,4,0,16,0,'d','a','t','a'};
    auto put=[&](unsigned at,std::uint32_t value){for(unsigned i=0;i<4;++i)h[at+i]=(value>>(8*i))&255;};
    put(4,audio_bytes-8);put(24,sample_rate);
    put(28,sample_rate*channels*bytes_per_sample);put(40,pcm_bytes);
    return h;
}();
bool fill_audio(std::uint64_t offset,unsigned char* output,std::size_t count) {
    if(offset>audio_bytes||count>audio_bytes-offset)return false;
    if(!count)return true;
    std::memset(output,0,count);
    if(offset<wav_header.size()) {
        const auto n=std::min(count,wav_header.size()-std::size_t(offset));
        std::memcpy(output,wav_header.data()+offset,n);
    }
    return true;
}
static bool number(const char*& p,std::uint64_t& n) {
    if(*p<'0'||*p>'9') return false;
    n=0;
    while(*p>='0'&&*p<='9') {
        const unsigned d=unsigned(*p++-'0');
        if(n>(UINT64_MAX-d)/10) return false;
        n=n*10+d;
    }
    return true;
}
HttpResult request_range(const char* input,std::uint64_t token,Range& range,bool& head) {
    range={};head=false;
    char method[8]{},path[96]{},http[16]{};
    if(std::sscanf(input,"%7s %95s %15s",method,path,http)!=3||
       (std::strcmp(http,"HTTP/1.1")&&std::strcmp(http,"HTTP/1.0"))) return HttpBad;
    head=std::strcmp(method,"HEAD")==0;
    if(!head&&std::strcmp(method,"GET")) return HttpMethod;
    char wanted[96];std::snprintf(wanted,sizeof wanted,"/probe-%016llx.wav",(unsigned long long)token);
    if(std::strcmp(path,wanted)) return HttpNotFound;
    const char* p=std::strstr(input,"\r\n");if(!p)return HttpBad;
    bool seen=false;
    while(p&&p[2]&&p[2]!='\r') {
        p+=2;const auto end=std::strstr(p,"\r\n");if(!end)return HttpBad;
        char key[7]{};
        if(end-p>=6) for(unsigned i=0;i<6;++i) key[i]=char(std::tolower(static_cast<unsigned char>(p[i])));
        if(!std::strcmp(key,"range:")) {
            if(seen)return HttpRange;
            seen=true;
            const char* v=p+6;while(*v==' '||*v=='\t')++v;
            if(std::strncmp(v,"bytes=",6))return HttpRange;
            v+=6;
            if(*v=='-') {
                ++v;std::uint64_t suffix;
                if(!number(v,suffix)||!suffix)return HttpRange;
                range.first=audio_bytes-std::min(suffix,audio_bytes);
            } else {
                if(!number(v,range.first)||*v++!='-')return HttpRange;
                if(*v>='0'&&*v<='9'&&!number(v,range.last))return HttpRange;
            }
            while(v<end&&(*v==' '||*v=='\t'))++v;
            if(v!=end||range.first>=audio_bytes||range.last<range.first)return HttpRange;
            range.last=std::min(range.last,audio_bytes-1);range.partial=true;
        }
        p=end;
    }
    return HttpOk;
}
}
