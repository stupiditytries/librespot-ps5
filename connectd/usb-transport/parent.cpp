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
extern "C" {extern const unsigned char home_bridge_blob[];extern const unsigned int home_bridge_blob_size;}
using namespace usb_transport;
static std::atomic<int> observed{Idle};
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t once=PTHREAD_ONCE_INIT;
static CommandRequest wanted;
static void note(const char* format,...) {
    char p[256];path(p,sizeof p,directory,"parent.log");FILE* f=std::fopen(p,"a");if(!f)return;
    std::fprintf(f,"[%llu] ",(unsigned long long)now_ms());va_list a;va_start(a,format);
    std::vfprintf(f,format,a);va_end(a);std::fputc('\n',f);std::fclose(f);
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
    q.build=USB_TRANSPORT_BUILD;q.token=(now_ms()<<16)^std::uint64_t(getpid());
    const char* names[]={"GetUsbDeviceListArray","GetUsbFileItemListArray","PlayUriByDeviceId","PlaybackCommand","GetCurrentSrcId",
        "sceMediaFwGetCurrentTime","sceMediaFwIsPlaying","sceMediaFwIsPaused","sceMediaFwIsStopped","sceMediaFwGetMediaError","GetMediaPlaybackInfo"};
    for(unsigned i=0;i<FunctionCount;++i) {
        const char* lib=i<=Current||i==Info?"libReactNative.Modules.Vsh.Media.sprx":"libSceMediaFrameworkInterface.sprx";
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
    if(!write_file(directory,"request.bin",q)||!write_file(directory,"heartbeat.bin",Heartbeat{q.token,now_ms()}))return false;
    if(!existing) {
        Resident a;a.build=q.build;a.shell_pid=q.shell_pid;a.shell_epoch=q.shell_epoch;
        if(!write_file(directory,"attempt.bin",a))return false;
        auto loaded=common_fps::ps5::inject_shellui_stable(q.shell_pid,home_bridge_blob,home_bridge_blob_size);
        note("LOADER mapped=%u thread=%u detached=%u auth_restored=%u missing=%u first=%s",loaded.elf_loaded,loaded.pthread_create_ok,
            loaded.detached,loaded.auth_restored,loaded.imports_unresolved,loaded.first_unresolved?loaded.first_unresolved:"");
        if(!loaded.elf_loaded||!loaded.pthread_create_ok||!loaded.detached||!loaded.auth_restored||loaded.imports_unresolved)return false;
    }
    return write_file(directory,"heartbeat.bin",Heartbeat{q.token,now_ms()})&&write_file(directory,"arm.bin",q.token);
}
static void* service(void*) {
    if(mkdir(directory,0755)&&errno!=EEXIST){observed=Failed;return nullptr;}
    char p[256];path(p,sizeof p,directory,"parent.lock");int lock=open(p,O_CREAT|O_RDWR,0644);
    if(lock<0||flock(lock,LOCK_EX|LOCK_NB)){if(lock>=0)close(lock);observed=Refused;return nullptr;}
    // Same exclusion used by the stand-alone audio probes. Keep its fd for this process.
    mkdir("/data/custom-overlays",0755);mkdir("/data/custom-overlays/http-audio-test",0755);
    int shared=open("/data/custom-overlays/http-audio-test/parent.lock",O_CREAT|O_RDWR,0644);
    if(shared<0||flock(shared,LOCK_EX|LOCK_NB)){note("REFUSED audio probe running");observed=Refused;return nullptr;}
    Request q;if(!initialize(q)){observed=Refused;return nullptr;}
    std::uint64_t sent=0,last=now_ms();int previous=INT32_MIN;
    for(;;) {
        if(!write_file(directory,"heartbeat.bin",Heartbeat{q.token,now_ms()})){observed=Failed;return nullptr;}
        pthread_mutex_lock(&mutex);auto command=wanted;pthread_mutex_unlock(&mutex);
        command.token=q.token;
        if(command.sequence!=sent) {
            if(!write_file(directory,"command.bin",command)){observed=Failed;return nullptr;}
            sent=command.sequence;last=now_ms();observed=Starting;
        }
        Status s;
        if(read_file(directory,"status.bin",s)&&s.signature==magic&&s.token==q.token&&s.sequence==sent&&
           s.at_ms<=now_ms()+1000&&now_ms()<=s.at_ms+3000) {
            observed=s.state;last=now_ms();
            if(previous!=s.state){note("STATE seq=%llu state=%d source=%d rc=%08x detail=%d",(unsigned long long)sent,s.state,s.source,s.rc,s.detail);previous=s.state;}
        } else if(now_ms()>last+20000)observed=Failed;
        usleep(100000);
    }
}
static void launch() {
    pthread_t thread;if(pthread_create(&thread,nullptr,service,nullptr)){observed=Failed;return;}
    pthread_detach(thread);
}
// A receiver replacement must wait for the old worker's parent-loss Stop before
// Stream::new rewrites the file. No injection or media commands during this wait.
extern "C" int usb_transport_quiesce_previous() {
    Resident resident;Request previous;
    if(!read_file(directory,"resident.bin",resident))return 0;
    if(!read_file(directory,"request.bin",previous)||!valid(previous,resident.shell_pid,resident.build))return -1;
    const auto began=now_ms();
    do {
        Status s;
        if(read_file(directory,"status.bin",s)&&s.signature==magic&&s.token==previous.token&&
           s.at_ms<=now_ms()+1000&&now_ms()<=s.at_ms+3000&&(s.state==Idle||s.state==StoppedOk||s.source<=0))return 0;
        usleep(100000);
    } while(now_ms()<began+8000);
    // A resident record from a previous boot has no live worker or current source.
    // Shell identity is checked in initialize; use a cheap pid/start query here too.
    int mib[]={CTL_KERN,KERN_PROC,KERN_PROC_PID,resident.shell_pid};kinfo_proc info{};std::size_t n=sizeof info;
    if(sysctl(mib,4,&info,&n,nullptr,0)||!n)return 0;
    const auto epoch=std::uint64_t(info.ki_start.tv_sec)*1000000+info.ki_start.tv_usec;
    if(epoch!=resident.shell_epoch)return 0;
    return -1;
}
extern "C" void usb_transport_request(int action) {
    pthread_mutex_lock(&mutex);wanted.action=action;++wanted.sequence;pthread_mutex_unlock(&mutex);
    observed=Starting;pthread_once(&once,launch);
}
extern "C" int usb_transport_state(){return observed.load();}
