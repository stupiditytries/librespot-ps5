// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "protocol.hpp"
#include <cstring>
namespace usb_free {
// Firmware 13.20: cached GetMediaError and server-backed IsPlaying/Paused/
// Stopped return this exact code for an invalid/missing positive source ID.
// Ordinary network, initialization and IPC errors do not count as absence.
constexpr int missing_source=static_cast<int>(0x81790003U);
struct PriorSource {std::uint64_t build=0,token=0;std::int32_t source=-1;std::uint32_t protocol=0;};
struct PriorSources {
    std::uint64_t signature=magic,token=0;
    std::uint32_t protocol=version,count=0;
    PriorSource entries[8]{};
};
static_assert(sizeof(PriorSources)==216);
inline bool valid(const PriorSources& p,std::uint64_t token) {
    if(p.signature!=magic||p.protocol!=version||p.token!=token||p.count>8)return false;
    for(unsigned i=0;i<p.count;++i)
        if(!p.entries[i].build||!p.entries[i].token||p.entries[i].source<=0)return false;
    return true;
}
inline bool prior_identity(const Request& q,const Resident& r,int pid,std::uint64_t epoch) {
    const auto n=q.protocol==1?9U:10U;
    if(q.protocol<1||q.protocol>3||q.signature!=magic||r.signature!=magic||
       r.protocol!=q.protocol||q.size!=48+n*8||!q.token||q.seconds<5||q.seconds>run_seconds||
       r.build!=q.build||!q.build||r.shell_pid!=pid||q.shell_pid!=pid||
       r.shell_epoch!=epoch||q.shell_epoch!=epoch)return false;
    for(unsigned i=0;i<n;++i)if(!address(q.functions[i]))return false;
    return true;
}
inline bool prior_terminal(const Request& q,const Resident& r,const Status& s,int pid,std::uint64_t epoch) {
    if(!prior_identity(q,r,pid,epoch)||s.signature!=magic||s.protocol!=q.protocol||s.token!=q.token)return false;
    if(s.phase!=Refused&&s.phase!=Complete&&s.phase!=Failed&&s.phase!=CleanupFailed&&
       !(q.protocol>=3&&s.phase==Retired))return false;
    if(s.source<=0)return s.source==-1&&!s.stop_rc&&!s.remove_rc&&(s.phase==Refused||s.reason==AddFailed||s.phase==Retired);
    // CleanupFailed is admitted only for the known table-notification case.
    // Live queries still have to prove this ID is gone before any AddSrc.
    return !s.add_rc&&!s.stop_rc&&!s.remove_rc&&s.stop_confirmed;
}
inline bool absent(int playing_rc,int paused_rc,int stopped_rc,int error_rc) {
    return playing_rc==missing_source&&paused_rc==missing_source&&
        stopped_rc==missing_source&&error_rc==missing_source;
}
struct Retirement {
    std::uint64_t signature=magic,build=0,token=0,shell_epoch=0;
    std::int32_t shell_pid=0,source=-1;
    std::uint32_t protocol=version,cleanup_recorded=0;
};
static_assert(sizeof(Retirement)==48);
}
