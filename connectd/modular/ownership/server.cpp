// SPDX-License-Identifier: GPL-3.0-or-later
#include "server.hpp"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <algorithm>
#include <cstdio>

namespace usb_free {
static bool nonblocking(int fd) {
    const auto flags=fcntl(fd,F_GETFL,0);
    bool ok=flags>=0&&fcntl(fd,F_SETFL,flags|O_NONBLOCK)==0;
    if(ok) {const auto applied=fcntl(fd,F_GETFL,0);ok=applied>=0&&(applied&O_NONBLOCK);}
#ifdef __FreeBSD__
    // Ordinary payloads reject fcntl on this console; the receiver validated
    // SO_NBIO (0x1200). Read-back is a nonzero state bit, not necessarily 1.
    if(!ok) {
        int on=1,value=0;socklen_t size=sizeof value;
        ok=setsockopt(fd,SOL_SOCKET,0x1200,&on,sizeof on)==0;
        if(ok&&getsockopt(fd,SOL_SOCKET,0x1200,&value,&size)==0)ok=value!=0;
    }
#endif
    if(!ok)return false;
#ifdef SO_NOSIGPIPE
    int yes=1;
    setsockopt(fd,SOL_SOCKET,SO_NOSIGPIPE,&yes,sizeof yes);
#endif
    return true;
}
void Server::close_client(Client& c) {
    if(c.fd>=0) {
        if(c.headers&&!c.head&&c.delivered<c.end-c.range.first)
            log("HTTP closing incomplete response delivered=%llu expected=%llu backpressured=%u",
                (unsigned long long)c.delivered,(unsigned long long)(c.end-c.range.first),unsigned(c.blocked));
        ::close(c.fd);
    }
    c=Client{};
}
Server::~Server() {for(auto& c:clients)close_client(c);if(listener>=0)::close(listener);}
bool Server::start(std::uint64_t run) {
    token=run;listener=socket(AF_INET,SOCK_STREAM,0);if(listener<0)return false;
    int yes=1;setsockopt(listener,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof yes);
    sockaddr_in a{};
#ifdef __FreeBSD__
    a.sin_len=sizeof a;
#endif
    a.sin_family=AF_INET;a.sin_port=htons(port);a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    // No SO_REUSEPORT: a second test cannot share the working listener.
    if(bind(listener,reinterpret_cast<sockaddr*>(&a),sizeof a)||listen(listener,4))return false;
    return nonblocking(listener);
}
void Server::receive(Client& c,std::uint64_t now) {
    auto n=recv(c.fd,c.input+c.used,sizeof(c.input)-c.used-1,0);
    if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK))return;
    if(n<=0){close_client(c);return;}
    c.used+=std::size_t(n);c.input[c.used]=0;
    if(!std::strstr(c.input,"\r\n\r\n")) {
        if(c.used==sizeof(c.input)-1)close_client(c);
        return;
    }
    const auto result=request_range(c.input,token,c.range,c.head);++requests;
    const auto* first_end=std::strstr(c.input,"\r\n");
    log("HTTP request_line=%.*s",int(std::min<std::size_t>(first_end?std::size_t(first_end-c.input):c.used,200)),c.input);
    c.headers=true;c.offset=c.range.first;c.end=c.range.last+1;
    c.primary=c.range.first==0;
    if(result==HttpOk&&c.primary&&!c.head)++primary_streams;
    int length;
    if(result!=HttpOk) {
        const int status=result==HttpRange?416:result==HttpNotFound?404:result==HttpMethod?405:400;
        length=std::snprintf(reinterpret_cast<char*>(c.pending),sizeof c.pending,
            "HTTP/1.1 %d Rejected\r\nContent-Length: 0\r\nContent-Range: bytes */%llu\r\nConnection: close\r\n\r\n",status,(unsigned long long)audio_bytes);
        c.head=true;
    } else if(c.range.partial) {
        length=std::snprintf(reinterpret_cast<char*>(c.pending),sizeof c.pending,
            "HTTP/1.1 206 Partial Content\r\nContent-Type: audio/wav\r\nContent-Length: %llu\r\n"
            "Content-Range: bytes %llu-%llu/%llu\r\nAccept-Ranges: bytes\r\nConnection: close\r\n\r\n",
            (unsigned long long)(c.end-c.offset),(unsigned long long)c.offset,
            (unsigned long long)c.range.last,(unsigned long long)audio_bytes);
    } else {
        length=std::snprintf(reinterpret_cast<char*>(c.pending),sizeof c.pending,
            "HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\nContent-Length: %llu\r\n"
            "Accept-Ranges: bytes\r\nConnection: close\r\n\r\n",(unsigned long long)audio_bytes);
    }
    c.queued=std::size_t(length);c.sent=0;
    (void)now;
    log("HTTP request=%llu result=%d head=%u range=%llu-%llu primary=%u",
        (unsigned long long)requests,result,c.head,(unsigned long long)c.offset,
        (unsigned long long)c.range.last,c.primary);
}
void Server::flush(Client& c,std::uint64_t now) {
    if(c.sent<c.queued) {
        auto n=send(c.fd,c.pending+c.sent,c.queued-c.sent,
#ifdef MSG_NOSIGNAL
            MSG_NOSIGNAL
#else
            0
#endif
        );
        if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK)) {
            // Native playback can stop reading during Pause or when prefetch
            // buffers are full. EAGAIN is backpressure, not a broken stream.
            // The parent run/stop/heartbeat deadlines bound this response's
            // lifetime; closing it here produces premature EOF on resume.
            if(!c.blocked) {
                c.blocked=true;c.blocked_at=now;
            }
            if(!c.blocked_logged&&now-c.blocked_at>=1000) {
                c.blocked_logged=true;
                log("HTTP backpressure begin delivered=%llu queued=%zu",(unsigned long long)c.delivered,c.queued-c.sent);
            }
            return;
        }
        if(n<=0){log("HTTP send ended rc=%lld errno=%d queued_offset=%llu end=%llu",(long long)n,n<0?errno:0,(unsigned long long)c.offset,(unsigned long long)c.end);close_client(c);return;}
        if(c.blocked) {
            if(c.blocked_logged)log("HTTP backpressure resumed after_ms=%llu delivered=%llu",(unsigned long long)(now-c.blocked_at),(unsigned long long)c.delivered);
            c.blocked=false;c.blocked_logged=false;
        }
        if(c.pending_audio){sent_bytes+=std::uint64_t(n);c.delivered+=std::uint64_t(n);}
        c.sent+=std::size_t(n);
        if(c.sent<c.queued)return;
        c.pending_audio=false;
        c.queued=c.sent=0;
        if(c.head||c.offset>=c.end){log("HTTP response complete head=%u end=%llu",c.head,(unsigned long long)c.end);close_client(c);return;}
    }
    // This finite silent ownership source is synthesized in RAM. Let the
    // decoder prefetch/seek freely; it never supplies the Spotify song clock.
    const auto count=std::size_t(std::min<std::uint64_t>(c.end-c.offset,sizeof c.pending));
    if(!fill_audio(c.offset,c.pending,count)){log("HTTP invalid fixture range");close_client(c);return;}
    c.offset+=count;c.queued=count;c.sent=0;c.pending_audio=true;
}
void Server::tick(std::uint64_t now) {
    for(unsigned i=0;i<4;++i) {
        int fd=accept(listener,nullptr,nullptr);if(fd<0)break;
        Client* slot=nullptr;for(auto& c:clients)if(c.fd<0){slot=&c;break;}
        if(!slot){::close(fd);continue;}
        if(!nonblocking(fd)){::close(fd);continue;}
        slot->fd=fd;slot->opened=now;
    }
    for(auto& c:clients)if(c.fd>=0) {
        if(!c.headers) {
            if(now>c.opened+3000){close_client(c);continue;}
            receive(c,now);
        }
        if(c.fd>=0&&c.headers)flush(c,now);
    }
}
}
