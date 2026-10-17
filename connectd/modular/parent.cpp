// SPDX-License-Identifier: GPL-3.0-or-later
#include "protocol.hpp"
#include "files.hpp"
#include "stable_shellui_injector.hpp"
#include <ps5/kernel.h>
#include <ps5/nid.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <atomic>
#include <cstdlib>
#include <cstdarg>
#include <cerrno>
#include "socket.hpp"
#include "usb-transport/protocol.hpp"
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <algorithm>
#include <sys/stat.h>
#include <dirent.h>
#include <type_traits>
#include "legacy-cleanup.hpp"
#include "service-memory.hpp"
extern "C" {extern const unsigned char home_bridge_blob[];extern const unsigned int home_bridge_blob_size;}
using namespace pcm_transport;
static std::atomic<int> observed{Idle};
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static CommandRequest wanted;
static void note(const char* format,...) {
    char p[256];path(p,sizeof p,directory,"parent.log");FILE* f=std::fopen(p,"a");if(!f)return;
    if(!std::fseek(f,0,SEEK_END)&&std::ftell(f)>2*1024*1024){std::fclose(f);f=std::fopen(p,"w");if(!f)return;}
    std::fprintf(f,"[%llu] ",(unsigned long long)now_ms());va_list a;va_start(a,format);
    std::vfprintf(f,format,a);va_end(a);std::fputc('\n',f);std::fclose(f);
}
#include "ownership/server.hpp"
#include "mode-parent.hpp"
#include "bootstrap.hpp"
#include "ownership/migrate-parent.hpp"
static std::atomic<std::uint64_t> pending_bootstrap{0};
static std::atomic<bool> shutdown_requested{false},shutdown_clean{false},receiver_exiting{false};
static InactiveSources startup_inactive;
// Decoder writes are confined to this thread's bounded, nonblocking loopback stream.
static std::atomic<unsigned> active_pcm_port{0};
static std::atomic<std::uint64_t> active_token{0},audio_generation{1},acknowledged_generation{0};
static std::atomic<bool> audio_allowed{false},audio_failed{false},service_wanted{false};
static pthread_mutex_t audio_mutex=PTHREAD_MUTEX_INITIALIZER;
static int audio_socket=-1;
static std::uint64_t socket_generation=0;
static Pacer producer;
static std::uint64_t microseconds(){timespec t{};clock_gettime(CLOCK_MONOTONIC,&t);return std::uint64_t(t.tv_sec)*1000000+t.tv_nsec/1000;}
extern "C" void pcm_transport_intent(int playing){service_wanted=playing!=0;}

// Never consume faster than real time. The player sees a Pause only between
// packets, and instant discards let it decode minutes ahead (v5 time skips).
static int discard(std::size_t samples) {
    const auto end=microseconds()+std::min<std::uint64_t>(samples/2*1000000ULL/pcm_rate,100000);
    for(auto now=microseconds();now<end;now=microseconds())usleep(unsigned(std::min<std::uint64_t>(end-now,2000)));
    return 0;
}
static void drop_socket() {if(audio_socket>=0)close(audio_socket);audio_socket=-1;producer.reset();}
// A new generation drops the connection and queued audio, so only a real
// change does that: v3 bumped twice per resume (intent, then sink start) and
// reset the fresh connection every time.
extern "C" void pcm_transport_gate(int playing) {
    if(audio_allowed.exchange(playing!=0)!=(playing!=0))++audio_generation;
}
// Seek: discard audio already queued for the old position.
extern "C" void pcm_transport_flush(){++audio_generation;}
extern "C" std::uint64_t pcm_transport_generation(){return audio_generation.load();}
extern "C" int pcm_transport_write(const std::int16_t* samples,std::size_t count,std::uint64_t generation) {
    if(!samples||count%2)return -1;
    pthread_mutex_lock(&audio_mutex);
    auto finish=[&](int rc){
        if(rc<0) {
            note("PCM transport failure code=%d; pausing Spotify",rc);
            audio_failed=true;observed=Lost;drop_socket();
            const auto deadline=now_ms()+2000;
            while(audio_allowed.load()&&audio_generation.load()==generation&&now_ms()<deadline)usleep(5000);
            // An unavailable output is not a bad Spotify track. The event loop
            // observes Lost and pauses; do not trigger automatic track skipping.
            rc=0;
        }
        pthread_mutex_unlock(&audio_mutex);return rc;
    };
    auto cancelled=[&]{return !audio_allowed.load()||audio_generation.load()!=generation;};
    if(cancelled()){drop_socket();finish(0);return discard(count);}
    if(socket_generation!=generation){drop_socket();socket_generation=generation;}
    const auto began=now_ms();
    while(observed.load()!=Running||!active_token.load()||acknowledged_generation.load()!=generation||!active_pcm_port.load()) {
        if(cancelled()){finish(0);return discard(count);}
        if(observed.load()<0||now_ms()>began+15000)return finish(-2);
        usleep(5000);
    }
    if(audio_socket<0) {
        audio_socket=socket(AF_INET,SOCK_STREAM,0);
        if(audio_socket<0)return finish(-3);
        int yes=1,buffer=socket_buffer;
        setsockopt(audio_socket,IPPROTO_TCP,TCP_NODELAY,&yes,sizeof yes);
        setsockopt(audio_socket,SOL_SOCKET,SO_SNDBUF,&buffer,sizeof buffer);
        if(setsockopt(audio_socket,SOL_SOCKET,SO_NOSIGPIPE,&yes,sizeof yes)){drop_socket();return finish(-4);}
        sockaddr_in a{};a.sin_len=sizeof a;a.sin_family=AF_INET;a.sin_port=htons(active_pcm_port.load());a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        errno=0;
        if(connect(audio_socket,reinterpret_cast<sockaddr*>(&a),sizeof a)){note("PCM connect errno=%d",errno);drop_socket();return finish(-5);}
        SocketTrace t;
        if(!nonblocking(audio_socket,t)){char d[320];t.describe(d,sizeof d);note("PCM sender nonblocking %s",d);drop_socket();return finish(-5);}
    }
    unsigned char bytes[sizeof(Packet)+packet_frames*4];
    for(std::size_t offset=0;offset<count;) {
        const unsigned frames=unsigned(std::min<std::size_t>((count-offset)/2,packet_frames));
        Packet h{active_token.load(),generation,now_ms(),frames,wire_magic};
        std::memcpy(bytes,&h,sizeof h);std::memcpy(bytes+sizeof h,samples+offset,frames*4);
        const unsigned length=sizeof h+frames*4;unsigned sent=0;const auto at=now_ms();
        while(sent<length) {
            if(cancelled()){drop_socket();finish(0);return discard(count-offset);}
            if(observed.load()!=Running||now_ms()>at+3000){
                note("PCM send timeout/state=%d sent=%u length=%u errno=%d generation=%llu",observed.load(),sent,length,errno,(unsigned long long)generation);
                drop_socket();return finish(-6);
            }
            int n=send(audio_socket,bytes+sent,length-sent,MSG_DONTWAIT);
            if(n>0)sent+=unsigned(n);
            else if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR))usleep(2000);
            else {drop_socket();return finish(-7);}
        }
        offset+=frames*2;
        const auto due=producer.advance(frames,microseconds(),lead_frames);
        for(auto now=microseconds();now<due;now=microseconds()) {
            if(cancelled()){drop_socket();finish(0);return discard(count-offset);}
            usleep(unsigned(std::min<std::uint64_t>(due-now,2000)));
        }
    }
    return finish(0);
}

static bool initialize(Request& q,ModeCoordinator& mode,usb_free::Server& server) {
    std::uint32_t sdk=0;std::size_t n=sizeof sdk;
    if(sysctlbyname("kern.sdk_version",&sdk,&n,nullptr,0)||n!=sizeof sdk||(sdk&0xffff0000U)!=0x13200000U) {
        note("REFUSED unverified firmware sdk=%08x",sdk);return false;
    }
    int mib[]={CTL_KERN,KERN_PROC,KERN_PROC_PROC,0};n=0;
    if(sysctl(mib,4,nullptr,&n,nullptr,0)||!n||n>16*1024*1024)return false;
    auto* data=static_cast<unsigned char*>(std::malloc(n));if(!data)return false;
    if(!sysctl(mib,4,data,&n,nullptr,0))for(std::size_t i=0;i+sizeof(kinfo_proc)<=n;) {
        auto* p=reinterpret_cast<kinfo_proc*>(data+i);
        if(p->ki_structsize<int(sizeof(kinfo_proc))||std::size_t(p->ki_structsize)>n-i)break;
        if(!std::strcmp(p->ki_comm,"SceShellUI")) {
            q.shell_pid=p->ki_pid;q.shell_epoch=std::uint64_t(p->ki_start.tv_sec)*1000000+p->ki_start.tv_usec;break;
        }
        i+=p->ki_structsize;
    }
    std::free(data);if(q.shell_pid<=1||!q.shell_epoch)return false;
    q.build=PCM_TRANSPORT_BUILD;q.token=(now_ms()<<16)^std::uint64_t(getpid());
    const char* names[]={"sceMediaFwAddSrc","sceMediaFwPlay","sceMediaFwStop","sceMediaFwRemoveSrc","GetCurrentSrcId",
        "sceMediaFwGetCurrentTime","sceMediaFwIsPlaying","sceMediaFwIsPaused","sceMediaFwIsStopped","sceMediaFwGetMediaError","sceMediaFwPause","sceAudioOutOpen","sceAudioOutOutput","sceAudioOutClose","sceAudioOutSetVolume","sceAudioOutGetPortState","sceAudioOutGetLastOutputTime","sceAudioOutInit","sceMediaFwSetCurrentTime","sceMediaFwLoad"};
    for(unsigned i=0;i<FunctionCount;++i) {
        const char* lib=i>=Open&&i<=Init?"libSceAudioOut.sprx":i==Current?"libReactNative.Modules.Vsh.Media.sprx":"libSceMediaFrameworkInterface.sprx";
        std::uint32_t handle=0;if(kernel_dynlib_handle(q.shell_pid,lib,&handle)<0||!handle)return false;
        auto value=kernel_dynlib_dlsym(q.shell_pid,handle,names[i]);
        if(!value){char hash[12]{};if(nid_encode(names[i],hash))value=kernel_dynlib_resolve(q.shell_pid,handle,hash);}
        if(!address(value)){note("REFUSED export=%s",names[i]);return false;}q.functions[i]=value;
    }
    Resident r;
    const bool existing=pcm_transport::read_file(directory,"resident.bin",r)&&r.signature==magic&&r.shell_pid==q.shell_pid&&r.shell_epoch==q.shell_epoch;
    if(existing&&!compatible_resident(r.protocol,r.build,q.build)){note("REFUSED resident build differs; use a separate generation");return false;}
    if(existing&&r.build!=q.build) {
        note("Reusing known compatible v12 resident; waiting for current-token cleanup status");q.build=r.build;
    }
    if(!existing) {
        Resident a;if(pcm_transport::read_file(directory,"attempt.bin",a)&&a.shell_pid==q.shell_pid&&a.shell_epoch==q.shell_epoch) {
            note("REFUSED unresolved loader attempt; inspect logs before retry");return false;
        }
    }
    if(existing) {
        Request previous;
        if(!pcm_transport::read_file(directory,"request.bin",previous)||!valid(previous,q.shell_pid,q.build)||previous.shell_epoch!=q.shell_epoch||
           usb_free::retire_requested(directory,previous.token)){note("REFUSED prior request invalid or retirement pending");return false;}
        // The original v12 bootstrap may overwrite this file every 100ms while
        // the PCM worker publishes every 500ms. Only its exact current token can
        // prove cleanup; a stale bootstrap Idle record never authorizes reuse.
        const auto deadline=now_ms()+2000;bool clean=false;
        do {
            Status s;
            if(pcm_transport::read_file(directory,"status.bin",s)&&previous_clean(s,previous.token)){clean=true;break;}
            usleep(10000);
        }while(now_ms()<deadline);
        if(!clean){note("REFUSED previous PCM worker cleanup unconfirmed");return false;}

    }
    usb_free::PriorSources prior;prior.token=q.token;
    if(!usb_free::collect_generations(q.shell_pid,q.shell_epoch,prior))return false;
    Resident old_resident;
    if(pcm_transport::read_file(directory,"resident.bin",old_resident)&&old_resident.shell_pid==q.shell_pid&&old_resident.shell_epoch==q.shell_epoch) {
        usb_free::FeaturePlan old_plan;usb_free::FeatureProgress old_progress;
        BootstrapStatus old_boot;Request old_request;std::uint64_t old_arm=0;
        const bool bootstrap_only=pcm_transport::read_file(directory,"request.bin",old_request)&&
            valid(old_request,q.shell_pid,q.build)&&old_request.shell_epoch==q.shell_epoch&&
            pcm_transport::read_file(directory,"bootstrap-status.bin",old_boot)&&bootstrap_clean(old_boot,old_request.token)&&
            (!pcm_transport::read_file(directory,"arm.bin",old_arm)||old_arm!=old_request.token);
        if(!bootstrap_only&&(!pcm_transport::read_file(directory,"feature-plan.bin",old_plan)||!pcm_transport::read_file(directory,"feature-status.bin",old_progress)||
           !usb_free::valid(old_plan,old_plan.token)||!usb_free::feature_clean(old_plan,old_progress))) {
            note("REFUSED previous native AudioTag restoration unconfirmed");return false;
        }
        usb_free::Retirement retired;
        if(pcm_transport::read_file(directory,"retired.bin",retired)&&retired.shell_pid==q.shell_pid&&retired.shell_epoch==q.shell_epoch) {
            note("REFUSED this worker was retired; use another generation");return false;
        }
    }
    const bool policy_ready=mode.start(q.shell_pid,q.token);
    const bool cold=!policy_ready&&mode.needs_bootstrap();
    if(!policy_ready&&!cold){note("REFUSED native AudioTag identity/plan");return false;}
    if(!server.start(q.token)){note("REFUSED ownership HTTP port busy errno=%d",errno);return false;}
    if(!pcm_transport::write_file(directory,"prior.bin",prior))return false;
    startup_inactive.token=q.token;
    if(!valid(startup_inactive,q.token)||!pcm_transport::write_file(directory,"inactive-sources.bin",startup_inactive))return false;
    note("USB-FREE ownership HTTP loopback port=%u seconds=%u; Spotify audio remains MAIN_PCM",usb_free::port,usb_free::stream_seconds);
    if(!pcm_transport::write_file(directory,"request.bin",q)||!pcm_transport::write_file(directory,"heartbeat.bin",Heartbeat{q.token,now_ms()}))return false;
    if(!existing) {
        Resident a;a.build=q.build;a.shell_pid=q.shell_pid;a.shell_epoch=q.shell_epoch;
        if(!pcm_transport::write_file(directory,"attempt.bin",a))return false;
        auto loaded=common_fps::ps5::inject_shellui_stable(q.shell_pid,home_bridge_blob,home_bridge_blob_size);
        note("LOADER mapped=%u thread=%u detached=%u auth_restored=%u missing=%u first=%s",loaded.elf_loaded,loaded.pthread_create_ok,
            loaded.detached,loaded.auth_restored,loaded.imports_unresolved,loaded.first_unresolved?loaded.first_unresolved:"");
        if(!loaded.elf_loaded||!loaded.pthread_create_ok||!loaded.detached||!loaded.auth_restored||loaded.imports_unresolved)return false;
    }
    if(cold) {
        note("BOOTSTRAP missing verified music core; bounded Add/Load only, no Play");
        pending_bootstrap=q.token;
        if(!pcm_transport::write_file(directory,"bootstrap.bin",BootstrapCommand{q.token,1,0})){pending_bootstrap=0;return false;}
        const auto began=now_ms();bool prepared=false,finish=false,clean=false;
        std::uint64_t probed=0,heartbeat_at=0;
        do {
            const auto now=now_ms();server.tick(now);
            if(now-heartbeat_at>=100) {
                heartbeat_at=now;
                if(!pcm_transport::write_file(directory,"heartbeat.bin",Heartbeat{q.token,now}))return false;
            }
            BootstrapStatus boot;
            if(pcm_transport::read_file(directory,"bootstrap-status.bin",boot)&&boot.signature==magic&&
               boot.token==q.token&&boot.at_ms<=now+1000&&now<=boot.at_ms+3000) {
                if(boot.phase==BootLoaded&&!finish) {
                    if(now-probed>=250) {probed=now;prepared=mode.start(q.shell_pid,q.token);}
                    if(prepared||now-began>=6000||shutdown_requested.load()) {
                        finish=true;
                        if(!pcm_transport::write_file(directory,"bootstrap.bin",BootstrapCommand{q.token,2,0}))return false;
                    }
                }
                if(boot.phase==BootClean||boot.phase==BootFailed) {
                    clean=bootstrap_clean(boot,q.token);
                    if(clean)pending_bootstrap=0;
                    note("BOOTSTRAP result phase=%d clean=%u source=%d rc=%08x policy_prepared=%u",
                        boot.phase,unsigned(clean),boot.source,boot.rc,unsigned(prepared));
                    break;
                }
            }
            if(shutdown_requested.load()&&!finish) {
                finish=true;(void)pcm_transport::write_file(directory,"bootstrap.bin",BootstrapCommand{q.token,2,0});
            }
            usleep(5000);
        }while(now_ms()-began<12000);
        if(!prepared||!clean||shutdown_requested.load()||!mode.start(q.shell_pid,q.token)) {
            note("REFUSED cold native startup/cleanup; no PCM armed");return false;
        }
        note("BOOTSTRAP CONFIRMED own_source_removed=1 no_audio_port=1 exact_policy_sample=1");
    }
    active_pcm_port=0;active_token=q.token;
    return pcm_transport::write_file(directory,"gate.bin",Gate{q.token,audio_generation.load(),audio_allowed.load()?1:0,service_wanted.load()?1:0})&&
        pcm_transport::write_file(directory,"heartbeat.bin",Heartbeat{q.token,now_ms()})&&pcm_transport::write_file(directory,"arm.bin",q.token);
}
static int own_lock=-1,shared_lock=-1;
static bool take_lock(int& fd,const char* file) {
    if(fd>=0)return true;
    fd=open(file,O_CREAT|O_RDWR,0644);
    if(fd>=0&&!flock(fd,LOCK_EX|LOCK_NB))return true;
    if(fd>=0)close(fd);fd=-1;return false;
}
static int run_service() {
    note("SERVICE entered; allocating HTTP state on heap");
    if(mkdir(directory,0755)&&errno!=EEXIST)return Failed;
    char p[256];path(p,sizeof p,directory,"parent.lock");
    if(!take_lock(own_lock,p))return Refused;
    // Same exclusion used by the stand-alone audio probes. Keep its fd for this process.
    mkdir("/data/custom-overlays",0755);mkdir("/data/custom-overlays/http-audio-test",0755);
    if(!take_lock(shared_lock,"/data/custom-overlays/http-audio-test/parent.lock")){note("REFUSED audio probe running");return Refused;}
    ModeCoordinator mode(note);auto server=allocate_ownership_server(note);
    if(!server){note("REFUSED ownership HTTP allocation bytes=%zu",sizeof(usb_free::Server));return Failed;}
    Request q;if(!initialize(q,mode,*server))return Refused;
    std::uint64_t sent=0,last=now_ms(),controlled=0,stop_at=0;int previous=INT32_MIN;
    bool stopping=false;
    for(;;) {
        const auto now=now_ms();server->tick(now);
        if(now-controlled>=100) {
            controlled=now;
            if(!pcm_transport::write_file(directory,"heartbeat.bin",Heartbeat{q.token,now})||
               !pcm_transport::write_file(directory,"gate.bin",Gate{q.token,audio_generation.load(),audio_allowed.load()?1:0,service_wanted.load()?1:0}))return Failed;
            if(!mode.tick(now))shutdown_requested=true;
            if(shutdown_requested.load()&&!stopping) {
                stopping=true;stop_at=now;audio_allowed=false;service_wanted=false;
                if(!pcm_transport::write_file(directory,"stop.bin",q.token))return Failed;
                note("SHUTDOWN waiting for owned source, PCM port and exact policy cleanup");
            }
            if(!stopping) {
                pthread_mutex_lock(&mutex);auto command=wanted;pthread_mutex_unlock(&mutex);
                command.token=q.token;
                if(command.sequence!=sent) {
                    Status old;
                    if(command.action==1&&mode.clean()&&pcm_transport::read_file(directory,"status.bin",old)&&
                       old.token==q.token&&old.source<0&&old.port<0&&old.state!=Starting&&old.state!=Running&&old.state!=Pausing) {
                        if(!mode.start(q.shell_pid,q.token)){shutdown_requested=true;continue;}
                    }
                    if(!pcm_transport::write_file(directory,"command.bin",command))return Failed;
                    sent=command.sequence;last=now;observed=Starting;
                }
            }
            Failure f;
            if(pcm_transport::read_file(directory,"failure.bin",f)&&f.signature==magic&&f.token==q.token) {
                note("REFUSED worker startup step=%d errno=%d rc=%d; inspect worker.log",f.step,f.error,f.rc);return Refused;
            }
            Status s;
            if(pcm_transport::read_file(directory,"status.bin",s)&&s.signature==magic&&s.token==q.token&&
               status_for_command(s,sent,stopping)&&s.at_ms<=now+1000&&now<=s.at_ms+3000) {
                acknowledged_generation=s.generation;
                if(s.state==Running) {
                    Endpoint endpoint;
                    if(!pcm_transport::read_file(directory,"endpoint.bin",endpoint)||!valid(endpoint,q)) {
                        active_pcm_port=0;observed=Failed;shutdown_requested=true;
                        note("REFUSED PCM endpoint identity/token; stopping own session");continue;
                    }
                    active_pcm_port=endpoint.port;
                }else active_pcm_port=0;
                // A startup failure has sequence zero: report it immediately
                // rather than waiting for a command that was never processed.
                if(s.state<0)shutdown_requested=true;
                observed=audio_failed.load()&&(s.state==Starting||s.state==Running)?Lost:s.state;
                last=now;
                if(previous!=s.state){note("STATE seq=%llu state=%d source=%d rc=%08x detail=%d port=%d frames=%llu underruns=%llu output=%x",(unsigned long long)s.sequence,s.state,s.source,s.rc,s.detail,s.port,(unsigned long long)s.audio_frames,(unsigned long long)s.underruns,s.output);previous=s.state;}
                if(stopping&&s.state==StoppedOk&&s.source<0&&s.port<0&&mode.clean()) {
                    shutdown_clean=true;note("SHUTDOWN CONFIRMED source_removed=1 PCM_closed=1 exact_policy_restored=1");
                    return receiver_exiting.load()?StoppedOk:Lost;
                }
            } else if(now>last+20000){observed=Failed;shutdown_requested=true;}
            if(stopping&&now-stop_at>12000){note("SHUTDOWN cleanup unconfirmed; worker restoration still required");return Failed;}
        }
        usleep(5000);
    }
}
// The service thread ends on any terminal failure. A later request starts it
// again so state is re-derived (the loader guards refuse a second injection),
// instead of reporting Starting forever as pthread_once did.
static std::atomic<bool> service_running{false};
static void* service(void*) {
    const int state=run_service();
    // Publish the terminal state and the restart permission together, so a
    // request racing this exit cannot leave Starting with no thread behind it.
    pthread_mutex_lock(&mutex);service_running=false;observed=state;pthread_mutex_unlock(&mutex);
    return nullptr;
}
// A receiver replacement must wait for the old worker's parent-loss Stop before
// Stream::new rewrites the file. No injection or media commands during this wait.
template<class R,class Q,class S>
static int wait_previous(const char* dir,std::uint64_t signature) {
    R r;Q q;if(!pcm_transport::read_file(dir,"resident.bin",r)){note("LEGACY no resident directory=%s",dir);return 0;}
    if(r.signature!=signature||!pcm_transport::read_file(dir,"request.bin",q)||q.signature!=signature||!q.token) {
        note("LEGACY REFUSED request/resident mismatch directory=%s expected_size=%zu",dir,sizeof(Q));return -1;
    }
    int mib[]={CTL_KERN,KERN_PROC,KERN_PROC_PID,r.shell_pid};std::size_t n=0;
    if(sysctl(mib,4,nullptr,&n,nullptr,0)){note("LEGACY process size-query pid=%d errno=%d directory=%s",r.shell_pid,errno,dir);return errno==ESRCH||errno==ENOENT?0:-1;}
    if(!n)return 0;
    if(n>16*1024*1024)return -1;
    auto* bytes=static_cast<unsigned char*>(std::malloc(n));if(!bytes)return -1;
    const int rc=sysctl(mib,4,bytes,&n,nullptr,0);
    kinfo_proc first{};if(!rc&&n>=sizeof first)std::memcpy(&first,bytes,sizeof first);
    const auto actual_epoch=std::uint64_t(first.ki_start.tv_sec)*1000000+first.ki_start.tv_usec;
    const bool well_formed=!rc&&usb_free::process_records_match<kinfo_proc>(bytes,n,r.shell_pid,actual_epoch);
    std::free(bytes);if(!well_formed){note("LEGACY REFUSED process response pid=%d bytes=%zu rc=%d directory=%s",r.shell_pid,n,rc,dir);return -1;}
    if(actual_epoch!=r.shell_epoch) {
        note("LEGACY process start time differs pid=%d; old worker belongs to an earlier process",r.shell_pid);return 0;
    }
    const auto began=now_ms();S last{};bool recorded=false;
    do {
        S s;if(pcm_transport::read_file(dir,"status.bin",s)&&s.signature==signature&&s.token==q.token&&
            s.at_ms<=now_ms()+1000&&now_ms()<=s.at_ms+3000) {
            last=s;recorded=true;int port=-1;
            if constexpr(requires{s.port;})port=s.port;
            constexpr bool retains_id=std::is_same_v<S,usb_transport::Status>;
            if constexpr(std::is_same_v<Q,Request>&&std::is_same_v<S,Status>) {
                if(!std::strcmp(dir,"/data/spotifyps5/pcm-transport-v11")&&signature==0x3c4d43505053534cULL&&
                   q.protocol==version&&r.protocol==version&&q.size==sizeof(Request)&&q.build==r.build&&q.shell_pid==r.shell_pid&&q.shell_epoch==r.shell_epoch) {
                    BootstrapStatus boot;usb_free::FeaturePlan plan;usb_free::FeatureProgress feature;Heartbeat hb;
                    const bool boot_clean=pcm_transport::read_file(dir,"bootstrap-status.bin",boot)&&boot.signature==signature&&
                        boot.token==q.token&&boot.phase==BootClean&&boot.source==-1&&!boot.rc&&boot.clean==1;
                    const bool untouched=pcm_transport::read_file(dir,"feature-plan.bin",plan)&&usb_free::valid(plan,q.token)&&
                        pcm_transport::read_file(dir,"feature-status.bin",feature)&&feature.signature==usb_free::feature_magic&&
                        feature.token==q.token&&feature.phase==usb_free::FeatureIdle;
                    const auto now=now_ms();
                    const bool parent_gone=pcm_transport::read_file(dir,"heartbeat.bin",hb)&&hb.token==q.token&&
                        hb.at_ms<=now&&now-hb.at_ms>=3000;
                    if(legacy_v11_unarmed(r.build,s.sequence,s.state,s.source,s.rc,s.detail,s.port,s.audio_frames,s.underruns,
                            boot_clean,untouched,parent_gone)) {
                        note("LEGACY v11 confirmed pre-arm bind failure; bootstrap removed, AudioTag untouched, PCM never opened");
                        return 0;
                    }
                }
            }
            if(legacy_cleanup_record(s.state,s.source,s.rc,s.detail,port,retains_id)) {
                if(s.source>0) {
                    if(startup_inactive.count==8)return -1;
                    startup_inactive.ids[startup_inactive.count++]=s.source;
                    note("LEGACY stopped USB metadata source=%d directory=%s; live native stop/absence check still required",s.source,dir);
                } else note("LEGACY clean state=%d source=%d port=%d directory=%s",s.state,s.source,port,dir);
                return 0;
            }
        }
        usleep(20000);
    }while(now_ms()<began+8000);
    note("LEGACY REFUSED cleanup directory=%s status_matched=%u state=%d source=%d rc=%08x detail=%d",dir,unsigned(recorded),last.state,last.source,last.rc,last.detail);
    return -1;
}
extern "C" int usb_transport_quiesce_previous() {
    if(mkdir(directory,0755)&&errno!=EEXIST)return -1;
    note("STARTUP USB-free PCM generation 12: checking previous workers before Spotify Connect starts");
    if(wait_previous<usb_transport::Resident,usb_transport::Request,usb_transport::Status>(
        usb_transport::directory,usb_transport::magic))return -1;
    if(wait_previous<Resident,LegacyRequest,Status>("/data/spotifyps5/pcm-transport",0x324d43505053534cULL))return -1;
    if(wait_previous<Resident,LegacyRequest,Status>("/data/spotifyps5/pcm-transport-v2",0x334d43505053534cULL))return -1;
    if(wait_previous<Resident,LegacyRequest,Status>("/data/spotifyps5/pcm-transport-v3",0x344d43505053534cULL))return -1;
    if(wait_previous<Resident,LegacyRequest,Status>("/data/spotifyps5/pcm-transport-v4",0x354d43505053534cULL))return -1;
    if(wait_previous<Resident,LegacyRequest,Status>("/data/spotifyps5/pcm-transport-v5",0x364d43505053534cULL))return -1;
    if(wait_previous<Resident,LegacyRequest,Status>("/data/spotifyps5/pcm-transport-v6",0x374d43505053534cULL))return -1;
    if(wait_previous<Resident,PreviousRequest,Status>("/data/spotifyps5/pcm-transport-v7",0x384d43505053534cULL))return -1;
    if(wait_previous<Resident,PreviousRequest,Status>("/data/spotifyps5/pcm-transport-v8",0x394d43505053534cULL))return -1;
    if(wait_previous<Resident,PreviousRequest,Status>("/data/spotifyps5/pcm-transport-v9",0x3a4d43505053534cULL))return -1;
    if(wait_previous<Resident,Request,Status>("/data/spotifyps5/pcm-transport-v10",0x3b4d43505053534cULL))return -1;
    if(wait_previous<Resident,Request,Status>("/data/spotifyps5/pcm-transport-v11",0x3c4d43505053534cULL))return -1;
    return wait_previous<Resident,Request,Status>(directory,magic);
}
extern "C" void usb_transport_request(int action) {
    if(receiver_exiting.load())return;
    if(action==1)audio_failed=false;
    pthread_mutex_lock(&mutex);
    if(receiver_exiting.load()){pthread_mutex_unlock(&mutex);return;}
    wanted.action=action;++wanted.sequence;observed=Starting;
    if(!service_running) {
        if(action==1){shutdown_requested=false;shutdown_clean=false;}
        pthread_t thread;pthread_attr_t attributes;
        int rc=pthread_attr_init(&attributes);
        if(!rc) {
            std::size_t previous_stack=0;(void)pthread_attr_getstacksize(&attributes,&previous_stack);
            rc=pthread_attr_setstacksize(&attributes,service_stack_bytes);
            note("SERVICE thread default_stack=%zu requested_stack=%zu HTTP_heap_bytes=%zu attr_rc=%d",
                previous_stack,service_stack_bytes,sizeof(usb_free::Server),rc);
            if(!rc)rc=pthread_create(&thread,&attributes,service,nullptr);
            (void)pthread_attr_destroy(&attributes);
        }
        if(rc){note("SERVICE thread creation failed rc=%d",rc);observed=Failed;}
        else {service_running=true;pthread_detach(thread);}
    }
    pthread_mutex_unlock(&mutex);
}
extern "C" int usb_transport_state(){return observed.load();}

extern "C" int pcm_transport_shutdown() {
    receiver_exiting=true;
    pthread_mutex_lock(&mutex);shutdown_requested=true;pthread_mutex_unlock(&mutex);
    pcm_transport_intent(0);pcm_transport_gate(0);
    const auto began=now_ms();
    if(!active_token.load()&&!service_running.load()&&!pending_bootstrap.load())return 0;
    do {
        if(shutdown_clean.load())return 0;
        const auto boot_token=pending_bootstrap.load();
        if(boot_token) {
            BootstrapStatus boot;
            if(pcm_transport::read_file(directory,"bootstrap-status.bin",boot)&&bootstrap_clean(boot,boot_token)) {
                pending_bootstrap=0;
                note("SHUTDOWN bootstrap own_source_removed=1 no_PCM_or_policy_started=1");
                if(!active_token.load()&&!service_running.load())return 0;
            }
        } else if(!service_running.load())return active_token.load()?-1:0;
        usleep(50000);
    }while(now_ms()-began<14000);
    return -1;
}
