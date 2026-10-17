// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "feature.hpp"
#include "process-records.hpp"
#include "guard.hpp"
#include "build/mode-firmware.inc"
#include <ps5/kernel.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <cerrno>
struct FeatureAppInfo {std::uint32_t app_id;std::uint64_t unknown;char title_id[14],reserved[0x3c];};
extern "C" int sceKernelGetAppInfo(pid_t,FeatureAppInfo*);
namespace usb_free {
struct ModeProcess {int pid=0;std::uint64_t epoch=0,base=0;};
class ModeContext {
    using Log=void(*)(const char*,...);
    Log log;
    ModeProcess core,server;
    std::uint64_t manager=0,slot=0,application=0;
    std::int32_t app_id=0;
    mutable const char* failed_stage=nullptr;
    mutable unsigned verified_processes=0;
    bool cold_missing=false;
    bool failed(const char* stage) const {
        if(!failed_stage||std::strcmp(failed_stage,stage))log("AUDIOTAG CHECK failed stage=%s",stage);
        failed_stage=stage;return false;
    }
    bool alive(const ModeProcess& p) const {
        int mib[]={CTL_KERN,KERN_PROC,KERN_PROC_PID,p.pid};std::size_t n=0;
        for(unsigned attempt=0;attempt<3;++attempt) {
            const int size_rc=sysctl(mib,4,nullptr,&n,nullptr,0);
            if(size_rc||!n||n>16*1024*1024) {
                log("AUDIOTAG PROCESS size_query pid=%d rc=%d errno=%d bytes=%zu SDK_record=%zu",p.pid,size_rc,errno,n,sizeof(kinfo_proc));return false;
            }
            auto* bytes=static_cast<unsigned char*>(std::malloc(n));if(!bytes)return false;
            const int rc=sysctl(mib,4,bytes,&n,nullptr,0),error=errno;
            const bool match=!rc&&process_records_match<kinfo_proc>(bytes,n,p.pid,p.epoch);
            const unsigned process_bit=p.pid==core.pid?1:2;
            if(match&&!(verified_processes&process_bit)) {
                kinfo_proc first{};std::memcpy(&first,bytes,sizeof first);
                log("AUDIOTAG PROCESS verified pid=%d bytes=%zu SDK_record=%zu stride=%d epoch=%llu",p.pid,n,sizeof first,
                    first.ki_structsize,static_cast<unsigned long long>(p.epoch));
                verified_processes|=process_bit;
            }
            if(!rc&&!match) {
                kinfo_proc first{};if(n>=sizeof first)std::memcpy(&first,bytes,sizeof first);
                log("AUDIOTAG PROCESS identity pid=%d bytes=%zu SDK_record=%zu stride=%d returned_pid=%d epoch=%llu expected=%llu",
                    p.pid,n,sizeof first,first.ki_structsize,first.ki_pid,
                    static_cast<unsigned long long>(first.ki_start.tv_sec)*1000000+first.ki_start.tv_usec,
                    static_cast<unsigned long long>(p.epoch));
            }
            std::free(bytes);
            if(match)return true;
            if(!rc||error!=ENOMEM) {
                if(rc)log("AUDIOTAG PROCESS read pid=%d rc=%d errno=%d bytes=%zu",p.pid,rc,error,n);
                return false;
            }
        }
        return false;
    }
    template<class T> static bool read(const ModeProcess& p,std::uint64_t at,T& value) {
        return core_state::address(at,0,sizeof value)&&!kernel_proc_copyout(p.pid,at,&value,sizeof value);
    }
    static bool identify(const kinfo_proc& p,const core_state::Fingerprint* checks,std::uint64_t entry,ModeProcess& out) {
        auto reader=[&](std::uint64_t at,void* bytes,std::size_t n){return kernel_proc_copyout(p.ki_pid,at,bytes,n);};
        // The live reader identified the core's main image as handle 0. Try
        // both main-image conventions, accepting only exact code fingerprints.
        for(std::uint32_t handle=0;handle<2;++handle) {
            const auto mapped=kernel_dynlib_mapbase_addr(p.ki_pid,handle);
            const auto start=kernel_dynlib_entry_addr(p.ki_pid,handle);
            const std::uint64_t bases[]={std::uint64_t(mapped),start>std::intptr_t(entry)?std::uint64_t(start)-entry:0};
            for(auto base:bases)if(core_state::matches(base,checks,3,reader)) {
                out={p.ki_pid,std::uint64_t(p.ki_start.tv_sec)*1000000+p.ki_start.tv_usec,base};return true;
            }
        }
        return false;
    }
public:
    explicit ModeContext(Log logger):log(logger){}
    bool needs_bootstrap() const {return cold_missing;}
    bool sample(int& feature,int& mode) const {
        std::uint64_t current_manager=0,current_app=0;std::int32_t id=0;unsigned char initialized=0;
        if(!alive(core))return failed("core_process_identity");
        if(!alive(server))return failed("server_process_identity");
        if(!read(server,server.base+0x1f72e0,current_manager)||current_manager!=manager)return failed("application_manager");
        if(!read(server,slot,current_app)||current_app!=application)return failed("application_slot");
        if(!read(server,application,id)||id!=app_id)return failed("application_identity");
        if(!read(server,application+0x54,feature))return failed("application_policy_read");
        if(!read(core,core.base+0x9c1c0,initialized)||!initialized)return failed("core_initialized");
        if(!read(core,core.base+0x9c1b0,mode))return failed("core_mode_read");
        std::uint64_t again=0;
        if(!read(server,slot,again)||again!=application)return failed("application_slot_changed");
        if(feature<0||feature>2||mode<0||mode>1)return failed("policy_mode_range");
        failed_stage=nullptr;return true;
    }
    bool open(int shell_pid,FeaturePlan& plan) {
        cold_missing=false;core={};server={};manager=slot=application=0;verified_processes=0;
        bool unknown_core=false;
        FeatureAppInfo info{};
        if(sceKernelGetAppInfo(shell_pid,&info)){log("AUDIOTAG REFUSED ShellUI application info unavailable");return false;}
        app_id=static_cast<std::int32_t>(info.app_id);
        log("AUDIOTAG identity shell_pid=%d app_id=%08x title=%.14s",shell_pid,info.app_id,info.title_id);
        int mib[]={CTL_KERN,KERN_PROC,KERN_PROC_PROC,0};std::size_t n=0;
        if(sysctl(mib,4,nullptr,&n,nullptr,0)||!n||n>16*1024*1024)return false;
        auto* bytes=static_cast<unsigned char*>(std::malloc(n));if(!bytes)return false;
        if(!sysctl(mib,4,bytes,&n,nullptr,0))for(std::size_t at=0;at+sizeof(kinfo_proc)<=n;) {
            auto* p=reinterpret_cast<const kinfo_proc*>(bytes+at);
            if(p->ki_structsize<int(sizeof(kinfo_proc))||std::size_t(p->ki_structsize)>n-at)break;
            if(std::strstr(p->ki_comm,"Music")||std::strstr(p->ki_comm,"BgmPlayer")) {
                if(!identify(*p,core_checks,core_entry,core))unknown_core=true;
            }
            if(std::strstr(p->ki_comm,"MediaCore"))
                (void)identify(*p,server_checks,server_entry,server);
            at+=p->ki_structsize;
        }
        std::free(bytes);
        log("AUDIOTAG firmware core_pid=%d core_base=%llx server_pid=%d server_base=%llx",core.pid,
            static_cast<unsigned long long>(core.base),server.pid,static_cast<unsigned long long>(server.base));
        cold_missing=!core.pid&&!unknown_core&&server.pid;
        if(!core.pid||!server.pid||!read(server,server.base+0x1f72e0,manager)||!core_state::address(manager,0,0x158)) {
            log("AUDIOTAG REFUSED matching core/MCS/application manager unavailable");return false;
        }
        unsigned found=0;
        for(unsigned i=0;i<20;++i) {
            const auto candidate_slot=manager+0x18+16*i;std::uint64_t candidate=0;std::int32_t id=0;
            if(!read(server,candidate_slot,candidate))return false;
            if(candidate&&read(server,candidate,id)&&id==app_id) {slot=candidate_slot;application=candidate;++found;}
        }
        if(found!=1){log("AUDIOTAG REFUSED application context matches=%u",found);return false;}
        log("AUDIOTAG application context matched app_id=%08x",app_id);
        int feature=-1,mode=-1;
        if(!sample(feature,mode)||mode!=int(feature!=0)) {
            log("AUDIOTAG REFUSED sample/mode mismatch original_enum=%d core_mode=%d",feature,mode);return false;
        }
        usleep(100000);int again=-1,again_mode=-1;
        if(!sample(again,again_mode)||again!=feature||again_mode!=mode) {
            log("AUDIOTAG REFUSED unstable feature/mode %d/%d -> %d/%d",feature,mode,again,again_mode);return false;
        }
        plan.original=feature;plan.original_mode=mode;plan.app_id=app_id;return true;
    }
};
}
