// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "protocol.hpp"
#include "ownership/media.hpp"
namespace pcm_transport {
// Firmware 13.20 RNPS Media GetCurrentSrcId returns this initialized source
// value for an empty service slot. It is not a MediaFramework IPC error.
constexpr int native_empty_source=-1000;
enum BootstrapPhase:int {BootIdle=0,BootLoaded=1,BootClean=2,BootFailed=-1};
struct BootstrapCommand {std::uint64_t token=0;std::int32_t action=0,reserved=0;};
struct BootstrapStatus {
    std::uint64_t signature=magic,token=0,at_ms=0;
    std::int32_t phase=BootIdle,source=-1,rc=0,clean=0;
};
inline bool bootstrap_clean(const BootstrapStatus& s,std::uint64_t token) {
    return s.signature==magic&&s.token==token&&s.source==-1&&s.clean==1&&
        (s.phase==BootClean||s.phase==BootFailed);
}
// This deliberately cannot call Play, AudioOut, or a policy setter.
struct BootstrapApi {
    int (*add)(const void*,const void*,std::int32_t*);
    int (*load)(int);
    int (*stop)(int);
    int (*remove)(int);
    int (*current)(int);
    int (*playing)(int,bool*);
    int (*paused)(int,bool*);
    int (*stopped)(int,bool*);
    int (*error)(int,int*,int*);
};
struct BootstrapClock {
    std::uint64_t (*now)();
    void (*sleep)(unsigned);
};
class Bootstrap {
    BootstrapApi api;
    BootstrapClock clock;
    void (*log)(const char*,...);
    std::uint64_t began=0;
    int baseline[20]{};
    bool available(bool capture) {
        for(int service=1;service<=20;++service) {
            const int id=api.current(service);
            if(id==native_empty_source||id==0||id==-1) {
                if(capture)baseline[service-1]=0;
                continue;
            }
            if(id<0){log("BOOTSTRAP REFUSED source-table service=%d value=%d",service,id);return false;}
            if(id==status.source)continue;
            bool playing=false,paused=false,stopped=false;
            const int a=api.playing(id,&playing),b=api.paused(id,&paused),c=api.stopped(id,&stopped);
            if(a||b||c||playing||(!paused&&!stopped)||(!capture&&baseline[service-1]!=id)) {
                log("BOOTSTRAP REFUSED foreign service=%d source=%d query=%08x/%08x/%08x playing=%u paused=%u stopped=%u",
                    service,id,a,b,c,unsigned(playing),unsigned(paused),unsigned(stopped));
                return false;
            }
            if(capture)baseline[service-1]=id;
        }
        return true;
    }
    bool absent(int id) {
        bool p=false,h=false,s=false;int error=0,detail=0;
        if(!usb_free::absent(api.playing(id,&p),api.paused(id,&h),api.stopped(id,&s),api.error(id,&error,&detail)))return false;
        for(int service=1;service<=20;++service)if(api.current(service)==id)return false;
        return true;
    }
    void finish(bool failed) {
        bool clean=true;
        if(status.source>0) {
            const int id=status.source,stop=api.stop(id);
            bool stopped=false;const auto at=clock.now();
            do {
                bool p=false,h=false,s=false;
                if(!api.playing(id,&p)&&!api.paused(id,&h)&&!api.stopped(id,&s)&&!p&&!h&&s){stopped=true;break;}
                clock.sleep(50);
            }while(clock.now()-at<2000);
            const int removed=api.remove(id);bool gone=false;const auto removal=clock.now();
            do {if(absent(id)){gone=true;break;}clock.sleep(50);}while(clock.now()-removal<2000);
            clean=!stop&&stopped&&!removed&&gone;
            log("BOOTSTRAP cleanup source=%d stop=%08x stopped=%u remove=%08x native_absence=%u",
                id,stop,unsigned(stopped),removed,unsigned(gone));
            if(clean)status.source=-1;
            else if(!status.rc)status.rc=stop?stop:removed?removed:-1;
        }
        status.clean=clean;
        status.phase=failed||!clean?BootFailed:BootClean;
        status.at_ms=clock.now();
    }
public:
    BootstrapStatus status;
    Bootstrap(BootstrapApi a,BootstrapClock c,void(*logger)(const char*,...)):api(a),clock(c),log(logger){}
    bool busy() const {return status.phase==BootLoaded;}
    bool terminal() const {return status.phase==BootClean||status.phase==BootFailed;}
    void begin(std::uint64_t token,const usb_free::Callbacks& callbacks) {
        if(status.token)return;
        status.token=token;began=clock.now();status.at_ms=began;
        if(!available(true)){status.rc=-2;finish(true);return;}
        usb_free::Source source(token);int id=-1;
        status.rc=api.add(source.bytes,&callbacks,&id);
        if(id>0)status.source=id; // Clean even an anomalous Add failure with an ID.
        log("BOOTSTRAP add source=%d rc=%08x; no Play/AudioOut/policy calls",id,status.rc);
        if(status.rc||id<=0){if(!status.rc)status.rc=-3;finish(true);return;}
        if(!available(false)){status.rc=-4;finish(true);return;}
        status.rc=api.load(id);
        log("BOOTSTRAP load source=%d rc=%08x",id,status.rc);
        if(status.rc){finish(true);return;}
        status.phase=BootLoaded;status.at_ms=clock.now();
    }
    void tick(bool parent_fresh,bool finish_requested) {
        if(!busy())return;
        if(!parent_fresh||clock.now()-began>=10000||!available(false)) {
            status.rc=-5;finish(true);
        }else if(finish_requested)finish(false);
        status.at_ms=clock.now();
    }
};
}
