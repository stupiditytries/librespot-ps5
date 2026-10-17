// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <cstring>
#include <algorithm>
namespace pcm_transport {
// 1024-frame AudioOut grains: about 21 ms of slack per Output call, not 5 ms.
constexpr unsigned pcm_rate=48000,pcm_grain=1024,capacity=8192,packet_frames=1024;
// Playback waits for prime_frames (100 ms) before starting and after running
// dry; the sender may run lead_frames (200 ms) ahead of real time. v4 kept
// about 40-80 ms and stuttered whenever decoding hiccuped.
constexpr unsigned prime_frames=4800,lead_frames=9600;
constexpr std::uint32_t wire_magic=0x314d4350;
struct Packet {std::uint64_t token,generation,at_ms;std::uint32_t frames,magic;};
static_assert(sizeof(Packet)==32);
struct Ring {
    std::int16_t samples[capacity*2]{};
    unsigned read=0,size=0;
    void clear(){read=size=0;}
    bool push(const void* pcm,unsigned frames) {
        if(frames>capacity-size)return false;
        auto* bytes=static_cast<const unsigned char*>(pcm);
        for(unsigned i=0;i<frames;++i)std::memcpy(samples+((read+size+i)%capacity)*2,bytes+i*4,4);
        size+=frames;return true;
    }
    unsigned pop(std::int16_t* out) {
        unsigned n=std::min(size,pcm_grain);
        for(unsigned i=0;i<n;++i){out[i*2]=samples[((read+i)%capacity)*2];out[i*2+1]=samples[((read+i)%capacity)*2+1];}
        std::memset(out+n*2,0,(pcm_grain-n)*4);read=(read+n)%capacity;size-=n;return n;
    }
};
// Jitter buffer: one short gap after a late producer instead of stutter.
struct Playout {
    bool priming=true;std::uint64_t dryouts=0;
    unsigned next(Ring& ring,std::int16_t* out) {
        if(priming&&ring.size<prime_frames){std::memset(out,0,pcm_grain*4);return 0;}
        priming=false;
        const unsigned n=ring.pop(out);
        if(n<pcm_grain){priming=true;++dryouts;}
        return n;
    }
};
struct Receiver {
    unsigned char bytes[16384]{};unsigned used=0;
    std::uint64_t accepted_frames=0,discarded_frames=0;
    Ring ring;Playout playout;
    // Drop queued audio (gate or seek). Framing of partly received bytes is kept.
    void flush(){ring.clear();playout.priming=true;}
    void clear(){used=0;flush();}
    bool parse(std::uint64_t token,std::uint64_t generation,std::uint64_t now,bool allowed) {
        while(used>=sizeof(Packet)) {
            Packet h{};std::memcpy(&h,bytes,sizeof h);
            if(h.magic!=wire_magic||h.frames>packet_frames||!h.frames||h.token!=token)return false;
            const unsigned length=sizeof h+h.frames*4;
            if(used<length)break;
            const bool take=allowed&&h.generation==generation&&h.at_ms<=now+1000&&now<=h.at_ms+500;
            if(take&&!ring.push(bytes+sizeof h,h.frames))break;
            if(take)accepted_frames+=h.frames;else discarded_frames+=h.frames;
            used-=length;std::memmove(bytes,bytes+length,used);
        }
        return true;
    }
};
}
