// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdio>
#include <cstring>
#include <ctime>

namespace pcm_transport {
inline std::uint64_t now_ms() {
    timespec t{};clock_gettime(CLOCK_MONOTONIC,&t);
    return std::uint64_t(t.tv_sec)*1000+t.tv_nsec/1000000;
}
inline void path(char* out,std::size_t size,const char* base,const char* name) {
    std::snprintf(out,size,"%s/%s",base,name);
}
template<class T> bool read_file(const char* base,const char* name,T& result) {
    char p[256];path(p,sizeof p,base,name);FILE* f=std::fopen(p,"rb");if(!f)return false;
    T value{};bool ok=std::fread(&value,1,sizeof value,f)==sizeof value&&std::fgetc(f)==EOF;
    std::fclose(f);if(ok)result=value;return ok;
}
template<class T> bool write_file(const char* base,const char* name,const T& value) {
    char p[256],temp[272];path(p,sizeof p,base,name);std::snprintf(temp,sizeof temp,"%s.tmp",p);
    FILE* f=std::fopen(temp,"wb");if(!f)return false;
    bool ok=std::fwrite(&value,1,sizeof value,f)==sizeof value;
    ok=std::fclose(f)==0&&ok;return ok&&std::rename(temp,p)==0;
}
}
