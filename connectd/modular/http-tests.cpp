// SPDX-License-Identifier: GPL-3.0-or-later
#include "ownership/server.hpp"
#include "ownership/files.hpp"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cassert>
#include <cstring>
#include <string>
#include <cstdio>
#include <algorithm>
using namespace usb_free;
static void discard(const char*,...) {}
static std::string fetch(Server& server,const char* request,bool pause_reader=false) {
    int fd=socket(AF_INET,SOCK_STREAM,0);assert(fd>=0);
    if(pause_reader){int size=65536;assert(!setsockopt(fd,SOL_SOCKET,SO_RCVBUF,&size,sizeof size));}
    sockaddr_in a{};a.sin_family=AF_INET;a.sin_port=htons(port);a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    assert(connect(fd,reinterpret_cast<sockaddr*>(&a),sizeof a)==0);
    assert(send(fd,request,std::strlen(request),0)==ssize_t(std::strlen(request)));
    fcntl(fd,F_SETFL,fcntl(fd,F_GETFL)|O_NONBLOCK);
    std::string result;auto start=now_ms();bool closed=false;
    std::uint64_t simulated_pause=0;
    if(pause_reader) {
        const auto before=server.sent_bytes;auto last=before;unsigned stalled=0;
        // Fill real TCP buffers without reading. Hold logical time fixed while
        // reaching backpressure, then simulate a pause longer than the old 2s
        // deadline. This must keep the response intact when the reader resumes.
        for(unsigned i=0;i<20000&&stalled<200;++i) {
            server.tick(start);
            if(server.sent_bytes==last)++stalled;else {stalled=0;last=server.sent_bytes;}
            if(i%128==0)usleep(1000);
        }
        assert(stalled==200&&server.sent_bytes>before&&server.sent_bytes-before<audio_bytes);
        simulated_pause=10000;server.tick(start+simulated_pause);
    }
    while(now_ms()-start<15000) {
        server.tick(now_ms()+simulated_pause);char buffer[65536];auto n=recv(fd,buffer,sizeof buffer,0);
        if(n>0)result.append(buffer,std::size_t(n));
        else if(n==0){closed=true;break;}
        usleep(1000);
    }
    close(fd);assert(closed);return result;
}
static std::string body(const std::string& s){return s.substr(s.find("\r\n\r\n")+4);}
int main(int argc,char** argv) {
    Server server(discard);assert(server.start(1));
    Server duplicate(discard);assert(!duplicate.start(1));
    auto head=fetch(server,"HEAD /probe-0000000000000001.wav HTTP/1.1\r\n\r\n");
    assert(head.starts_with("HTTP/1.1 200")&&body(head).empty()&&head.find("Content-Type: audio/wav")!=std::string::npos);
    auto range=fetch(server,"GET /probe-0000000000000001.wav HTTP/1.1\r\nRange: bytes=40-63\r\n\r\n");
    assert(range.starts_with("HTTP/1.1 206")&&body(range).size()==24);
    const auto cross=body(range);std::uint32_t data_size=0;std::memcpy(&data_size,cross.data(),4);
    assert(data_size==23040000&&std::all_of(cross.begin()+4,cross.end(),[](char c){return c==0;}));
    auto suffix=fetch(server,"GET /probe-0000000000000001.wav HTTP/1.1\r\nRange: bytes=-16\r\n\r\n");
    assert(body(suffix)==std::string(16,'\0'));
    auto pcm=fetch(server,"GET /probe-0000000000000001.wav HTTP/1.1\r\nRange: bytes=44-99\r\n\r\n");
    assert(body(pcm)==std::string(56,'\0'));
    auto bad=fetch(server,"GET /probe-0000000000000001.wav HTTP/1.1\r\nRange: bytes=9-2\r\n\r\n");
    assert(bad.starts_with("HTTP/1.1 416")&&body(bad).empty());
    auto stale=fetch(server,"GET /probe-0000000000000002.wav HTTP/1.1\r\n\r\n");
    assert(stale.starts_with("HTTP/1.1 404")&&body(stale).empty());
    auto full=body(fetch(server,"GET /probe-0000000000000001.wav HTTP/1.1\r\n\r\n"));
    assert(full.size()==23040044&&full.starts_with("RIFF")&&full.substr(8,8)=="WAVEfmt "&&full.substr(36,4)=="data");
    assert(std::all_of(full.begin()+44,full.end(),[](char c){return c==0;}));
    assert(cross==full.substr(40,24));
    unsigned char guard[8];std::memset(guard,0x55,sizeof guard);
    assert(!fill_audio(audio_bytes-2,guard,3)&&!fill_audio(UINT64_MAX,guard,1));
    for(auto b:guard)assert(b==0x55);
    assert(fill_audio(audio_bytes,guard,0)&&!fill_audio(audio_bytes+1,guard,0));
    const auto resumed=body(fetch(server,"GET /probe-0000000000000001.wav HTTP/1.1\r\n\r\n",true));
    assert(resumed==full);
    if(argc==2){FILE* f=std::fopen(argv[1],"wb");assert(f);assert(std::fwrite(full.data(),1,full.size(),f)==full.size());assert(!std::fclose(f));}
    std::puts("PASS real HTTP: complete silent WAV, paused/backpressured reader resumes intact after 10s, HEAD, header/PCM ranges, suffix, stale token, invalid range, port collision, fixture bounds");
}
