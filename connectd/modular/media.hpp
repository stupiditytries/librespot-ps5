// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "protocol.hpp"
namespace pcm_transport {
struct Api {
    int (*add)(const void*,const void*,std::int32_t*);
    int (*play)(std::int32_t);
    int (*stop)(std::int32_t);
    int (*remove)(std::int32_t);
    int (*current)(int);
    int (*time)(std::int32_t,double*);
    int (*playing)(std::int32_t,bool*);
    int (*paused)(std::int32_t,bool*);
    int (*stopped)(std::int32_t,bool*);
    int (*error)(std::int32_t,int*,int*);
    int (*pause)(std::int32_t);
    int (*open)(int,int,int,unsigned,unsigned,unsigned);
    int (*output)(int,const void*);
    int (*close)(int);
    int (*volume)(int,int,const int*);
    int (*port_state)(int,void*);
    int (*last_time)(int,std::uint64_t*);
    int (*init)();
    int (*seek)(std::int32_t,double);
};
}
