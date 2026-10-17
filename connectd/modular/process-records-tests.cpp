// SPDX-License-Identifier: GPL-3.0-or-later
#include "ownership/process-records.hpp"
#include <cassert>
#include <cstdio>
struct Record {int ki_structsize,ki_pid;struct {long tv_sec,tv_usec;} ki_start;};
int main() {
    using usb_free::process_records_match;
    constexpr auto epoch=123456789ULL;
    Record r{sizeof(Record),280,{123,456789}};
    assert(process_records_match<Record>(&r,sizeof r,280,epoch));
    unsigned char extended[sizeof r+64]{};
    r.ki_structsize=sizeof extended;std::memcpy(extended,&r,sizeof r);
    assert(process_records_match<Record>(extended,sizeof extended,280,epoch));
    assert(!process_records_match<Record>(extended,sizeof extended-1,280,epoch));
    assert(!process_records_match<Record>(extended,sizeof extended,281,epoch));
    assert(!process_records_match<Record>(extended,sizeof extended,280,epoch+1));
    assert(!process_records_match<Record>(extended,sizeof r-1,280,epoch));
    assert(!process_records_match<Record>(nullptr,0,280,epoch));
    r.ki_structsize=0;std::memcpy(extended,&r,sizeof r);
    assert(!process_records_match<Record>(extended,sizeof extended,280,epoch));
    r.ki_structsize=sizeof extended;r.ki_start.tv_usec=1000000;std::memcpy(extended,&r,sizeof r);
    assert(!process_records_match<Record>(extended,sizeof extended,280,epoch));
    std::puts("process records: extended firmware stride accepted; truncation, PID/epoch mismatch and malformed records refused");
}
