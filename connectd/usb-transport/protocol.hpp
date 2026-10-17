// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <cstddef>
namespace usb_transport {
constexpr std::uint64_t magic=0x3154524f5053554cULL;
constexpr unsigned version=1;
#ifdef USB_TRANSPORT_TEST
constexpr const char* directory="/tmp/librespot-usb-native-tests";
constexpr const char* worker_directory=directory;
#else
constexpr const char* directory="/data/spotifyps5/usb-transport";
constexpr const char* worker_directory="/user/data/spotifyps5/usb-transport";
#endif
constexpr const char* clip_folder="/Music/Start Librespot";
constexpr const char* clip_path="/Music/Start Librespot/Librespot Streamer.flac";
enum Function:unsigned {Devices,Files,Start,Command,Current,Time,Playing,Paused,Stopped,Error,Info,FunctionCount};
enum State:int {Idle=0,Starting=1,Running=2,StoppedOk=3,Refused=-1,Failed=-2,Lost=-3};
struct Request {
    std::uint64_t signature=magic;
    std::uint32_t protocol=version,size=sizeof(Request);
    std::uint64_t build=0,token=0;
    std::int32_t shell_pid=0;
    std::uint32_t reserved=0;
    std::uint64_t shell_epoch=0,functions[FunctionCount]{};
};
struct Resident {
    std::uint64_t signature=magic,build=0;
    std::uint32_t protocol=version;
    std::int32_t shell_pid=0;
    std::uint64_t shell_epoch=0;
};
struct Heartbeat {std::uint64_t token=0,at_ms=0;};
struct CommandRequest {std::uint64_t token=0,sequence=0;std::int32_t action=0,reserved=0;};
struct Status {
    std::uint64_t signature=magic,token=0,sequence=0,at_ms=0;
    std::int32_t state=Idle,source=-1,rc=0,detail=0;
};
static_assert(sizeof(Request)==136&&sizeof(Resident)==32&&sizeof(Status)==48);
inline bool address(std::uint64_t p){return p>=0x10000&&p<0x800000000000ULL;}
inline bool valid(const Request& r,int pid,std::uint64_t build) {
    if(r.signature!=magic||r.protocol!=version||r.size!=sizeof r||!r.token||
       r.shell_pid!=pid||!r.shell_epoch||r.build!=build)return false;
    for(auto p:r.functions)if(!address(p))return false;
    return true;
}
inline bool streamer(const char* path) {
    if(!path)return false;
    if(path[0]=='u'&&path[1]=='s'&&path[2]=='b'&&path[3]==':')path+=4;
    if(*path=='/')++path;
    const char* expected=clip_path+1;
    while(*path&&*path==*expected){++path;++expected;}
    return !*path&&!*expected;
}
}
