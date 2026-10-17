// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
namespace usb_free {
// Kernel records can contain firmware extensions beyond the SDK structure.
// Accept their declared stride, but never partial records or a different process.
template<class Record> bool process_records_match(const void* data,std::size_t size,int pid,std::uint64_t epoch) {
    if(!data||!size||pid<=1||!epoch)return false;
    const auto* bytes=static_cast<const unsigned char*>(data);
    for(std::size_t at=0;at<size;) {
        if(size-at<sizeof(Record))return false;
        Record record{};std::memcpy(&record,bytes+at,sizeof record);
        if(record.ki_structsize<int(sizeof(Record))||std::size_t(record.ki_structsize)>size-at||
           record.ki_pid!=pid||record.ki_start.tv_sec<0||record.ki_start.tv_usec<0||record.ki_start.tv_usec>=1000000||
           std::uint64_t(record.ki_start.tv_sec)*1000000+record.ki_start.tv_usec!=epoch)return false;
        at+=record.ki_structsize;
    }
    return true;
}
}
