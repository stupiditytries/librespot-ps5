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
#include "../usb-transport/protocol.hpp"
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <algorithm>
extern "C" {extern const unsigned char home_bridge_blob[];extern const unsigned int home_bridge_blob_size;}
using namespace pcm_transport;
static std::atomic<int> observed{Idle};
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static CommandRequest wanted;
static void note(const char* format,...) {
    char p[256];path(p,sizeof p,directory,"parent.log");FILE* f=std::fopen(p,"a");if(!f)return;
    std::fprintf(f,"[%llu] ",(unsigned long long)now_ms());va_list a;va_start(a,format);
    std::vfprintf(f,format,a);va_end(a);std::fputc('\n',f);std::fclose(f);
}
// Decoder writes are confined to this thread's bounded, nonblocking loopback stream.
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
    while(observed.load()!=Running||!active_token.load()||acknowledged_generation.load()!=generation) {
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
        sockaddr_in a{};a.sin_len=sizeof a;a.sin_family=AF_INET;a.sin_port=htons(pcm_port);a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
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

static bool initialize(Request& q) {
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
    const char* names[]={"GetUsbDeviceListArray","GetUsbFileItemListArray","PlayUriByDeviceId","PlaybackCommand","GetCurrentSrcId",
        "sceMediaFwGetCurrentTime","sceMediaFwIsPlaying","sceMediaFwIsPaused","sceMediaFwIsStopped","sceMediaFwGetMediaError","GetMediaPlaybackInfo","sceAudioOutOpen","sceAudioOutOutput","sceAudioOutClose","sceAudioOutSetVolume","sceAudioOutGetPortState","sceAudioOutGetLastOutputTime","sceAudioOutInit"};
    for(unsigned i=0;i<FunctionCount;++i) {
        const char* lib=i>=Open?"libSceAudioOut.sprx":i<=Current||i==Info?"libReactNative.Modules.Vsh.Media.sprx":"libSceMediaFrameworkInterface.sprx";
        std::uint32_t handle=0;if(kernel_dynlib_handle(q.shell_pid,lib,&handle)<0||!handle)return false;
        auto value=kernel_dynlib_dlsym(q.shell_pid,handle,names[i]);
        if(!value){char hash[12]{};if(nid_encode(names[i],hash))value=kernel_dynlib_resolve(q.shell_pid,handle,hash);}
        if(!address(value)){note("REFUSED export=%s",names[i]);return false;}q.functions[i]=value;
    }
    Resident r;
    const bool existing=read_file(directory,"resident.bin",r)&&r.signature==magic&&r.shell_pid==q.shell_pid&&r.shell_epoch==q.shell_epoch;
    if(existing&&(r.protocol!=version||r.build!=q.build)){note("REFUSED resident build differs; reboot needed for a changed worker");return false;}
    if(!existing) {
        Resident a;if(read_file(directory,"attempt.bin",a)&&a.shell_pid==q.shell_pid&&a.shell_epoch==q.shell_epoch) {
            note("REFUSED unresolved loader attempt; inspect logs before retry");return false;
        }
    }
    if(existing) {
        Status s;if(!read_file(directory,"status.bin",s)||s.port>=0||s.source>0||s.state==Starting||s.state==Running) {
            note("REFUSED previous PCM worker cleanup unconfirmed");return false;
        }
    }
    if(!write_file(directory,"request.bin",q)||!write_file(directory,"heartbeat.bin",Heartbeat{q.token,now_ms()}))return false;
    if(!existing) {
        Resident a;a.build=q.build;a.shell_pid=q.shell_pid;a.shell_epoch=q.shell_epoch;
        if(!write_file(directory,"attempt.bin",a))return false;
        auto loaded=common_fps::ps5::inject_shellui_stable(q.shell_pid,home_bridge_blob,home_bridge_blob_size);
        note("LOADER mapped=%u thread=%u detached=%u auth_restored=%u missing=%u first=%s",loaded.elf_loaded,loaded.pthread_create_ok,
            loaded.detached,loaded.auth_restored,loaded.imports_unresolved,loaded.first_unresolved?loaded.first_unresolved:"");
        if(!loaded.elf_loaded||!loaded.pthread_create_ok||!loaded.detached||!loaded.auth_restored||loaded.imports_unresolved)return false;
    }
    active_token=q.token;
    return write_file(directory,"gate.bin",Gate{q.token,audio_generation.load(),audio_allowed.load()?1:0,service_wanted.load()?1:0})&&
        write_file(directory,"heartbeat.bin",Heartbeat{q.token,now_ms()})&&write_file(directory,"arm.bin",q.token);
}
static int own_lock=-1,shared_lock=-1;
static bool take_lock(int& fd,const char* file) {
    if(fd>=0)return true;
    fd=open(file,O_CREAT|O_RDWR,0644);
    if(fd>=0&&!flock(fd,LOCK_EX|LOCK_NB))return true;
    if(fd>=0)close(fd);fd=-1;return false;
}
static int run_service() {
    if(mkdir(directory,0755)&&errno!=EEXIST)return Failed;
    char p[256];path(p,sizeof p,directory,"parent.lock");
    if(!take_lock(own_lock,p))return Refused;
    // Same exclusion used by the stand-alone audio probes. Keep its fd for this process.
    mkdir("/data/custom-overlays",0755);mkdir("/data/custom-overlays/http-audio-test",0755);
    if(!take_lock(shared_lock,"/data/custom-overlays/http-audio-test/parent.lock")){note("REFUSED audio probe running");return Refused;}
    Request q;if(!initialize(q))return Refused;
    std::uint64_t sent=0,last=now_ms();int previous=INT32_MIN;
    for(;;) {
        if(!write_file(directory,"heartbeat.bin",Heartbeat{q.token,now_ms()})||
           !write_file(directory,"gate.bin",Gate{q.token,audio_generation.load(),audio_allowed.load()?1:0,service_wanted.load()?1:0}))return Failed;
        pthread_mutex_lock(&mutex);auto command=wanted;pthread_mutex_unlock(&mutex);
        command.token=q.token;
        if(command.sequence!=sent) {
            if(!write_file(directory,"command.bin",command))return Failed;
            sent=command.sequence;last=now_ms();observed=Starting;
        }
        Failure f;
        if(read_file(directory,"failure.bin",f)&&f.signature==magic&&f.token==q.token) {
            note("REFUSED worker startup step=%d errno=%d rc=%d; see worker.log, reboot before another attempt",f.step,f.error,f.rc);
            return Refused;
        }
        Status s;
        if(read_file(directory,"status.bin",s)&&s.signature==magic&&s.token==q.token&&s.sequence==sent&&
           s.at_ms<=now_ms()+1000&&now_ms()<=s.at_ms+3000) {
            acknowledged_generation=s.generation;
            observed=audio_failed.load()&&(s.state==Starting||s.state==Running)?Lost:s.state;
            last=now_ms();
            if(previous!=s.state){note("STATE seq=%llu state=%d source=%d rc=%08x detail=%d port=%d frames=%llu underruns=%llu output=%x",(unsigned long long)sent,s.state,s.source,s.rc,s.detail,s.port,(unsigned long long)s.audio_frames,(unsigned long long)s.underruns,s.output);previous=s.state;}
        } else if(now_ms()>last+20000)observed=Failed;
        usleep(100000);
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
    R r;Q q;if(!read_file(dir,"resident.bin",r))return 0;
    if(r.signature!=signature||!read_file(dir,"request.bin",q)||q.signature!=signature||!q.token)return -1;
    int mib[]={CTL_KERN,KERN_PROC,KERN_PROC_PID,r.shell_pid};kinfo_proc info{};std::size_t n=sizeof info;
    if(sysctl(mib,4,&info,&n,nullptr,0)||!n)return 0;
    const auto epoch=std::uint64_t(info.ki_start.tv_sec)*1000000+info.ki_start.tv_usec;
    if(epoch!=r.shell_epoch)return 0;
    const auto began=now_ms();
    do {
        S s;if(read_file(dir,"status.bin",s)&&s.signature==signature&&s.token==q.token&&
            s.at_ms<=now_ms()+1000&&now_ms()<=s.at_ms+3000&&
            (s.state==Idle||s.state==StoppedOk||s.source<=0)) {
            if constexpr(requires{s.port;}){if(s.port>=0)return -1;}
            return 0;
        }
        usleep(100000);
    }while(now_ms()<began+8000);
    return -1;
}
extern "C" int usb_transport_quiesce_previous() {
    if(wait_previous<usb_transport::Resident,usb_transport::Request,usb_transport::Status>(
        usb_transport::directory,usb_transport::magic))return -1;
    if(wait_previous<Resident,Request,Status>("/data/spotifyps5/pcm-transport",0x324d43505053534cULL))return -1;
    if(wait_previous<Resident,Request,Status>("/data/spotifyps5/pcm-transport-v2",0x334d43505053534cULL))return -1;
    if(wait_previous<Resident,Request,Status>("/data/spotifyps5/pcm-transport-v3",0x344d43505053534cULL))return -1;
    if(wait_previous<Resident,Request,Status>("/data/spotifyps5/pcm-transport-v4",0x354d43505053534cULL))return -1;
    if(wait_previous<Resident,Request,Status>("/data/spotifyps5/pcm-transport-v5",0x364d43505053534cULL))return -1;
    return wait_previous<Resident,Request,Status>(directory,magic);
}
extern "C" void usb_transport_request(int action) {
    if(action==1)audio_failed=false;
    pthread_mutex_lock(&mutex);wanted.action=action;++wanted.sequence;observed=Starting;
    if(!service_running) {
        pthread_t thread;
        if(pthread_create(&thread,nullptr,service,nullptr))observed=Failed;
        else {service_running=true;pthread_detach(thread);}
    }
    pthread_mutex_unlock(&mutex);
}
extern "C" int usb_transport_state(){return observed.load();}
