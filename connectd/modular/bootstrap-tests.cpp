// SPDX-License-Identifier: GPL-3.0-or-later
#include "protocol.hpp"
#include "bootstrap.hpp"
#include <cassert>
#include <cstdio>
using namespace pcm_transport;
static std::uint64_t at;
static int sources[21],adds,loads,stops,removes,add_rc,load_rc,stop_rc,remove_rc,queries_rc,foreign;
static bool gone,ended,table_sticks;
static constexpr int missing=usb_free::missing_source;
static std::uint64_t now(){return at;}
static void sleep_ms(unsigned ms){at+=ms;}
static void log(const char*,...){}
static void event(std::uint64_t,std::uint64_t,std::uint64_t,std::uint64_t){}
static usb_free::Callbacks callbacks{event,nullptr,4};
static int current(int service){return sources[service];}
static int playing(int id,bool* value){*value=id==foreign;return id==42&&gone?missing:queries_rc;}
static int paused(int id,bool* value){*value=false;return id==42&&gone?missing:queries_rc;}
static int stopped(int id,bool* value){*value=id!=42||ended;return id==42&&gone?missing:queries_rc;}
static int error(int id,int* a,int* b){*a=*b=0;return id==42&&gone?missing:queries_rc;}
static int add(const void* source,const void* cb,int* id){
    assert(!std::strncmp(static_cast<const char*>(source),"http://127.0.0.1:",17));
    assert(cb==&callbacks);++adds;*id=42;gone=false;return add_rc;
}
static int load(int id){assert(id==42);++loads;return load_rc;}
static int stop(int id){assert(id==42);++stops;ended=true;return stop_rc;}
static int remove(int id){
    assert(id==42);++removes;
    if(!remove_rc){gone=true;if(!table_sticks)for(int& source:sources)if(source==42)source=0;}
    return remove_rc;
}
static Bootstrap make(){
    at=100;sources[0]=0;
    for(int& source:sources)source=native_empty_source;
    adds=loads=stops=removes=add_rc=load_rc=stop_rc=remove_rc=queries_rc=foreign=0;
    gone=ended=table_sticks=false;
    return Bootstrap({add,load,stop,remove,current,playing,paused,stopped,error},{now,sleep_ms},log);
}
int main(){
    auto b=make();b.begin(123,callbacks);assert(b.busy()&&adds==1&&loads==1&&!stops&&!removes);
    b.tick(true,true);assert(bootstrap_clean(b.status,123)&&stops==1&&removes==1);
    b.begin(123,callbacks);assert(adds==1); // No request replay.
    b=make();sources[1]=80;foreign=80;b.begin(123,callbacks);
    assert(b.status.phase==BootFailed&&b.status.clean&&!adds&&!loads&&!stops&&!removes);
    b=make();sources[1]=80;b.begin(123,callbacks);assert(b.busy()&&sources[1]==80);b.tick(true,true);assert(sources[1]==80);
    b=make();sources[1]=0;sources[2]=-1;b.begin(123,callbacks);assert(b.busy());b.tick(true,true);assert(bootstrap_clean(b.status,123));
    b=make();sources[2]=-7;b.begin(123,callbacks);assert(!adds&&bootstrap_clean(b.status,123));
    b=make();add_rc=-6;b.begin(123,callbacks);
    assert(b.status.phase==BootFailed&&bootstrap_clean(b.status,123)&&stops==1&&removes==1&&!loads);
    b=make();load_rc=-7;b.begin(123,callbacks);
    assert(b.status.phase==BootFailed&&bootstrap_clean(b.status,123)&&stops==1&&removes==1);
    b=make();b.begin(123,callbacks);b.tick(false,false);assert(bootstrap_clean(b.status,123)&&b.status.phase==BootFailed);
    b=make();b.begin(123,callbacks);at+=10000;b.tick(true,false);assert(bootstrap_clean(b.status,123)&&b.status.phase==BootFailed);
    b=make();b.begin(123,callbacks);sources[3]=80;foreign=80;b.tick(true,false);
    assert(bootstrap_clean(b.status,123)&&b.status.phase==BootFailed&&sources[3]==80);
    b=make();b.begin(123,callbacks);remove_rc=-8;b.tick(true,true);assert(!b.status.clean&&b.status.source==42&&!bootstrap_clean(b.status,123));
    b=make();b.begin(123,callbacks);stop_rc=-9;b.tick(true,true);assert(!b.status.clean&&b.status.phase==BootFailed);
    b=make();b.begin(123,callbacks);sources[1]=42;table_sticks=true;b.tick(true,true);
    assert(!b.status.clean&&b.status.source==42&&at>=2100);
    b=make();b.begin(123,callbacks);queries_rc=-10;b.tick(true,true);assert(!b.status.clean&&at>=2100);
    b=make();b.begin(123,callbacks);b.tick(true,true);auto stale=b.status;
    stale.signature^=1;assert(!bootstrap_clean(stale,123));assert(!bootstrap_clean(b.status,456));
    std::puts("PASS cold bootstrap: non-playing Add/Load, firmware -1000/0/-1 empty slots, active/unknown foreign refusal, source-preserving cleanup, Add/Load failure, parent loss, timeout, exact absence and poisoned cleanup");
}
