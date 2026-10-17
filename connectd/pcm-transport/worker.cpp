// SPDX-License-Identifier: GPL-3.0-or-later
#include "media.hpp"
#include "files.hpp"
#include <unistd.h>
#include <cstdarg>
#include "socket.hpp"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <cerrno>
using namespace pcm_transport;
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
static bool close_audio() {
    if(status.port<0)return true;
    const int rc=api.close(status.port);note("AUDIO close port=%d rc=%08x",status.port,rc);
    if(rc){status.rc=rc;status.detail=7;return false;}
    status.port=-1;return true;
}
static bool stop_owned() {
    const bool closed=close_audio();
    if(!selected()){status.source=-1;status.state=closed?StoppedOk:Failed;return closed;}
    // Only this session's source (or a positively identified adopted Streamer).
    status.rc=api.command(6,status.source,1);
    note("STOP source=%d rc=%08x",status.source,status.rc);
    if(status.rc){status.state=Failed;return false;}
    for(int i=0;i<30;++i) {
        bool stopped=false,playing=false;
        if(!selected()||(!api.stopped(status.source,&stopped)&&!api.playing(status.source,&playing)&&stopped&&!playing)) {
            status.state=StoppedOk;note("STOP confirmed source=%d",status.source);status.source=-1;status.state=closed?StoppedOk:Failed;return closed;
        }
        usleep(100000);
    }
    status.state=Failed;status.detail=6;return false;
}
// Pause the existing owned source; never select a new file or issue a toast.
static void pause_owned() {
    if(!close_audio()){status.state=Failed;return;}
    if(!selected()){status.source=-1;status.state=StoppedOk;return;}
    bool paused=false;
    if(!api.paused(status.source,&paused)&&paused){status.state=ServicePaused;return;}
    status.rc=api.command(3,status.source,1);
    note("PAUSE source=%d rc=%08x",status.source,status.rc);
    status.state=status.rc?Failed:Pausing;started_at=now_ms();
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

static SocketReader channel;
static Receiver& receiver=channel.receiver;
static bool wanted=false;
static void disconnect_client(){channel.disconnect();}
static bool open_audio() {
    if(status.port>=0)return true;
    int port=api.open(0xff,0,0,pcm_grain,pcm_rate,1);
    if(std::uint32_t(port)==0x8026000fU) {
        int init=api.init();note("AUDIO conditional Init rc=%08x",init);
        if(!init||std::uint32_t(init)==0x8026000eU)port=api.open(0xff,0,0,pcm_grain,pcm_rate,1);
    }
    status.rc=port;if(port<0)return false;status.port=port;
    const int volume[2]={32768,32768};status.rc=api.volume(port,3,volume);
    note("AUDIO MAIN open=%08x volume_rc=%08x rate=%u grain=%u",port,status.rc,pcm_rate,pcm_grain);
    return !status.rc;
}
// One check of the owned source's native state. A source that was just started
// or resumed can briefly be unselected or Stopped before it plays: that is not
// a loss until the start deadline (v3 reported Lost 7 ms after Start).
static void observe(std::uint64_t now) {
    status.checked_ms=now;bool playing=false,paused=false,stopped=false;int error=0,detail=0;
    const bool settling=status.state==Starting&&now<=started_at+15000;
    if(!selected()) {
        if(settling)return;
        status.source=-1;status.state=Lost;(void)close_audio();disconnect_client();
    }
    else if(!unobstructed(status.source)) {
        (void)stop_owned();status.state=Lost;status.detail=8;disconnect_client();
        note("YIELD another music source became active");
    }
    else if(api.playing(status.source,&playing)||api.paused(status.source,&paused)||api.stopped(status.source,&stopped))status.state=Failed;
    else if(!api.error(status.source,&error,&detail)&&error){status.state=Failed;status.rc=error;status.detail=detail;}
    else if(playing&&status.state!=Pausing) {
        if(!wanted) {
            // Resumed from the console while Spotify is paused. Do not pause it
            // back: report it so the receiver resumes Spotify, then open audio.
            if(status.state!=ConsoleResumed)note("CONSOLE resumed source=%d",status.source);
            status.state=ConsoleResumed;return;
        }
        double seconds=0;
        if(!api.time(status.source,&seconds)&&seconds>0.1) {
            if(open_audio())status.state=Running;
            else {int rc=status.rc;(void)stop_owned();status.state=Failed;status.rc=rc;status.detail=7;}
        }
    }
    else if(paused&&status.state!=Starting) {
        (void)close_audio();disconnect_client();
        // Paused at the console while Spotify played, versus our own Pause: only
        // the first may pause Spotify (a late 4 from our Pause did, in v5).
        if(status.state==Running){note("CONSOLE paused source=%d",status.source);status.state=ConsolePaused;}
        else if(status.state!=ConsolePaused)status.state=ServicePaused;
    }
    else if(stopped) {
        if(settling)return;
        (void)close_audio();disconnect_client();status.source=-1;status.state=Lost;
    }
    else if(status.state==Pausing&&now>started_at+3000){status.state=Failed;status.detail=9;}
    else if(status.state==Starting&&now>started_at+15000){status.state=Failed;status.detail=3;}
    if(status.state==Failed){int rc=status.rc,detail=status.detail;disconnect_client();(void)stop_owned();status.state=Failed;status.rc=rc;status.detail=detail;}
}

#ifndef PCM_TRANSPORT_TEST
static std::uint64_t generation=0;
static bool allowed=false;
static std::uint64_t micros(){timespec t{};clock_gettime(CLOCK_MONOTONIC,&t);return std::uint64_t(t.tv_sec)*1000000+t.tv_nsec/1000;}
// Audio loop benchmark per health window. work is time between Output calls
// spent on anything but the pacing sleep (file I/O, media queries, receive).
struct Timing {
    std::uint64_t last_start=0,last_end=0,slept=0,grains=0,late=0,period_max=0,call_max=0,call_total=0,work_max=0;
    void window(){grains=late=period_max=call_max=call_total=work_max=0;}
};
static Timing timing;
constexpr std::uint64_t grain_us=std::uint64_t(pcm_grain)*1000000/pcm_rate;
static bool observing(){
    return status.state==Starting||status.state==Running||status.state==Pausing||
        status.state==ServicePaused||status.state==ConsoleResumed||status.state==ConsolePaused;
}
static bool listen_pcm() {
    const bool ok=channel.listen_on();char detail[320];channel.trace.describe(detail,sizeof detail);
    note("PCM listen %s port=%d %s",ok?"ok":"FAILED",pcm_port,detail);
    return ok;
}
static bool receive_pcm() {
    const auto connections=channel.connections,refused=channel.refused_clients;
    bool ok=channel.receive(session.token,generation,now_ms(),allowed);
    if(channel.connections!=connections||channel.refused_clients!=refused) {
        char detail[320];channel.client_trace.describe(detail,sizeof detail);
        note("PCM accept token=%llx %s",(unsigned long long)session.token,detail);
    }
    return ok;
}
[[noreturn]] static void fail_startup(const Request& q,FailureStep step,int error,int rc) {
    Failure f;f.build=PCM_TRANSPORT_BUILD;f.token=q.token;f.shell_epoch=q.shell_epoch;f.shell_pid=getpid();
    f.step=step;f.error=error;f.rc=rc;
    note("REFUSED startup step=%d errno=%d rc=%d failure_published=%u",step,error,rc,unsigned(write_file(worker_directory,"failure.bin",f)));
    for(;;)usleep(1000000);
}
extern "C" __attribute__((noreturn,visibility("default"))) void* elf_main(void*) {
    Request initial;
    if(!read_file(worker_directory,"request.bin",initial)||!valid(initial,getpid(),PCM_TRANSPORT_BUILD)) {
        note("REFUSED startup request invalid build=%016llx",(unsigned long long)PCM_TRANSPORT_BUILD);for(;;)usleep(1000000);
    }
    Resident r;r.build=PCM_TRANSPORT_BUILD;r.shell_pid=getpid();r.shell_epoch=initial.shell_epoch;
    if(!listen_pcm())fail_startup(initial,ListenFailed,channel.trace.error,channel.trace.rc);
    if(!write_file(worker_directory,"resident.bin",r))fail_startup(initial,ResidentFailed,errno,-1);
    note("READY build=%016llx MAIN_PCM service_marker no_transport_toasts",(unsigned long long)r.build);
    std::uint64_t checked=0,published=0,reported=0,due=0,pace_start=0,paced_frames=0;bool active_parent=false;
    alignas(64) std::int16_t pcm[pcm_grain*2]{};
    for(;;) {
        const auto now=now_ms();Request q;std::uint64_t arm=0;
        if(now-checked>=100) {
            checked=now;
            if(read_file(worker_directory,"request.bin",q)&&valid(q,getpid(),PCM_TRANSPORT_BUILD)&&q.shell_epoch==r.shell_epoch&&
               read_file(worker_directory,"arm.bin",arm)&&arm==q.token&&q.token!=session.token) {
                if(session.token&&(status.port>=0||status.source>0)&&!stop_owned()){usleep(100000);continue;}
                disconnect_client();session=q;status=Status{};status.token=q.token;
                api={reinterpret_cast<decltype(Api::devices)>(q.functions[Devices]),reinterpret_cast<decltype(Api::files)>(q.functions[Files]),
                    reinterpret_cast<decltype(Api::start)>(q.functions[Start]),reinterpret_cast<decltype(Api::command)>(q.functions[Command]),
                    reinterpret_cast<decltype(Api::current)>(q.functions[Current]),reinterpret_cast<decltype(Api::time)>(q.functions[Time]),
                    reinterpret_cast<decltype(Api::playing)>(q.functions[Playing]),reinterpret_cast<decltype(Api::paused)>(q.functions[Paused]),
                    reinterpret_cast<decltype(Api::stopped)>(q.functions[Stopped]),reinterpret_cast<decltype(Api::error)>(q.functions[Error]),
                    reinterpret_cast<decltype(Api::info)>(q.functions[Info]),reinterpret_cast<decltype(Api::open)>(q.functions[Open]),
                    reinterpret_cast<decltype(Api::output)>(q.functions[Output]),reinterpret_cast<decltype(Api::close)>(q.functions[Close]),
                    reinterpret_cast<decltype(Api::volume)>(q.functions[Volume]),reinterpret_cast<decltype(Api::port_state)>(q.functions[PortState]),
                    reinterpret_cast<decltype(Api::last_time)>(q.functions[LastTime]),reinterpret_cast<decltype(Api::init)>(q.functions[Init])};
                if(channel.listener<0&&!listen_pcm()){status.state=Failed;status.rc=channel.last_error;status.detail=10;continue;}
                generation=0;allowed=wanted=false;active_parent=true;due=0;
            }
            if(session.token&&active_parent) {
                if(!fresh()) {
                    channel.shutdown();
                    (void)stop_owned();active_parent=false;allowed=false;
                    note("PARENT LOST state=%d source=%d port=%d",status.state,status.source,status.port);
                } else {
                    Gate g;
                    if(read_file(worker_directory,"gate.bin",g)&&g.token==session.token) {
                        if(g.generation!=generation||bool(g.playing)!=allowed){generation=g.generation;allowed=g.playing;receiver.flush();}
                        wanted=g.intent!=0;
                    }
                    CommandRequest c;
                    if(read_file(worker_directory,"command.bin",c)&&c.token==session.token&&c.sequence>status.sequence) {
                        status.sequence=c.sequence;note("REQUEST seq=%llu action=%d",(unsigned long long)c.sequence,c.action);
                        if(c.action==1)ensure();else if(c.action==2){disconnect_client();(void)stop_owned();}
                        status.checked_ms=0;
                    }
                }
            }
        }
        if(active_parent&&!wanted&&(status.state==Starting||status.state==Running)) {
            disconnect_client();pause_owned();status.checked_ms=0;
            if(status.state==Failed){const int rc=status.rc;(void)stop_owned();status.state=Failed;status.rc=rc;}
        }
        const unsigned interval=status.state==Starting||status.state==Pausing?100:500;
        if(active_parent&&observing()&&now-status.checked_ms>=interval)observe(now);
        if(status.state==Running&&status.port>=0&&active_parent) {
            if(!receive_pcm()){disconnect_client();note("PCM connection reset errno=%d bytes=%llu calls=%llu empty=%llu; queued audio cleared",channel.last_error,(unsigned long long)channel.received_bytes,(unsigned long long)channel.receives,(unsigned long long)channel.empty_reads);}
            if(!allowed)receiver.flush();
            // Underruns now count dry-outs of a started stream, not silent grains.
            auto& playout=receiver.playout;const auto dry=playout.dryouts;
            unsigned frames=playout.next(receiver.ring,pcm);status.audio_frames+=frames;
            if(playout.dryouts!=dry&&allowed)++status.underruns;
            const auto start=micros();
            if(timing.last_end) {
                timing.work_max=std::max(timing.work_max,start-timing.last_end-std::min(timing.slept,start-timing.last_end));
                const auto period=start-timing.last_start;timing.period_max=std::max(timing.period_max,period);
                if(period>grain_us*3/2)++timing.late;
            }
            status.rc=api.output(status.port,pcm);
            const auto us=micros();
            timing.call_max=std::max(timing.call_max,us-start);timing.call_total+=us-start;++timing.grains;
            timing.last_start=start;timing.last_end=us;timing.slept=0;
            if(status.rc<0){int rc=status.rc;(void)stop_owned();status.state=Failed;status.rc=rc;}
            // Deadline pacing even when Output returns immediately, kept one grain
            // ahead so a non-blocking Output is never fed just in time.
            if(!due||us>due+grain_us){pace_start=us;paced_frames=0;}
            paced_frames+=pcm_grain;due=pace_start+(paced_frames-pcm_grain)*1000000/pcm_rate;
            if(due>us){const auto s=std::min<std::uint64_t>(due-us,10000);usleep(unsigned(s));timing.slept+=s;}
        } else {due=0;timing.last_end=0;usleep(5000);}
        if(active_parent&&now-reported>=5000) {
            reported=now;note("PCM health state=%d intent=%u gate=%u gen=%llu bytes=%llu frames=%llu queued=%u underruns=%llu receives=%llu empty=%llu accepted=%llu discarded=%llu",
                status.state,unsigned(wanted),unsigned(allowed),(unsigned long long)generation,(unsigned long long)channel.received_bytes,
                (unsigned long long)status.audio_frames,receiver.ring.size,(unsigned long long)status.underruns,
                (unsigned long long)channel.receives,(unsigned long long)channel.empty_reads,
                (unsigned long long)receiver.accepted_frames,(unsigned long long)receiver.discarded_frames);
            if(timing.grains)note("PCM timing grains=%llu late=%llu period_max=%lluus call_max=%lluus call_avg=%lluus work_max=%lluus grain=%lluus",
                (unsigned long long)timing.grains,(unsigned long long)timing.late,(unsigned long long)timing.period_max,
                (unsigned long long)timing.call_max,(unsigned long long)(timing.call_total/timing.grains),
                (unsigned long long)timing.work_max,(unsigned long long)grain_us);
            timing.window();
        }
        if(session.token&&now-published>=500) {
            published=now;status.at_ms=now;status.generation=generation;status.queued_frames=receiver.ring.size;
            if(status.port>=0){unsigned char state[32]{};if(!api.port_state(status.port,state))std::memcpy(&status.output,state,2);}
            (void)write_file(worker_directory,"status.bin",status);
        }
    }
}

#endif
