// SPDX-License-Identifier: GPL-3.0-or-later
#include "media.hpp"
#include "files.hpp"
#include <unistd.h>
#include <cstdarg>
using namespace usb_transport;
static Buffers buffers;
static unsigned char info[0x2588]; // GetMediaPlaybackInfo: FW 13.20 exact output size.
static void note(const char* format,...) {
    char p[256];path(p,sizeof p,worker_directory,"worker.log");FILE* f=std::fopen(p,"a");if(!f)return;
    if(!std::fseek(f,0,SEEK_END)&&std::ftell(f)>512*1024){std::fclose(f);f=std::fopen(p,"w");if(!f)return;}
    std::fprintf(f,"[%llu] ",(unsigned long long)now_ms());va_list a;va_start(a,format);
    std::vfprintf(f,format,a);va_end(a);std::fputc('\n',f);std::fclose(f);
}
static Api api;
static Request session;
static Status status;
static std::uint64_t started_at;
static bool fresh() {
    Heartbeat h;const auto now=now_ms();
    return read_file(worker_directory,"heartbeat.bin",h)&&h.token==session.token&&h.at_ms<=now+1000&&now<=h.at_ms+3000;
}
static bool selected() {
    return status.source>0&&api.current(2)==status.source;
}
static bool identity(int source) {
    std::memset(info,0,sizeof info);
    int rc=api.info(2,info);int id=0,service=0;
    std::memcpy(&id,info+4,4);std::memcpy(&service,info+8,4);
    auto* uri=reinterpret_cast<char*>(info+0xc4);
    return !rc&&id==source&&service==2&&std::memchr(uri,0,4096)&&streamer(uri);
}
static bool stop_owned() {
    if(!selected()){status.source=-1;status.state=StoppedOk;return true;}
    // Only this session's source (or a positively identified adopted Streamer).
    status.rc=api.command(6,status.source,1);
    note("STOP source=%d rc=%08x",status.source,status.rc);
    if(status.rc){status.state=Failed;return false;}
    for(int i=0;i<30;++i) {
        bool stopped=false,playing=false;
        if(!selected()||(!api.stopped(status.source,&stopped)&&!api.playing(status.source,&playing)&&stopped&&!playing)) {
            status.state=StoppedOk;note("STOP confirmed source=%d",status.source);return true;
        }
        usleep(100000);
    }
    status.state=Failed;status.detail=6;return false;
}
static bool unobstructed(int ours=-1) {
    for(int service=1;service<=20;++service) {
        int id=api.current(service);if(id<=0||id==ours)continue;
        bool playing=false,paused=false,stopped=false;
        if(api.playing(id,&playing)||api.paused(id,&paused)||api.stopped(id,&stopped)||playing||(!paused&&!stopped)) {
            note("REFUSED existing music service=%d source=%d playing=%u paused=%u stopped=%u",service,id,playing,paused,stopped);
            return false;
        }
    }
    return true;
}
static bool still_requested() {
    CommandRequest c;
    return fresh()&&read_file(worker_directory,"command.bin",c)&&c.token==session.token&&c.sequence==status.sequence&&c.action==1;
}
static void ensure() {
    status.rc=0;status.detail=0;
    int id=api.current(2);
    if(id>0&&id!=status.source&&identity(id))status.source=id;
    if(selected()) {
        bool playing=false,paused=false;
        if(!api.playing(id,&playing)&&playing){status.state=Running;return;}
        if(!api.paused(id,&paused)&&paused) {
            if(!unobstructed(id)||!still_requested()){status.state=Refused;return;}
            status.rc=api.command(2,id,1);status.state=status.rc?Failed:Starting;started_at=now_ms();
            note("RESUME source=%d rc=%08x",id,status.rc);return;
        }
    }
    if(!unobstructed()){status.state=Refused;return;}
    int count=0;std::memset(&buffers,0,sizeof buffers);
    if((status.rc=api.devices(&count,&buffers.devices))||count<1||count>8||buffers.devices.count!=count) {
        status.state=Failed;status.detail=1;return;
    }
    unsigned found=0;std::uint32_t device=0;
    auto list=[&](std::uint32_t dev) {
        int tracks=0,folders=0;std::memset(&buffers.tracks,0,sizeof buffers.tracks);std::memset(&buffers.folders,0,sizeof buffers.folders);
        int rc=api.files(dev,clip_folder,0,&tracks,&buffers.tracks,&folders,&buffers.folders);
        note("LIST device=%u rc=%08x total=%d entries=%d",dev,rc,tracks,buffers.tracks.count);
        const auto& t=buffers.tracks.entries[0];
        return !rc&&tracks==1&&folders==0&&buffers.tracks.count==1&&buffers.folders.count==0&&buffers.tracks.page==0&&
            t.index==0&&t.device==dev&&std::memchr(t.path,0,sizeof t.path)&&streamer(t.path);
    };
    for(int i=0;i<count&&still_requested();++i)if(list(buffers.devices.entries[i].id)){++found;device=buffers.devices.entries[i].id;}
    if(found!=1||!still_requested()||!list(device)||!unobstructed()) {status.state=Refused;status.detail=2;return;}
    const auto& track=buffers.tracks.entries[0];status.source=-1;
    // The tested native helper supplies Sony's callbacks and current source bookkeeping.
    status.rc=api.start(2,track.path,track.index,device,&status.source,1,false);
    note("START device=%u index=%d path=%s source=%d rc=%08x",device,track.index,track.path,status.source,status.rc);
    const int start_rc=status.rc;
    if(start_rc||!selected()) {
        if(selected())(void)stop_owned();
        status.state=Failed;status.rc=start_rc;status.detail=4;
    } else status.state=Starting;
    started_at=now_ms();
}
#ifndef USB_TRANSPORT_TEST
extern "C" __attribute__((noreturn,visibility("default"))) void* elf_main(void*) {
    Request initial;
    if(!read_file(worker_directory,"request.bin",initial)||!valid(initial,getpid(),USB_TRANSPORT_BUILD))for(;;)usleep(1000000);
    Resident r;r.build=USB_TRANSPORT_BUILD;r.shell_pid=getpid();r.shell_epoch=initial.shell_epoch;
    if(!write_file(worker_directory,"resident.bin",r))for(;;)usleep(1000000);
    note("READY build=%016llx",(unsigned long long)r.build);
    for(;;) {
        Request q;std::uint64_t arm=0;
        if(read_file(worker_directory,"request.bin",q)&&valid(q,getpid(),USB_TRANSPORT_BUILD)&&q.shell_epoch==r.shell_epoch&&
           read_file(worker_directory,"arm.bin",arm)&&arm==q.token&&q.token!=session.token) {
            if(session.token&&status.source>0&&!stop_owned()){usleep(250000);continue;}
            session=q;status=Status{};status.token=q.token;
            api={reinterpret_cast<decltype(Api::devices)>(q.functions[Devices]),reinterpret_cast<decltype(Api::files)>(q.functions[Files]),
                reinterpret_cast<decltype(Api::start)>(q.functions[Start]),reinterpret_cast<decltype(Api::command)>(q.functions[Command]),
                reinterpret_cast<decltype(Api::current)>(q.functions[Current]),reinterpret_cast<decltype(Api::time)>(q.functions[Time]),
                reinterpret_cast<decltype(Api::playing)>(q.functions[Playing]),reinterpret_cast<decltype(Api::paused)>(q.functions[Paused]),
                reinterpret_cast<decltype(Api::stopped)>(q.functions[Stopped]),reinterpret_cast<decltype(Api::error)>(q.functions[Error]),
                reinterpret_cast<decltype(Api::info)>(q.functions[Info])};
        }
        if(session.token) {
            if(!fresh()) {
                if(status.source>0&&status.state!=StoppedOk)(void)stop_owned();
            } else {
                CommandRequest c;
                if(read_file(worker_directory,"command.bin",c)&&c.token==session.token&&c.sequence>status.sequence) {
                    status.sequence=c.sequence;note("REQUEST seq=%llu action=%d",(unsigned long long)c.sequence,c.action);
                    if(c.action==1)ensure();else if(c.action==2)(void)stop_owned();
                }
                if(status.state==Starting||status.state==Running) {
                    bool playing=false,paused=false,stopped=false;int error=0,detail=0;
                    if(!selected()){status.state=Lost;status.source=-1;}
                    else if(api.playing(status.source,&playing)||api.paused(status.source,&paused)||api.stopped(status.source,&stopped))status.state=Failed;
                    else if(!api.error(status.source,&error,&detail)&&error){status.state=Failed;status.rc=error;status.detail=detail;}
                    else if(playing)status.state=Running;
                    else if(status.state==Running&&(paused||stopped))status.state=Lost;
                    else if(status.state==Starting&&now_ms()>started_at+15000){status.state=Failed;status.detail=3;}
                }
            }
            status.at_ms=now_ms();(void)write_file(worker_directory,"status.bin",status);
        }
        usleep(100000);
    }
}

#endif
