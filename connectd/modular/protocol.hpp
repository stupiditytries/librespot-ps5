// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <cstddef>
namespace pcm_transport {
constexpr std::uint64_t magic=0x3d4d43505053534cULL;
constexpr unsigned version=1;
#ifdef PCM_TRANSPORT_TEST
constexpr const char* directory="/tmp/librespot-pcm-native-tests";
constexpr const char* worker_directory=directory;
#else
constexpr const char* directory="/data/spotifyps5/pcm-transport-v12";
constexpr const char* worker_directory="/user/data/spotifyps5/pcm-transport-v12";
#endif
enum Function:unsigned {Add,Play,Stop,Remove,Current,Time,Playing,Paused,Stopped,Error,Pause,Open,Output,Close,Volume,PortState,LastTime,Init,Seek,Load,FunctionCount};
enum State:int {Idle=0,Starting=1,Running=2,StoppedOk=3,ServicePaused=4,Pausing=5,ConsoleResumed=6,ConsolePaused=7,Refused=-1,Failed=-2,Lost=-3};
struct Request {
    std::uint64_t signature=magic;
    std::uint32_t protocol=version,size=sizeof(Request);
    std::uint64_t build=0,token=0;
    std::int32_t shell_pid=0;
    std::uint32_t reserved=0;
    std::uint64_t shell_epoch=0,functions[FunctionCount]{};
};
// Worker publishes the actual kernel-assigned loopback PCM port for this session.
// A fresh status alone cannot authorize connecting to a stale/different endpoint.
struct Endpoint {
    std::uint64_t signature=magic,token=0,build=0,shell_epoch=0;
    std::int32_t shell_pid=0;
    std::uint16_t port=0,reserved=0;
};
static_assert(sizeof(Endpoint)==40);
inline bool valid(const Endpoint& e,const Request& q) {
    return q.signature==magic&&q.protocol==version&&q.size==sizeof q&&q.token&&q.build&&q.shell_pid>1&&q.shell_epoch&&
        e.signature==magic&&e.token==q.token&&e.build==q.build&&e.shell_epoch==q.shell_epoch&&
        e.shell_pid==q.shell_pid&&e.port>=1024&&!e.reserved;
}
struct Resident {
    std::uint64_t signature=magic,build=0;
    std::uint32_t protocol=version;
    std::int32_t shell_pid=0;
    std::uint64_t shell_epoch=0;
};
// Written once by a worker whose startup failed, so the parent reports the
// failure instead of Starting. That worker stays inert for this ShellUI run.
enum FailureStep:int {ListenFailed=1,ResidentFailed=2};
struct Failure {
    std::uint64_t signature=magic,build=0,token=0,shell_epoch=0;
    std::int32_t shell_pid=0,step=0,error=0,rc=0;
};
struct Heartbeat {std::uint64_t token=0,at_ms=0;};
// Earlier USB workers keep their stopped ID. Metadata qualifies it for a live
// native check, never for ignoring an active source or controlling that source.
struct InactiveSources {
    std::uint64_t signature=magic,token=0;
    std::uint32_t count=0,reserved=0;
    std::int32_t ids[8]{};
};
inline bool valid(const InactiveSources& s,std::uint64_t token) {
    if(s.signature!=magic||s.token!=token||s.count>8)return false;
    for(unsigned i=0;i<s.count;++i)if(s.ids[i]<=0)return false;
    return true;
}
struct CommandRequest {std::uint64_t token=0,sequence=0;std::int32_t action=0,reserved=0;};
struct Gate {std::uint64_t token=0,generation=0;std::int32_t playing=0,intent=0;};
struct Status {
    std::uint64_t signature=magic,token=0,sequence=0,at_ms=0;
    std::int32_t state=Idle,source=-1,rc=0,detail=0;
    std::int32_t port=-1;std::uint32_t queued_frames=0;
    std::uint64_t audio_frames=0,underruns=0,generation=0,checked_ms=0;
    std::uint16_t output=0,reserved[3]{};
};
struct PreviousRequest {std::uint64_t signature=0;std::uint32_t protocol=0,size=0;std::uint64_t build=0,token=0;unsigned char tail[168]{};};
static_assert(sizeof(PreviousRequest)==200);
// This deployed v12 worker has identical wire/API layout, but can republish an
// old bootstrap token after a PCM session. The parent waits for its real status.
constexpr std::uint64_t bootstrap_status_race_build=0x686f6c6f1cfdc220ULL;
inline bool compatible_resident(std::uint32_t protocol,std::uint64_t resident,std::uint64_t current) {
    return protocol==version&&(resident==current||resident==bootstrap_status_race_build);
}
inline bool previous_clean(const Status& s,std::uint64_t token) {
    return token&&s.signature==magic&&s.token==token&&s.port<0&&s.source<=0&&
        (s.state==Idle||s.state==StoppedOk);
}
inline bool publish_bootstrap_idle(std::uint64_t session_token,bool active,bool terminal) {
    return !session_token&&!active&&terminal;
}
inline bool startup_failure(const Status& s) {
    return s.sequence==0&&s.state==Failed&&s.source==-1&&s.port==-1&&(s.detail==10||s.detail==24);
}
inline bool status_for_command(const Status& s,std::uint64_t sequence,bool stopping) {
    return stopping||s.sequence==sequence||startup_failure(s);
}
// Prior PCM generations used the same envelope with 18 functions (192 bytes).
struct LegacyRequest {
    std::uint64_t signature=0;
    std::uint32_t protocol=0,size=0;
    std::uint64_t build=0,token=0;
    unsigned char tail[160]{};
};
static_assert(sizeof(Request)==208&&sizeof(LegacyRequest)==192&&sizeof(Resident)==32&&sizeof(Status)==96&&sizeof(Failure)==48);
inline bool address(std::uint64_t p){return p>=0x10000&&p<0x800000000000ULL;}
inline bool valid(const Request& r,int pid,std::uint64_t build) {
    if(r.signature!=magic||r.protocol!=version||r.size!=sizeof r||!r.token||
       r.shell_pid!=pid||!r.shell_epoch||r.build!=build)return false;
    for(auto p:r.functions)if(!address(p))return false;
    return true;
}
}
