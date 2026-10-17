// SPDX-License-Identifier: GPL-3.0-or-later
#include "ownership/migration.hpp"
#include "ownership/files.hpp"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
using namespace usb_free;
int main() {
    Request q;q.protocol=1;q.size=120;q.build=2;q.token=3;q.shell_pid=150;q.shell_epoch=4;
    for(auto& p:q.functions)p=0x10000;
    Resident r;r.protocol=1;r.build=2;r.shell_pid=150;r.shell_epoch=4;
    Status s;s.protocol=1;s.token=3;s.phase=CleanupFailed;s.reason=MediaError;s.source=27;s.stop_confirmed=1;
    assert(prior_terminal(q,r,s,150,4));
    s.stop_rc=-1;assert(!prior_terminal(q,r,s,150,4));s.stop_rc=0;
    s.remove_rc=-1;assert(!prior_terminal(q,r,s,150,4));s.remove_rc=0;
    s.stop_confirmed=0;assert(!prior_terminal(q,r,s,150,4));s.stop_confirmed=1;
    s.phase=Playing;assert(!prior_terminal(q,r,s,150,4));s.phase=CleanupFailed;
    s.token=4;assert(!prior_terminal(q,r,s,150,4));s.token=3;
    r.build=3;assert(!prior_terminal(q,r,s,150,4));r.build=2;
    assert(!prior_terminal(q,r,s,150,5));
    q.functions[8]=0;assert(!prior_terminal(q,r,s,150,4));q.functions[8]=0x10000;
    q.protocol=3;q.size=128;r.protocol=3;s.protocol=3;s.phase=Retired;
    assert(prior_terminal(q,r,s,150,4));
    q.protocol=4;assert(!prior_terminal(q,r,s,150,4));
    assert(absent(missing_source,missing_source,missing_source,missing_source));
    assert(!absent(missing_source,missing_source,0,missing_source));
    assert(!absent(-1,-1,-1,-1));
    PriorSources batch;batch.token=1;batch.count=1;batch.entries[0]={2,3,27,1};
    assert(valid(batch,1)&&!valid(batch,2));batch.count=9;assert(!valid(batch,1));
    char dir[]="/tmp/usb-free-retire-XXXXXX";assert(mkdtemp(dir));
    char p[256];path(p,sizeof p,dir,"retire.txt");
    FILE* f=std::fopen(p,"w");assert(f);std::fputs("retire 000000000000002a\n",f);assert(!std::fclose(f));
    assert(retire_requested(dir,42)&&!retire_requested(dir,43));
    assert(!unlink(p)&&!rmdir(dir));
    std::puts("PASS generation identity, terminal cleanup qualification, exact native absence codes and tokened retirement flag");
}
