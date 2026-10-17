// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "protocol.hpp"
namespace usb_free {
// Canonical finite RIFF/PCM WAV. Only its 44-byte header is stored; the parent
// synthesizes zero PCM for any requested range without a file or large buffer.
bool fill_audio(std::uint64_t offset,unsigned char* output,std::size_t count);
struct Range { std::uint64_t first=0,last=audio_bytes-1; bool partial=false; };
enum HttpResult { HttpOk, HttpBad, HttpNotFound, HttpRange, HttpMethod };
// Accepts a complete bounded HTTP request. HEAD and one bytes range are supported.
HttpResult request_range(const char* input,std::uint64_t token,Range&,bool& head);
}
