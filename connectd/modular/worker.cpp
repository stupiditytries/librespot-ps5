// SPDX-License-Identifier: GPL-3.0-or-later
#include "media.hpp"
#include "ownership/media.hpp"
#include "ownership/files.hpp"
#include "ownership/feature.hpp"
#include <optional>
#include "files.hpp"
#include "bootstrap.hpp"
#include <unistd.h>
#include <cstdarg>
#include "socket.hpp"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <cerrno>
using namespace pcm_transport;
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
    return pcm_transport::read_file(worker_directory,"heartbeat.bin",h)&&h.token==session.token&&h.at_ms<=now+1000&&now<=h.at_ms+3000;
}
static bool close_audio() {
    if(status.port<0)return true;
    const int rc=api.close(status.port);note("AUDIO close port=%d rc=%08x",status.port,rc);
    if(rc){status.rc=rc;status.detail=7;return false;}
    status.port=-1;return true;
}
static bool wanted=false;
#include "marker.inc"
static SocketReader channel;
static Receiver& receiver=channel.receiver;
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
    else if(__atomic_load_n(&callback_errors,__ATOMIC_RELAXED)||__atomic_load_n(&callback_ends,__ATOMIC_RELAXED)) {
        status.state=Failed;status.detail=23;
    }
    else if(marker_seek_at) {
        double seconds=-1;
        const int rc=api.time(status.source,&seconds);
        if(!rc&&playing&&seconds>=0&&seconds<10) {
            note("MARKER rewind confirmed source=%d position=%0.3f; PCM song timeline unchanged",status.source,seconds);
            marker_seek_at=0;
        } else if(now>=marker_seek_at+5000) {status.state=Failed;status.rc=rc;status.detail=25;}
        // Rewind can briefly report Paused/Stopped. It is not a console command;
        // keep the existing PCM port and state while waiting for native seek.
    }
    else if(playing&&status.state!=Pausing) {
        if(!wanted) {
            // Resumed from the console while Spotify is paused. Do not pause it
            // back: report it so the receiver resumes Spotify, then open audio.
            if(status.state!=ConsoleResumed)note("CONSOLE resumed source=%d",status.source);
            status.state=ConsoleResumed;return;
        }
        double seconds=0;
        if(!api.time(status.source,&seconds)&&seconds>0.1) {
            if(seconds>=90&&wanted) {
                // Rewind only the silent ownership WAV, never Spotify PCM.
                status.rc=api.seek(status.source,0.0);
                note("MARKER rewind source=%d from=%0.3f rc=%08x",status.source,seconds,status.rc);
                if(status.rc){status.state=Failed;status.detail=25;}
                else marker_seek_at=now;
            }
            if(status.state!=Failed&&open_audio())status.state=Running;
            else if(status.state!=Failed){int rc=status.rc;(void)stop_owned();status.state=Failed;status.rc=rc;status.detail=7;}
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
        (void)close_audio();disconnect_client();if(stop_owned())status.state=Lost;
    }
    else if(status.state==Pausing&&now>started_at+3000){status.state=Failed;status.detail=9;}
    else if(status.state==Starting&&now>started_at+15000){status.state=Failed;status.detail=3;}
    if(status.state==Failed){int rc=status.rc,detail=status.detail;disconnect_client();(void)stop_owned();status.state=Failed;status.rc=rc;status.detail=detail;}
}

#ifndef PCM_TRANSPORT_TEST
static void bind_api(const Request& q) {
    api={reinterpret_cast<decltype(Api::add)>(q.functions[Add]),
                    reinterpret_cast<decltype(Api::play)>(q.functions[Play]),
                    reinterpret_cast<decltype(Api::stop)>(q.functions[Stop]),
                    reinterpret_cast<decltype(Api::remove)>(q.functions[Remove]),
                    reinterpret_cast<decltype(Api::current)>(q.functions[Current]),
                    reinterpret_cast<decltype(Api::time)>(q.functions[Time]),
                    reinterpret_cast<decltype(Api::playing)>(q.functions[Playing]),
                    reinterpret_cast<decltype(Api::paused)>(q.functions[Paused]),
                    reinterpret_cast<decltype(Api::stopped)>(q.functions[Stopped]),
                    reinterpret_cast<decltype(Api::error)>(q.functions[Error]),
                    reinterpret_cast<decltype(Api::pause)>(q.functions[Pause]),
                    reinterpret_cast<decltype(Api::open)>(q.functions[Open]),
                    reinterpret_cast<decltype(Api::output)>(q.functions[Output]),
                    reinterpret_cast<decltype(Api::close)>(q.functions[Close]),
                    reinterpret_cast<decltype(Api::volume)>(q.functions[Volume]),
                    reinterpret_cast<decltype(Api::port_state)>(q.functions[PortState]),
                    reinterpret_cast<decltype(Api::last_time)>(q.functions[LastTime]),
                    reinterpret_cast<decltype(Api::init)>(q.functions[Init]),
                    reinterpret_cast<decltype(Api::seek)>(q.functions[Seek])};
}
static void bootstrap_event(std::uint64_t,std::uint64_t,std::uint64_t,std::uint64_t) {}
static usb_free::Callbacks bootstrap_callbacks{bootstrap_event,nullptr,4};
#include "feature-worker.inc"
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
    note("PCM listen %s port=%u %s",ok?"ok":"FAILED",channel.bound_port,detail);
    if(!ok)return false;
    Endpoint endpoint;endpoint.token=session.token;endpoint.build=session.build;
    endpoint.shell_pid=session.shell_pid;endpoint.shell_epoch=session.shell_epoch;endpoint.port=channel.bound_port;
    if(!valid(endpoint,session)||!pcm_transport::write_file(worker_directory,"endpoint.bin",endpoint)) {
        channel.last_error=errno?errno:EINVAL;channel.shutdown();
        note("PCM endpoint publication FAILED errno=%d",channel.last_error);return false;
    }
    return true;
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
    note("REFUSED startup step=%d errno=%d rc=%d failure_published=%u",step,error,rc,unsigned(pcm_transport::write_file(worker_directory,"failure.bin",f)));
    for(;;)usleep(1000000);
}
extern "C" __attribute__((noreturn,visibility("default"))) void* elf_main(void*) {
    Request initial;
    if(!pcm_transport::read_file(worker_directory,"request.bin",initial)||!valid(initial,getpid(),PCM_TRANSPORT_BUILD)) {
        note("REFUSED startup request invalid build=%016llx",(unsigned long long)PCM_TRANSPORT_BUILD);for(;;)usleep(1000000);
    }
    Resident r;r.build=PCM_TRANSPORT_BUILD;r.shell_pid=getpid();r.shell_epoch=initial.shell_epoch;
    if(!pcm_transport::write_file(worker_directory,"resident.bin",r))fail_startup(initial,ResidentFailed,errno,-1);
    prepare_feature=begin_feature;restore_feature=end_feature;
    note("READY build=%016llx MAIN_PCM native_HTTP_marker USB_free exact_mode_restore PCM_listener_deferred_until_arm",(unsigned long long)r.build);
    std::uint64_t checked=0,published=0,reported=0,due=0,pace_start=0,paced_frames=0;bool active_parent=false,startup_failed=false;
    alignas(64) std::int16_t pcm[pcm_grain*2]{};
    std::optional<Bootstrap> bootstrap;
    std::uint64_t boot_published=0;
    for(;;) {
        const auto now=now_ms();Request q;std::uint64_t arm=0;
        if(now-checked>=100) {
            checked=now;
            BootstrapCommand boot;
            if(!active_parent&&status.port<0&&status.source<0&&
               pcm_transport::read_file(worker_directory,"request.bin",q)&&valid(q,getpid(),PCM_TRANSPORT_BUILD)&&q.shell_epoch==r.shell_epoch&&
               pcm_transport::read_file(worker_directory,"bootstrap.bin",boot)&&boot.token==q.token&&boot.action==1&&
               (!bootstrap||bootstrap->status.token!=q.token)) {
                // A failed cleanup permanently blocks this resident.
                if(bootstrap&&!bootstrap_clean(bootstrap->status,bootstrap->status.token))continue;
                Heartbeat hb;
                if(!pcm_transport::read_file(worker_directory,"heartbeat.bin",hb)||hb.token!=q.token||
                   hb.at_ms>now+1000||now>hb.at_ms+3000)continue;
                bind_api(q);
                bootstrap.emplace(BootstrapApi{api.add,reinterpret_cast<int(*)(int)>(q.functions[Load]),api.stop,api.remove,
                    api.current,api.playing,api.paused,api.stopped,api.error},
                    BootstrapClock{now_ms,[](unsigned ms){usleep(ms*1000);}},note);
                bootstrap->begin(q.token,bootstrap_callbacks);
            }
            if(bootstrap) {
                Heartbeat hb;std::uint64_t force_stop=0;
                const bool alive=pcm_transport::read_file(worker_directory,"heartbeat.bin",hb)&&hb.token==bootstrap->status.token&&
                    hb.at_ms<=now+1000&&now<=hb.at_ms+3000;
                const bool finish=pcm_transport::read_file(worker_directory,"bootstrap.bin",boot)&&
                    boot.token==bootstrap->status.token&&boot.action==2;
                const bool stopped=pcm_transport::read_file(worker_directory,"stop.bin",force_stop)&&force_stop==bootstrap->status.token;
                bootstrap->tick(alive&&!stopped,finish);
                if(now-boot_published>=100) {
                    boot_published=now;bootstrap->status.at_ms=now_ms();
                    (void)pcm_transport::write_file(worker_directory,"bootstrap-status.bin",bootstrap->status);
                    if(publish_bootstrap_idle(session.token,active_parent,bootstrap->terminal())) {
                        Status idle;idle.token=bootstrap->status.token;idle.at_ms=now_ms();
                        if(!bootstrap_clean(bootstrap->status,idle.token)) {
                            idle.state=Failed;idle.source=bootstrap->status.source;idle.rc=bootstrap->status.rc;idle.detail=26;
                        }
                        (void)pcm_transport::write_file(worker_directory,"status.bin",idle);
                    }
                }
                if(bootstrap->busy()||!bootstrap_clean(bootstrap->status,bootstrap->status.token))continue;
            }
            if(pcm_transport::read_file(worker_directory,"request.bin",q)&&valid(q,getpid(),PCM_TRANSPORT_BUILD)&&q.shell_epoch==r.shell_epoch&&
               pcm_transport::read_file(worker_directory,"arm.bin",arm)&&arm==q.token&&q.token!=session.token) {
                if(session.token&&(status.port>=0||status.source>0)&&!stop_owned()){usleep(100000);continue;}
                disconnect_client();session=q;status=Status{};status.token=q.token;
                bind_api(q);
                // Cleanup/parent-loss handling must remain reachable even if listen
                // or startup-file validation fails before the first command.
                active_parent=true;startup_failed=false;allowed=wanted=false;
                if(!pcm_transport::read_file(worker_directory,"inactive-sources.bin",inactive_sources)||!valid(inactive_sources,q.token)||
                   !pcm_transport::read_file(worker_directory,"prior.bin",prior_sources)||!usb_free::valid(prior_sources,q.token)||
                   !pcm_transport::read_file(worker_directory,"feature-plan.bin",feature_plan)||!usb_free::valid(feature_plan,q.token)) {
                    status.state=Failed;status.detail=24;startup_failed=true;continue;
                }
                if(channel.listener<0&&!listen_pcm()){status.state=Failed;status.rc=channel.last_error;status.detail=10;startup_failed=true;continue;}
                generation=0;allowed=wanted=false;active_parent=true;due=0;
            }
            if(session.token&&active_parent) {
                std::uint64_t force_stop=0;
                const bool retired=usb_free::retire_requested(worker_directory,session.token);
                const bool forced=pcm_transport::read_file(worker_directory,"stop.bin",force_stop)&&force_stop==session.token;
                if(!fresh()||retired||forced) {
                    channel.shutdown();
                    (void)stop_owned();active_parent=false;allowed=false;
                    note("SESSION STOP parent_fresh=%u retired=%u requested=%u state=%d source=%d port=%d",unsigned(fresh()),unsigned(retired),unsigned(forced),status.state,status.source,status.port);
                    if(retired) {
                        usb_free::Retirement ack;ack.build=r.build;ack.token=session.token;ack.shell_pid=getpid();ack.shell_epoch=r.shell_epoch;
                        ack.protocol=usb_free::version;ack.source=status.source;
                        usb_free::FeatureProgress progress;
                        ack.cleanup_recorded=!cleanup_poisoned&&status.source<0&&status.port<0&&
                            pcm_transport::read_file(worker_directory,"feature-status.bin",progress)&&usb_free::feature_clean(feature_plan,progress);
                        (void)pcm_transport::write_file(worker_directory,"retired.bin",ack);
                        status.at_ms=now_ms();(void)pcm_transport::write_file(worker_directory,"status.bin",status);
                        for(;;)usleep(1000000);
                    }
                } else {
                    Gate g;
                    if(pcm_transport::read_file(worker_directory,"gate.bin",g)&&g.token==session.token) {
                        if(g.generation!=generation||bool(g.playing)!=allowed){generation=g.generation;allowed=g.playing;receiver.flush();}
                        wanted=g.intent!=0;
                    }
                    CommandRequest c;
                    if(!startup_failed&&pcm_transport::read_file(worker_directory,"command.bin",c)&&c.token==session.token&&c.sequence>status.sequence) {
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
            (void)pcm_transport::write_file(worker_directory,"status.bin",status);
        }
    }
}

#endif
