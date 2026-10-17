// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "protocol.hpp"
#include "migration.hpp"
#include <cstring>
#include <cstdio>

namespace usb_free {
// Source/callback shapes from the previously console-tested HTTP spike.
struct Source {
    unsigned char bytes[0x2980]{};
    explicit Source(std::uint64_t token) {
        std::snprintf(reinterpret_cast<char*>(bytes),0x1000,
            "http://127.0.0.1:%u/probe-%016llx.wav",port,(unsigned long long)token);
        std::snprintf(reinterpret_cast<char*>(bytes+0x1000),0x800,
            "audio/wav;corename=\"SystemMusicCore\";titleid=\"NPXS40087\";trackindex=0;");
        // Generic PlayUri's constructor at 0x5d6e..0x5da7: audio, Element,
        // previous-active/pause-inactive, BGM suppression On (all other words 0).
        // V1/v2 inherited the USB helper's behavior defaults (3/2) instead.
        std::int32_t kind=2;
        std::memcpy(bytes+0x2948,&kind,4);
    }
};
struct Callbacks {
    void (*function)(std::uint64_t,std::uint64_t,std::uint64_t,std::uint64_t)=nullptr;
    void* user_data=nullptr;
    std::uint64_t count=4;
};
struct Media {
    int (*add)(const void*,const void*,std::int32_t*)=nullptr;
    int (*play)(std::int32_t)=nullptr;
    int (*stop)(std::int32_t)=nullptr;
    int (*remove)(std::int32_t)=nullptr;
    int (*time)(std::int32_t,double*)=nullptr;
    int (*current)(int)=nullptr;
    bool (*is_playing)(std::int32_t)=nullptr;
    int (*pause)(std::int32_t)=nullptr;
    int (*bgm)()=nullptr;
    int (*query_playing)(std::int32_t,bool*)=nullptr;
    int (*query_paused)(std::int32_t,bool*)=nullptr;
    int (*query_stopped)(std::int32_t,bool*)=nullptr;
    int (*error)(std::int32_t,int*,int*)=nullptr;
};
struct SourceState {
    int playing_rc=-1,paused_rc=-1,stopped_rc=-1;
    bool queried=false,playing=false,paused=false,stopped=false;
    bool inactive() const {
        return queried&&!playing_rc&&!paused_rc&&!stopped_rc&&!playing&&(paused||stopped);
    }
};
struct Events { std::uint32_t play=0,error=0,time=0,ended=0; };
// All hooks below belong to the test, never to a ShellUI UI/input hook.
struct Runtime {
    void* context;
    std::uint64_t (*now)(void*);
    void (*sleep)(void*,unsigned);
    // Returns UserStop or ParentLost when the run must end, otherwise None.
    Reason (*abort)(void*);
    Events (*events)(void*);
    void (*publish)(void*,const Status&);
    void (*observe_source)(void*,int service,int source,const SourceState&,bool blocked)=nullptr;
    void (*observe_prior)(void*,int source,int error_rc,bool gone)=nullptr;
    bool (*prepare_mode)(void*)=nullptr;
    bool (*restore_mode)(void*)=nullptr;
};
// Owns exactly the returned source. Does not initialize/finalize MediaFramework,
// stop other sources, capture input, or call SetCurrentSrcId.
Status exercise(const Request&,const Media&,const Callbacks&,const Runtime&,const PriorSources& = PriorSources{});
}
