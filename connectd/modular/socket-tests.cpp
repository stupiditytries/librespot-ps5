// Exercise the actual worker reader/pacer over TCP, not a parser-only mock.
#include "protocol.hpp"
#include "socket.hpp"
#include "files.hpp"
#include <cassert>
#include <atomic>
#include <thread>
#include <chrono>
#include <cstdio>
#include <cstring>
using namespace pcm_transport;
static std::uint64_t now_us(){return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
static void sleep_until(std::uint64_t due){while(now_us()<due)usleep(unsigned(std::min<std::uint64_t>(due-now_us(),1000)));}
static int connect_to(SocketReader& channel) {
    sockaddr_in address{};socklen_t size=sizeof address;
    assert(!getsockname(channel.listener,reinterpret_cast<sockaddr*>(&address),&size));
    assert(channel.bound_port==ntohs(address.sin_port)&&channel.bound_port>=1024);
    int fd=socket(AF_INET,SOCK_STREAM,0);assert(fd>=0);
    int yes=1,small=8192;assert(!setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&yes,sizeof yes));
    assert(!setsockopt(fd,SOL_SOCKET,SO_SNDBUF,&small,sizeof small));
    assert(!connect(fd,reinterpret_cast<sockaddr*>(&address),sizeof address));assert(nonblocking(fd));return fd;
}
static void send_all(int fd,const void* bytes,unsigned count){
    auto* p=static_cast<const unsigned char*>(bytes);auto deadline=now_us()+3000000;
    while(count){int n=send(fd,p,count,MSG_DONTWAIT|MSG_NOSIGNAL);
        if(n>0){p+=n;count-=n;}
        else {assert(n<0&&(would_block()||errno==EINTR));assert(now_us()<deadline);usleep(500);}
    }
}
static std::int16_t sample(unsigned frame){return std::int16_t(frame%30000+1);}
static void pacer_lead() {
    Pacer p;const std::uint64_t t=1000000;
    assert(p.advance(lead_frames,t,lead_frames)==t);          // the lead is sent at once
    assert(p.advance(pcm_rate/10,t,lead_frames)==t+100000);   // then real time
    Pacer q;assert(q.advance(pcm_rate/10,t)==t+100000);         // no lead: real time
}
// The console's decision path, with its system calls scripted. Host sockets
// only exercise fcntl; these cases cover ShellUI and ordinary-payload answers.
struct ScriptedSockets {
    int flags=2,getfl_error=0,setfl_error=0,setopt_error=0,getopt_error=0,nbio_value=0;bool fcntl_applies=true;
    int set_nbio_calls=0;
    int get_flags(int){if(getfl_error){errno=getfl_error;return -1;}return flags;}
    int set_flags(int,int f){if(setfl_error){errno=setfl_error;return -1;}if(fcntl_applies)flags=f;return 0;}
    int set_nbio(int,int){++set_nbio_calls;if(setopt_error){errno=setopt_error;return -1;}return 0;}
    int get_nbio(int,int* v,socklen_t* n){if(getopt_error){errno=getopt_error;return -1;}*v=nbio_value;*n=sizeof *v;return 0;}
};
static void scripted_nonblocking_cases() {
    {ScriptedSockets s;SocketTrace t;assert(make_nonblocking(s,7,t)&&!std::strcmp(t.method,"fcntl")&&!s.set_nbio_calls&&(t.flags&O_NONBLOCK));}
    // Ordinary payload: fcntl refused, SO_NBIO read back as the state bit.
    {ScriptedSockets s;s.setfl_error=EACCES;s.nbio_value=0x100;SocketTrace t;
     assert(make_nonblocking(s,7,t)&&!std::strcmp(t.method,"SO_NBIO")&&t.nbio==0x100&&t.fcntl_error==EACCES);}
    {ScriptedSockets s;s.setfl_error=EACCES;s.nbio_value=1;SocketTrace t;assert(make_nonblocking(s,7,t));}
    // fcntl accepted but not applied must not be trusted.
    {ScriptedSockets s;s.fcntl_applies=false;s.nbio_value=0x100;SocketTrace t;
     assert(make_nonblocking(s,7,t)&&s.set_nbio_calls==1&&t.fcntl_error==-1);}
    {ScriptedSockets s;s.getfl_error=EBADF;s.setopt_error=EBADF;SocketTrace t;
     assert(!make_nonblocking(s,7,t)&&!std::strcmp(t.step,"setsockopt SO_NBIO")&&t.error==EBADF&&t.fcntl_error==EBADF&&t.fd==7);}
    {ScriptedSockets s;s.setfl_error=EACCES;s.nbio_value=0;SocketTrace t;
     assert(!make_nonblocking(s,7,t)&&!std::strcmp(t.step,"SO_NBIO read-back zero")&&!t.error);}
    {ScriptedSockets s;s.setfl_error=EACCES;s.getopt_error=ENOPROTOOPT;SocketTrace t;
     assert(make_nonblocking(s,7,t)&&!std::strcmp(t.method,"SO_NBIO unread")&&t.nbio_error==ENOPROTOOPT);}
    char text[320];SocketTrace t;assert(t.describe(text,sizeof text)>0&&std::strstr(text,"step=none"));
}
static void listen_failure_is_named() {
    SocketReader first,second;assert(first.listen_on(0));
    sockaddr_in address{};socklen_t size=sizeof address;
    assert(!getsockname(first.listener,reinterpret_cast<sockaddr*>(&address),&size));
    assert(!second.listen_on(ntohs(address.sin_port)));
    assert(!std::strcmp(second.trace.step,"bind")&&second.trace.error==EADDRINUSE&&second.last_error==EADDRINUSE&&second.listener<0);
    assert(!std::strcmp(first.trace.method,"fcntl")&&!std::strcmp(first.trace.step,"listening"));
    first.shutdown();
}
static void endpoint_identity_and_collision() {
    // Hold the same fixed port used by the real app artwork proxy. The native
    // reader's default must choose a different, kernel-assigned loopback port.
    int art=socket(AF_INET,SOCK_STREAM,0);assert(art>=0);
    auto fixed=endpoint(38577);
    const int art_bind=bind(art,reinterpret_cast<sockaddr*>(&fixed),sizeof fixed);
    if(art_bind)assert(errno==EADDRINUSE);else assert(!listen(art,1));
    SocketReader pcm;assert(pcm.listen_on()&&pcm.bound_port>=1024&&pcm.bound_port!=38577);
    Request q;q.build=9;q.token=123;q.shell_pid=58;q.shell_epoch=456;
    Endpoint e;e.token=q.token;e.build=q.build;e.shell_pid=q.shell_pid;e.shell_epoch=q.shell_epoch;e.port=pcm.bound_port;
    assert(valid(e,q));
    auto reject=[&](Endpoint bad){assert(!valid(bad,q));};
    auto bad=e;bad.token++;reject(bad);bad=e;bad.build++;reject(bad);bad=e;bad.shell_epoch++;reject(bad);
    bad=e;bad.shell_pid++;reject(bad);bad=e;bad.signature^=1;reject(bad);bad=e;bad.port=0;reject(bad);
    bad=e;bad.reserved=1;reject(bad);
    // The sender uses the published endpoint, not a fixed port.
    int fd=socket(AF_INET,SOCK_STREAM,0);assert(fd>=0);auto selected=endpoint(e.port);
    assert(!connect(fd,reinterpret_cast<sockaddr*>(&selected),sizeof selected));
    assert(pcm.receive(q.token,1,now_ms(),true));assert(pcm.client>=0);close(fd);
    pcm.shutdown();close(art);assert(!pcm.bound_port);
    std::puts("PASS PCM endpoint: artwork port occupied, kernel-selected loopback, actual sender connect, stale token/build/PID/epoch/port rejected");
}
int main(){
    endpoint_identity_and_collision();
    scripted_nonblocking_cases();listen_failure_is_named();pacer_lead();
    SocketReader channel;assert(channel.listen_on(0));
    auto start=now_us();
    for(int i=0;i<1000;++i)assert(channel.receive(123,7,now_ms(),true));
    assert(now_us()-start<500000); // An idle producer cannot block AudioOut.
    int sender=connect_to(channel);
    const unsigned total=pcm_rate*4;
    std::atomic<bool> finished{false};
    std::thread producer([&]{
        Pacer pace;unsigned frame=0;unsigned char bytes[sizeof(Packet)+packet_frames*4];
        while(frame<total){
            unsigned count=std::min(packet_frames,total-frame);
            Packet h{123,7,now_ms(),count,wire_magic};std::memcpy(bytes,&h,sizeof h);
            for(unsigned i=0;i<count;++i){std::int16_t pair[]={sample(frame+i),std::int16_t(-sample(frame+i))};std::memcpy(bytes+sizeof h+i*4,pair,4);}
            // Deliberate fragmentation of the first packet exercises real recv.
            if(!frame){send_all(sender,bytes,13);usleep(2000);send_all(sender,bytes+13,sizeof h+count*4-13);}
            else send_all(sender,bytes,sizeof h+count*4);
            frame+=count;sleep_until(pace.advance(count,now_us()));
        }
        finished=true;
    });
    unsigned consumed=0;std::int16_t out[pcm_grain*2];Pacer clock;bool stalled=false;
    start=now_us();
    while(consumed<total){
        assert(now_us()<start+6000000);
        if(!stalled&&consumed>=pcm_rate){usleep(100000);stalled=true;}
        assert(channel.receive(123,7,now_ms(),true));
        assert(channel.receiver.ring.size<=capacity);
        unsigned got=channel.receiver.ring.pop(out);
        for(unsigned i=0;i<got;++i){assert(out[i*2]==sample(consumed+i));assert(out[i*2+1]==-sample(consumed+i));}
        consumed+=got;sleep_until(clock.advance(pcm_grain,now_us()));
    }
    producer.join();assert(finished&&now_us()-start>=3900000);
    // No data while connected must still return immediately.
    start=now_us();for(int i=0;i<1000;++i)assert(channel.receive(123,7,now_ms(),true));
    assert(now_us()-start<500000&&channel.empty_reads>0);
    close(sender);assert(!channel.receive(123,7,now_ms(),true));channel.disconnect();
    // Reconnect with a new seek/resume generation; old packets are discarded.
    sender=connect_to(channel);Packet stale{123,7,now_ms(),1,wire_magic},fresh{123,8,now_ms(),1,wire_magic};
    std::int16_t pair[2]={1200,-1200};
    send_all(sender,&stale,sizeof stale);send_all(sender,pair,sizeof pair);
    send_all(sender,&fresh,sizeof fresh);send_all(sender,pair,sizeof pair);
    const auto deadline=now_us()+500000;
    while(!channel.receiver.ring.size){assert(now_us()<deadline);assert(channel.receive(123,8,now_ms(),true));usleep(500);}
    assert(channel.receiver.ring.pop(out)==1&&out[0]==1200&&out[1]==-1200);
    close(sender);channel.shutdown();
    std::puts("Real TCP PCM test passed: 7 scripted console nonblocking cases, named bind failure, 192000 ordered stereo frames, tiny buffers, fragmented packets, 100 ms consumer stall, idle nonblocking I/O and new generation reconnect");
}
