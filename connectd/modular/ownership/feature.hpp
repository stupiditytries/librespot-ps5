// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "protocol.hpp"
namespace usb_free {
constexpr std::uint64_t feature_magic=0x31544741454c4641ULL;
enum FeaturePhase : std::uint32_t { FeatureIdle,FeatureEntering,FeatureActive,FeatureRestoring,FeatureRestored,FeatureBroken };
struct FeaturePlan {
    std::uint64_t signature=feature_magic,token=0,setter=0;
    std::uint32_t size=sizeof(FeaturePlan),protocol=1;
    std::int32_t original=-1,original_mode=-1,app_id=0,reserved=0;
};
struct FeatureProgress {
    std::uint64_t signature=feature_magic,token=0,at_ms=0;
    std::uint32_t phase=FeatureIdle;
    std::int32_t set_rc=-1,restore_rc=-1;
    std::uint32_t verified=0;
};
struct FeatureAck {
    std::uint64_t signature=feature_magic,token=0,at_ms=0;
    std::uint32_t phase=FeatureIdle;
    std::int32_t feature=-1,mode=-1,reserved=0;
};
inline bool valid(const FeaturePlan& p,std::uint64_t token) {
    return p.signature==feature_magic&&p.token==token&&p.protocol==1&&p.size==sizeof p&&
        address(p.setter)&&p.original>=0&&p.original<=2&&p.original_mode==int(p.original!=0);
}
inline bool feature_clean(const FeaturePlan& p,const FeatureProgress& s) {
    return s.signature==feature_magic&&s.token==p.token&&
        (s.phase==FeatureIdle||(s.phase==FeatureRestored&&!s.restore_rc&&s.verified));
}
struct FeatureOps {
    void* context;
    std::uint64_t (*now)(void*);
    void (*sleep)(void*,unsigned);
    bool (*cancel)(void*);
    int (*set)(void*,int);
    bool (*publish)(void*,const FeatureProgress&);
    bool (*ack)(void*,FeatureAck&);
};
class FeatureLease {
    FeaturePlan plan;
    FeatureOps ops;
    FeatureProgress status;
    bool touched=false;
    bool publish(){status.at_ms=ops.now(ops.context);return ops.publish(ops.context,status);}
    bool wait(bool restoring) {
        const auto start=ops.now(ops.context);
        while(ops.now(ops.context)-start<4000) {
            const auto now=ops.now(ops.context);FeatureAck a;
            if(ops.ack(ops.context,a)&&a.signature==feature_magic&&a.token==plan.token&&
               a.phase==status.phase&&a.at_ms>=start&&a.at_ms<=now&&now-a.at_ms<=1000&&
               a.feature==(restoring?plan.original:0)&&a.mode==(restoring?plan.original_mode:0))return true;
            if(!restoring&&ops.cancel(ops.context))return false;
            ops.sleep(ops.context,50);
        }
        return false;
    }
public:
    FeatureLease(const FeaturePlan& p,const FeatureOps& io):plan(p),ops(io){status.token=p.token;}
    bool prepare() {
        if(!valid(plan,plan.token)||!plan.token)return false;
        status.phase=FeatureEntering;
        if(!publish())return false;
        // Mark before calling: an error return does not prove no state changed.
        touched=true;status.set_rc=ops.set(ops.context,0);
        if(!publish()||status.set_rc||!wait(false))return false;
        status.phase=FeatureActive;return publish();
    }
    bool restore() {
        if(!touched){status.phase=FeatureIdle;return publish();}
        status.phase=FeatureRestoring;status.verified=0;
        (void)publish(); // Restoration is attempted even if reporting fails.
        status.restore_rc=ops.set(ops.context,plan.original);
        const bool reported=publish();
        const bool ok=reported&&!status.restore_rc&&wait(true);
        status.verified=ok;status.phase=ok?FeatureRestored:FeatureBroken;
        return publish()&&ok;
    }
};
static_assert(sizeof(FeaturePlan)==48&&sizeof(FeatureProgress)==40&&sizeof(FeatureAck)==40);
}
