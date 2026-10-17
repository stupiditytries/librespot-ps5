// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "stream.hpp"
#include <cstdarg>
namespace usb_free {
using Logger=void(*)(const char*,...);
class Server {
    struct Client {
        int fd=-1;
        char input[4096]{};std::size_t used=0;
        unsigned char pending[16384]{};std::size_t queued=0,sent=0;
        Range range;
        std::uint64_t offset=0,end=0,blocked_at=0,opened=0,delivered=0;
        bool headers=false,head=false,primary=false,pending_audio=false,blocked=false,blocked_logged=false;
    } clients[4];
    int listener=-1;
    std::uint64_t token=0;
    Logger log;
    void close_client(Client&);
    void receive(Client&,std::uint64_t);
    void flush(Client&,std::uint64_t);
public:
    std::uint64_t sent_bytes=0,requests=0,primary_streams=0;
    explicit Server(Logger logger):log(logger) {}
    ~Server();
    bool start(std::uint64_t);
    void tick(std::uint64_t);
};
}
