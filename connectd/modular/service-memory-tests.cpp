// SPDX-License-Identifier: GPL-3.0-or-later
#include "service-memory.hpp"
#include "ownership/files.hpp"
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cassert>
#include <cstring>
#include <cstdio>
static void discard(const char*,...){}
static void* small_stack(void*) {
    auto server=pcm_transport::allocate_ownership_server(discard);
    assert(server&&server->start(42));
    int fd=socket(AF_INET,SOCK_STREAM,0);assert(fd>=0);
    sockaddr_in a{};a.sin_family=AF_INET;a.sin_port=htons(usb_free::port);a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    assert(!connect(fd,reinterpret_cast<sockaddr*>(&a),sizeof a));
    const char request[]="HEAD /probe-000000000000002a.wav HTTP/1.1\r\n\r\n";
    assert(send(fd,request,sizeof request-1,0)==sizeof request-1);
    assert(!fcntl(fd,F_SETFL,fcntl(fd,F_GETFL)|O_NONBLOCK));
    char response[512]{};std::size_t used=0;bool closed=false;
    const auto start=usb_free::now_ms();
    while(usb_free::now_ms()-start<3000) {
        server->tick(usb_free::now_ms());
        const auto n=recv(fd,response+used,sizeof response-used-1,0);
        if(n>0)used+=std::size_t(n);else if(!n){closed=true;break;}
        usleep(1000);
    }
    close(fd);assert(closed&&std::strstr(response,"HTTP/1.1 200 OK")&&std::strstr(response,"Content-Length: 23040044"));
    return nullptr;
}
int main() {
    static_assert(sizeof(usb_free::Server)>64*1024);
    static_assert(sizeof(pcm_transport::OwnershipServer)<=16);
    pthread_attr_t attr;assert(!pthread_attr_init(&attr));
    assert(!pthread_attr_setstacksize(&attr,64*1024));
    pthread_t thread;assert(!pthread_create(&thread,&attr,small_stack,nullptr));
    assert(!pthread_attr_destroy(&attr));assert(!pthread_join(thread,nullptr));
    std::printf("PASS ownership server on 64 KiB thread: %zu-byte HTTP state stays on heap; configured console service stack %zu bytes\n",sizeof(usb_free::Server),pcm_transport::service_stack_bytes);
}
