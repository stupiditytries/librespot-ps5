// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "ownership/mode-context.hpp"
#include "ownership/files.hpp"
class ModeCoordinator {
    usb_free::ModeContext context;
    usb_free::FeaturePlan plan;
    std::uint32_t phase=usb_free::FeatureIdle,matched=usb_free::FeatureIdle;
    std::uint64_t since=0;
    bool healthy=true,ack_logged=false;
public:
    explicit ModeCoordinator(void(*log)(const char*,...)):context(log){}
    bool start(int pid,std::uint64_t token) {
        plan.token=token;
        if(!context.open(pid,plan))return false;
        std::uint32_t handle=0;
        if(kernel_dynlib_handle(pid,"libSceMediaFrameworkInterface.sprx",&handle)<0||!handle)return false;
        auto setter=kernel_dynlib_dlsym(pid,handle,"sceMediaFwSetAudioTagFeature");
        if(!setter){char hash[12]{};if(nid_encode("sceMediaFwSetAudioTagFeature",hash))setter=kernel_dynlib_resolve(pid,handle,hash);}
        plan.setter=setter;
        if(!usb_free::valid(plan,token))return false;
        usb_free::FeatureProgress initial;initial.token=token;
        note("AUDIOTAG original_enum=%d core_mode=%d app_id=%08x",plan.original,plan.original_mode,plan.app_id);
        return usb_free::write_file(pcm_transport::directory,"feature-plan.bin",plan)&&
            usb_free::write_file(pcm_transport::directory,"feature-status.bin",initial);
    }
    bool needs_bootstrap() const {return context.needs_bootstrap();}
    bool clean() const {
        usb_free::FeatureProgress progress;
        return usb_free::read_file(pcm_transport::directory,"feature-status.bin",progress)&&usb_free::feature_clean(plan,progress);
    }
    bool tick(std::uint64_t now) {
        usb_free::FeatureProgress p;
        if(!usb_free::read_file(pcm_transport::directory,"feature-status.bin",p)||p.signature!=usb_free::feature_magic||p.token!=plan.token)return false;
        if(p.phase!=phase) {
            phase=p.phase;matched=usb_free::FeatureIdle;since=0;ack_logged=false;
            note("AUDIOTAG phase=%u set=%08x restore=%08x verified=%u",phase,p.set_rc,p.restore_rc,p.verified);
        }
        const bool enter=phase==usb_free::FeatureEntering&&!p.set_rc;
        const bool restore=phase==usb_free::FeatureRestoring&&!p.restore_rc;
        if(enter||restore) {
            int feature=-1,mode=-1;
            const int wanted=restore?plan.original:0,wanted_mode=restore?plan.original_mode:0;
            if(context.sample(feature,mode)&&feature==wanted&&mode==wanted_mode) {
                if(matched!=phase){matched=phase;since=now;}
                else if(now-since>=100) {
                    usb_free::FeatureAck a;a.token=plan.token;a.at_ms=now;a.phase=phase;a.feature=feature;a.mode=mode;
                    if(!usb_free::write_file(pcm_transport::directory,"feature-ack.bin",a))healthy=false;
                    else if(!ack_logged){note("AUDIOTAG verified phase=%u enum=%d mode=%d",phase,feature,mode);ack_logged=true;}
                }
            } else {matched=usb_free::FeatureIdle;since=0;}
        } else if(phase==usb_free::FeatureActive) {
            int feature=-1,mode=-1;
            if(!context.sample(feature,mode)||feature||mode) {
                usb_free::FeatureProgress again;
                // Restoration publishes its phase before changing the mode.
                // Re-read to avoid treating our concurrent restore as takeover.
                if(usb_free::read_file(pcm_transport::directory,"feature-status.bin",again)&&
                   again.signature==usb_free::feature_magic&&again.token==plan.token&&again.phase!=usb_free::FeatureActive)return healthy;
                if(healthy)note("AUDIOTAG policy changed/unreadable; stopping own PCM source");
                healthy=false;
            }
        } else if(phase==usb_free::FeatureBroken)healthy=false;
        return healthy;
    }
};
