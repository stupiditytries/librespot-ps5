#include "ring.hpp"
#include <cassert>
#include <cstdio>
using namespace pcm_transport;
int main(){
    std::int16_t samples[packet_frames*2],out[pcm_grain*2];
    for(unsigned i=0;i<packet_frames*2;++i)samples[i]=std::int16_t(i+1);
    Ring r;assert(r.push(samples,1024));assert(r.pop(out)==pcm_grain&&out[0]==1&&out[pcm_grain*2-1]==std::int16_t(pcm_grain*2));
    r.clear();assert(!r.size&&r.pop(out)==0);for(auto v:out)assert(!v);
    for(unsigned i=0;i<capacity/1024;++i){assert(r.push(samples,1024));}
    assert(!r.push(samples,1));
    assert(r.pop(out)==pcm_grain&&out[0]==1);assert(r.push(samples,pcm_grain));
    Receiver rx;Packet h{123,7,1000,1024,wire_magic};
    std::memcpy(rx.bytes,&h,sizeof h);std::memcpy(rx.bytes+sizeof h,samples,sizeof samples);
    rx.used=31;assert(rx.parse(123,7,1000,true)&&!rx.ring.size&&rx.used==31);
    rx.used=sizeof h+sizeof samples;assert(rx.parse(123,7,1000,true)&&rx.ring.size==1024&&!rx.used);
    auto offer=[&](Packet packet,std::uint64_t generation,std::uint64_t now,bool allowed){
        rx.clear();std::memcpy(rx.bytes,&packet,sizeof packet);std::memcpy(rx.bytes+sizeof packet,samples,sizeof samples);
        rx.used=sizeof packet+sizeof samples;return rx.parse(123,generation,now,allowed);
    };
    assert(offer(h,8,1000,true)&&!rx.ring.size); // stale seek/pause generation
    assert(offer(h,7,1600,true)&&!rx.ring.size); // old transport audio expires
    assert(offer(h,7,1000,false)&&!rx.ring.size); // paused retains no audio
    auto bad=h;bad.token=222;assert(!offer(bad,7,1000,true));
    bad=h;bad.frames=1025;assert(!offer(bad,7,1000,true));
    bad=h;bad.magic=0;assert(!offer(bad,7,1000,true));
    // Jitter buffer: silence until primed, play through, re-prime after a dry-out.
    {Ring q;Playout p;assert(q.push(samples,1024));
     assert(p.next(q,out)==0&&p.priming&&q.size==1024);for(auto v:out)assert(!v);
     while(q.size<prime_frames)assert(q.push(samples,1024));
     const unsigned primed=q.size;unsigned played=0;
     do played+=p.next(q,out); while(!p.priming);
     assert(played==primed&&p.dryouts==1&&!q.size);
     assert(q.push(samples,1024)&&p.next(q,out)==0);}
    {Receiver q;assert(q.ring.push(samples,1024));q.playout.priming=false;q.flush();assert(q.playout.priming&&!q.ring.size&&!q.playout.dryouts);}
    std::puts("PCM jitter buffer tests passed");
    std::puts("PCM IPC partial-frame, ordering, bounds, cancellation, expiry and ownership tests passed");
}
