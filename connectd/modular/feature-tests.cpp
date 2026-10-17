// SPDX-License-Identifier: GPL-3.0-or-later
#include "ownership/feature.hpp"
#include <cassert>
#include <cstdio>
#include <vector>
using namespace usb_free;
struct FakeFeature {
    std::uint64_t now=1000;FeaturePlan plan;FeatureProgress progress;
    std::vector<int> calls;
    bool bad_token=false,stale=false,no_ack=false,cancel=false,fail_set=false,fail_restore=false;
    bool fail_publish_once=false,wrong_feature=false;
    FakeFeature(){plan.token=42;plan.setter=0x12345;plan.original=2;plan.original_mode=1;}
    FeatureOps ops() {
        return {this,[](void* p){return static_cast<FakeFeature*>(p)->now;},
            [](void* p,unsigned n){static_cast<FakeFeature*>(p)->now+=n;},
            [](void* p){return static_cast<FakeFeature*>(p)->cancel;},
            [](void* p,int value){auto& f=*static_cast<FakeFeature*>(p);f.calls.push_back(value);
                return f.calls.size()==1&&f.fail_set?-5:f.calls.size()>1&&f.fail_restore?-6:0;},
            [](void* p,const FeatureProgress& s){auto& f=*static_cast<FakeFeature*>(p);f.progress=s;
                if(f.fail_publish_once){f.fail_publish_once=false;return false;}return true;},
            [](void* p,FeatureAck& a){auto& f=*static_cast<FakeFeature*>(p);if(f.no_ack)return false;
                a.token=f.bad_token?f.plan.token+1:f.plan.token;a.at_ms=f.stale?999:f.now;
                a.phase=f.progress.phase;const bool restore=a.phase==FeatureRestoring;
                a.feature=f.wrong_feature?1:restore?f.plan.original:0;a.mode=restore?f.plan.original_mode:0;return true;}};
    }
};
int main() {
    for(int original=0;original<=2;++original) {
        FakeFeature f;f.plan.original=original;f.plan.original_mode=original!=0;
        FeatureLease lease(f.plan,f.ops());assert(lease.prepare()&&lease.restore());
        assert(f.calls==std::vector<int>({0,original})&&feature_clean(f.plan,f.progress));
    }
    FakeFeature failure;failure.fail_set=true;FeatureLease failed(failure.plan,failure.ops());
    assert(!failed.prepare()&&failed.restore()&&failure.calls==std::vector<int>({0,2}));
    FakeFeature wrong;wrong.bad_token=true;FeatureLease wrong_lease(wrong.plan,wrong.ops());
    assert(!wrong_lease.prepare());wrong.bad_token=false;assert(wrong_lease.restore());
    FakeFeature stale;stale.stale=true;FeatureLease stale_lease(stale.plan,stale.ops());
    assert(!stale_lease.prepare());stale.stale=false;assert(stale_lease.restore());
    FakeFeature wrong_policy;wrong_policy.wrong_feature=true;FeatureLease wrong_policy_lease(wrong_policy.plan,wrong_policy.ops());
    assert(!wrong_policy_lease.prepare());wrong_policy.wrong_feature=false;assert(wrong_policy_lease.restore());
    FakeFeature lost;lost.cancel=true;lost.no_ack=true;FeatureLease parent_lost(lost.plan,lost.ops());
    assert(!parent_lost.prepare()&&!parent_lost.restore()&&lost.calls==std::vector<int>({0,2}));
    assert(!feature_clean(lost.plan,lost.progress)&&lost.progress.restore_rc==0); // Requested, not verified.
    FakeFeature restore_fail;FeatureLease broken(restore_fail.plan,restore_fail.ops());assert(broken.prepare());
    restore_fail.fail_restore=true;assert(!broken.restore()&&!feature_clean(restore_fail.plan,restore_fail.progress));
    FakeFeature io_fail;io_fail.fail_publish_once=true;FeatureLease cannot_report(io_fail.plan,io_fail.ops());
    assert(!cannot_report.prepare()&&cannot_report.restore()&&io_fail.calls.empty());
    std::puts("PASS AudioTag lease: exact 0/1/2 restoration, partial setup failure, stale/wrong acknowledgements, parent loss, restore error and reporting failure");
}
