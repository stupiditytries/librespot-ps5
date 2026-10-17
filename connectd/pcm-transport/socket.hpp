// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "ring.hpp"
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstdio>
namespace pcm_transport {
constexpr int so_nbio=0x1200;
// Each way. With the ring this bounds latency near 150 ms, so TCP backpressure
// follows the AudioOut clock; 64 KB each way let v3 drift past 500 ms.
constexpr int socket_buffer=8192;
// Each setup call's own result. v2 printed global errno after cleanup and
// could not say which call failed; a console failure must name its step.
struct SocketTrace {
    const char* step="none";int fd=-1,rc=0,error=0;
    const char* method="none";
    int flags=-1,fcntl_error=0,nbio=-1,nbio_size=-1,nbio_error=0,reuse_error=0,rcvbuf_error=0;
    bool fail(const char* name,int descriptor,int result){error=errno;step=name;fd=descriptor;rc=result;return false;}
    int describe(char* out,std::size_t size) const {
        return std::snprintf(out,size,"step=%s fd=%d rc=%d errno=%d method=%s flags=%x fcntl_errno=%d nbio=%d nbio_len=%d nbio_errno=%d reuse_errno=%d rcvbuf_errno=%d",
            step,fd,rc,error,method,unsigned(flags),fcntl_error,nbio,nbio_size,nbio_error,reuse_error,rcvbuf_error);
    }
};
struct SystemSockets {
    int get_flags(int fd){return fcntl(fd,F_GETFL,0);}
    int set_flags(int fd,int flags){return fcntl(fd,F_SETFL,flags);}
#ifdef __FreeBSD__
    int set_nbio(int fd,int on){return setsockopt(fd,SOL_SOCKET,so_nbio,&on,sizeof on);}
    int get_nbio(int fd,int* value,socklen_t* size){return getsockopt(fd,SOL_SOCKET,so_nbio,value,size);}
#else
    int set_nbio(int,int){errno=ENOPROTOOPT;return -1;}
    int get_nbio(int,int*,socklen_t*){errno=ENOPROTOOPT;return -1;}
#endif
};
// v1 proved fcntl O_NONBLOCK inside ShellUI. Ordinary payloads answer EACCES
// there, so fall back to the console's SO_NBIO option as compat_socket.c does.
// SO_NBIO's read-back is the kernel's state bit, not necessarily 1 (v2 failed
// requiring exactly 1 with errno=0): only a definite zero is a refusal.
template<class Ops> bool make_nonblocking(Ops& ops,int fd,SocketTrace& t) {
    t.fd=fd;errno=0;
    const int flags=ops.get_flags(fd);t.flags=flags;
    if(flags<0)t.fcntl_error=errno;
    else {
        errno=0;
        if(ops.set_flags(fd,flags|O_NONBLOCK))t.fcntl_error=errno;
        else {
            errno=0;const int applied=ops.get_flags(fd);t.flags=applied;
            if(applied>=0&&(applied&O_NONBLOCK)){t.method="fcntl";return true;}
            t.fcntl_error=applied<0?errno:-1; // Accepted, but not reported as applied.
        }
    }
    errno=0;
    if(ops.set_nbio(fd,1))return t.fail("setsockopt SO_NBIO",fd,-1);
    int value=0;socklen_t size=sizeof value;errno=0;
    if(ops.get_nbio(fd,&value,&size)){t.nbio_error=errno;t.method="SO_NBIO unread";return true;}
    t.nbio=value;t.nbio_size=int(size);
    if(!value){errno=0;return t.fail("SO_NBIO read-back zero",fd,0);}
    t.method="SO_NBIO";return true;
}
inline bool nonblocking(int fd,SocketTrace& t){SystemSockets ops;return make_nonblocking(ops,fd,t);}
inline bool nonblocking(int fd){SocketTrace t;return nonblocking(fd,t);}
inline sockaddr_in endpoint(unsigned port) {
    sockaddr_in a{};
#ifdef __FreeBSD__
    a.sin_len=sizeof a;
#endif
    a.sin_family=AF_INET;a.sin_port=htons(port);a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);return a;
}
inline bool would_block(){return errno==EAGAIN||errno==EWOULDBLOCK;}
struct SocketReader {
    int listener=-1,client=-1,last_error=0;
    std::uint64_t received_bytes=0,receives=0,empty_reads=0,connections=0,refused_clients=0;
    SocketTrace trace,client_trace;
    Receiver receiver;
    void disconnect(){if(client>=0)close(client);client=-1;receiver.clear();}
    void shutdown(){disconnect();if(listener>=0)close(listener);listener=-1;}
    bool refuse(){last_error=trace.error;shutdown();return false;}
    bool listen_on(unsigned port=pcm_port) {
        trace=SocketTrace{};errno=0;
        listener=socket(AF_INET,SOCK_STREAM,0);
        if(listener<0){trace.fail("socket",-1,listener);listener=-1;return refuse();}
        // Advisory options: record their results without failing on them.
        int yes=1,buffer=socket_buffer;errno=0;
        if(setsockopt(listener,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof yes))trace.reuse_error=errno?errno:-1;
        errno=0;
        if(setsockopt(listener,SOL_SOCKET,SO_RCVBUF,&buffer,sizeof buffer))trace.rcvbuf_error=errno?errno:-1;
        auto a=endpoint(port);errno=0;
        int rc=bind(listener,reinterpret_cast<sockaddr*>(&a),sizeof a);
        if(rc){trace.fail("bind",listener,rc);return refuse();}
        errno=0;rc=listen(listener,1);
        if(rc){trace.fail("listen",listener,rc);return refuse();}
        if(!nonblocking(listener,trace))return refuse();
        trace.step="listening";return true;
    }
    bool receive(std::uint64_t token,std::uint64_t generation,std::uint64_t now,bool allowed) {
        if(client<0) {
            client=accept(listener,nullptr,nullptr);
            if(client>=0) {
                client_trace=SocketTrace{};
                if(!nonblocking(client,client_trace)){++refused_clients;last_error=client_trace.error;disconnect();return false;}
                receiver.clear();++connections;
            } else if(!would_block()&&errno!=EINTR){last_error=errno;return false;}
        }
        if(client<0)return true;
        for(unsigned count=0;count<8;++count) {
            if(!receiver.parse(token,generation,now,allowed)){last_error=EPROTO;return false;}
            // Do not read ahead of a full audio ring. TCP provides backpressure.
            if(receiver.ring.size>capacity-packet_frames||receiver.used==sizeof receiver.bytes)return true;
            int n=recv(client,receiver.bytes+receiver.used,sizeof receiver.bytes-receiver.used,MSG_DONTWAIT);
            ++receives;
            if(n>0){receiver.used+=unsigned(n);received_bytes+=unsigned(n);}
            else if(n==0)return false;
            else if(would_block()){++empty_reads;return true;}
            else if(errno!=EINTR){last_error=errno;return false;}
        }
        return receiver.parse(token,generation,now,allowed);
    }
};
// Bound the producer to real audio time, so a fast decoder cannot queue an
// entire song into kernel buffers or give the phone a rapidly advancing clock.
struct Pacer {
    std::uint64_t epoch=0,frames=0,due=0;
    void reset(){epoch=frames=due=0;}
    // lead: frames allowed ahead of real time, a cushion for decoder hiccups.
    std::uint64_t advance(unsigned count,std::uint64_t now_us,unsigned lead=0) {
        if(!epoch||now_us>due+100000){epoch=now_us;frames=0;}
        frames+=count;due=epoch+(frames>lead?frames-lead:0)*1000000/pcm_rate;return due;
    }
};
}
