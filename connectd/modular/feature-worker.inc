static usb_free::FeaturePlan feature_plan;
static std::optional<usb_free::FeatureLease> feature_lease;
static bool feature_active=false;
static bool feature_cancel(void*) {
    return !still_requested()||usb_free::retire_requested(worker_directory,session.token);
}
static bool feature_publish(void*,const usb_free::FeatureProgress& p) {
    note("AUDIOTAG phase=%u set=%08x restore=%08x verified=%u",p.phase,p.set_rc,p.restore_rc,p.verified);
    return pcm_transport::write_file(worker_directory,"feature-status.bin",p);
}
static bool begin_feature() {
    if(feature_active)return true;
    if(!pcm_transport::read_file(worker_directory,"feature-plan.bin",feature_plan)||!usb_free::valid(feature_plan,session.token))return false;
    usb_free::FeatureOps ops{nullptr,[](void*){return now_ms();},[](void*,unsigned ms){usleep(ms*1000);},feature_cancel,
        [](void*,int value){return reinterpret_cast<int(*)(int)>(feature_plan.setter)(value);},feature_publish,
        [](void*,usb_free::FeatureAck& a){return pcm_transport::read_file(worker_directory,"feature-ack.bin",a);}};
    feature_lease.emplace(feature_plan,ops);feature_active=feature_lease->prepare();return feature_active;
}
static bool end_feature() {
    if(!feature_lease)return true;
    const bool ok=feature_lease->restore();
    if(ok){feature_active=false;feature_lease.reset();}
    return ok;
}
