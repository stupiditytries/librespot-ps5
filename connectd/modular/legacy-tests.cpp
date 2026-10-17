// SPDX-License-Identifier: GPL-3.0-or-later
#include "legacy-cleanup.hpp"
#include "protocol.hpp"
#include <cassert>
#include <cstdio>
using namespace pcm_transport;
int main(){
    assert(legacy_v11_unarmed(0x3c607987011438abULL,0,-2,-1,48,10,-1,0,0,true,true,true));
    assert(!legacy_v11_unarmed(0x3c607987011438abULL,1,-2,-1,48,10,-1,0,0,true,true,true));
    assert(!legacy_v11_unarmed(0x3c607987011438abULL,0,-2,1,48,10,-1,0,0,true,true,true));
    assert(!legacy_v11_unarmed(0x3c607987011438abULL,0,-2,-1,48,10,5,0,0,true,true,true));
    assert(!legacy_v11_unarmed(0x3c607987011438abULL,0,-2,-1,48,10,-1,1,0,true,true,true));
    assert(!legacy_v11_unarmed(0x3c607987011438abULL,0,-2,-1,48,10,-1,0,0,false,true,true));
    assert(!legacy_v11_unarmed(0x3c607987011438abULL,0,-2,-1,48,10,-1,0,0,true,false,true));
    assert(!legacy_v11_unarmed(0x3c607987011438abULL,0,-2,-1,48,10,-1,0,0,true,true,false));
    assert(!legacy_v11_unarmed(1,0,-2,-1,48,10,-1,0,0,true,true,true));

    // Actual console record that v9 rejected: USB StoppedOk retains source 16.
    assert(legacy_cleanup_record(3,16,0,0,-1,true));
    assert(!legacy_cleanup_record(3,16,0,0,-1,false));
    assert(legacy_cleanup_record(3,-1,0,0,-1,false));
    assert(legacy_cleanup_record(0,-1,0,0,-1,false));
    assert(!legacy_cleanup_record(0,16,0,0,-1,true));
    assert(!legacy_cleanup_record(2,16,0,0,-1,true));
    assert(!legacy_cleanup_record(4,16,0,0,-1,true));
    assert(!legacy_cleanup_record(3,16,-7,0,-1,true));
    assert(!legacy_cleanup_record(3,16,0,1,-1,true));
    assert(!legacy_cleanup_record(3,-1,0,0,9,false));
    InactiveSources s;s.token=123;s.count=1;s.ids[0]=16;
    assert(valid(s,123)&&!valid(s,124));s.count=9;assert(!valid(s,123));
    std::puts("PASS legacy handover: actual stopped USB ID qualifies for native verification; active, paused, error, PCM-owned ID and open port still refuse");
}
