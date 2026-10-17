// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
namespace core_state {
struct Fingerprint {std::uint64_t offset,hash;};
constexpr std::uint64_t hash(const unsigned char* bytes,std::size_t length) {
    std::uint64_t h=14695981039346656037ULL;
    for(std::size_t i=0;i<length;++i){h^=bytes[i];h*=1099511628211ULL;}
    return h;
}
inline bool address(std::uint64_t base,std::uint64_t offset,std::size_t length) {
    return base>=0x10000&&base<0x800000000000ULL&&offset<0x800000000000ULL-base&&
        length<=0x800000000000ULL-base-offset;
}
template<class Reader> bool matches(std::uint64_t base,const Fingerprint* checks,std::size_t count,Reader read) {
    if(count!=3)return false;
    unsigned char bytes[32];
    for(std::size_t i=0;i<count;++i)
        if(!address(base,checks[i].offset,sizeof bytes)||
           read(base+checks[i].offset,bytes,sizeof bytes)||hash(bytes,sizeof bytes)!=checks[i].hash)return false;
    return true;
}
}
