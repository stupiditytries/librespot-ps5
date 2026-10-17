// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
namespace pcm_transport {
// Only the old USB protocol retains a stopped source ID. The ID is queued for
// independent native confirmation before playback, not accepted as native proof.
inline bool legacy_cleanup_record(int state,int source,int rc,int detail,int port,bool usb_retains_id) {
    if(rc||detail||port>=0)return false;
    if(state==0)return source<=0;
    return state==3&&(source<=0||usb_retains_id);
}
inline bool legacy_v11_unarmed(std::uint64_t build,std::uint64_t sequence,int state,int source,
        int rc,int detail,int port,std::uint64_t frames,std::uint64_t underruns,
        bool bootstrap_clean,bool untouched_feature,bool parent_gone) {
    return build==0x3c607987011438abULL&&sequence==0&&state==-2&&source==-1&&
        rc==48&&detail==10&&port==-1&&!frames&&!underruns&&
        bootstrap_clean&&untouched_feature&&parent_gone;
}
}
