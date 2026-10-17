// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "ownership/server.hpp"
#include <memory>
#include <new>
namespace pcm_transport {
constexpr std::size_t service_stack_bytes=256*1024;
using OwnershipServer=std::unique_ptr<usb_free::Server>;
inline OwnershipServer allocate_ownership_server(usb_free::Logger log) {
    // Four clients carry >80 KiB of buffers. Never put these on a pthread's
    // default stack; v10 faulted at its first call after reserving this frame.
    return OwnershipServer(new(std::nothrow) usb_free::Server(log));
}
}
