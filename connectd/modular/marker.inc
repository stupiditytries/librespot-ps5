// Native HTTP source ownership only. PCM still uses the tested AudioOut path.
static bool cleanup_poisoned=false;
static int baseline[20]{};
static usb_free::PriorSources prior_sources;
static InactiveSources inactive_sources;
static bool (*prepare_feature)()=nullptr;
static bool (*restore_feature)()=nullptr;
static std::uint32_t callback_errors=0,callback_ends=0;
static std::uint64_t marker_seek_at=0;
static void marker_event(std::uint64_t kind,std::uint64_t,std::uint64_t,std::uint64_t) {
    if(kind==19)__atomic_fetch_add(&callback_errors,1,__ATOMIC_RELAXED);
    if(kind==13)__atomic_fetch_add(&callback_ends,1,__ATOMIC_RELAXED);
}
// Never unload the worker or its callback storage, including after RemoveSrc.
static usb_free::Callbacks marker_callbacks{marker_event,nullptr,4};
static bool selected(){return status.source>0;}
static bool native_absent(int id) {
    bool playing=false,paused=false,stopped=false;int error=-1,detail=-1;
    return usb_free::absent(api.playing(id,&playing),api.paused(id,&paused),
        api.stopped(id,&stopped),api.error(id,&error,&detail));
}
static bool old_source_absent(int id) {
    for(unsigned i=0;i<prior_sources.count;++i)
        if(prior_sources.entries[i].source==id)return native_absent(id);
    return false;
}
static bool legacy_inactive(int id) {
    bool playing=false,paused=false,stopped=false;int error=-1,detail=-1;
    const int a=api.playing(id,&playing),b=api.paused(id,&paused),c=api.stopped(id,&stopped),d=api.error(id,&error,&detail);
    const bool absent=usb_free::absent(a,b,c,d);
    const bool inactive=!a&&!b&&!c&&!d&&!error&&!playing&&!paused&&stopped;
    note("LEGACY LIVE source=%d absent=%u stopped=%u playing=%u paused=%u rc=%08x/%08x/%08x/%08x error=%d",id,absent,stopped,playing,paused,a,b,c,d,error);
    return absent||inactive;
}
static bool unobstructed(int ours=-1,bool capture=false) {
    for(int service=1;service<=20;++service) {
        const int id=api.current(service);
        if(id<=0){if(capture)baseline[service-1]=0;continue;}
        if(id==ours||old_source_absent(id))continue;
        bool playing=false,paused=false,stopped=false;
        if(api.playing(id,&playing)||api.paused(id,&paused)||api.stopped(id,&stopped)||
           playing||(!paused&&!stopped)||(!capture&&baseline[service-1]!=id)) {
            note("YIELD foreign source service=%d source=%d playing=%u paused=%u stopped=%u",service,id,playing,paused,stopped);
            return false;
        }
        if(capture)baseline[service-1]=id;
    }
    return true;
}
static bool still_requested() {
    CommandRequest c;Gate gate;std::uint64_t stop=0;
    if(!wanted||!fresh()||usb_free::retire_requested(worker_directory,session.token)||
       (read_file(worker_directory,"stop.bin",stop)&&stop==session.token))return false;
    return read_file(worker_directory,"gate.bin",gate)&&gate.token==session.token&&gate.intent&&
        read_file(worker_directory,"command.bin",c)&&c.token==session.token&&c.sequence==status.sequence&&c.action==1;
}
static bool stop_owned() {
    const bool closed=close_audio();bool source_clean=true;
    if(status.source>0) {
        const int id=status.source;
        const int stop_rc=api.stop(id);bool confirmed=false;
        const auto began=now_ms();
        do {
            bool playing=false,paused=false,stopped=false;
            if(!api.playing(id,&playing)&&!api.paused(id,&paused)&&!api.stopped(id,&stopped)&&
               !playing&&!paused&&stopped){confirmed=true;break;}
            usleep(50000);
        }while(now_ms()-began<2000);
        const int remove_rc=api.remove(id);bool gone=false;
        const auto removed=now_ms();
        do {
            bool table=false;
            for(int service=1;service<=20;++service)if(api.current(service)==id)table=true;
            if(!table&&native_absent(id)){gone=true;break;}
            usleep(50000);
        }while(now_ms()-removed<2000);
        source_clean=!stop_rc&&confirmed&&!remove_rc&&gone;
        note("MARKER cleanup source=%d stop=%08x stopped=%u remove=%08x native_absence=%u wait_ms=%llu",
            id,stop_rc,confirmed,remove_rc,gone,(unsigned long long)(now_ms()-removed));
        if(source_clean)status.source=-1;
        else {status.rc=stop_rc?stop_rc:remove_rc;status.detail=21;}
    }
    // Always attempt restoration, even after a source/AudioOut cleanup failure.
    const bool restored=restore_feature&&restore_feature();
    const bool clean=closed&&source_clean&&restored;
    if(!clean){cleanup_poisoned=true;if(!restored)status.detail=22;}
    if(clean){status.rc=0;status.detail=0;}
    status.state=clean?StoppedOk:Failed;
    marker_seek_at=0;
    return clean;
}
static void pause_owned() {
    marker_seek_at=0;
    if(!close_audio()){status.state=Failed;return;}
    if(!selected()){status.state=StoppedOk;return;}
    bool paused=false;
    if(!api.paused(status.source,&paused)&&paused){status.state=ServicePaused;return;}
    status.rc=api.pause(status.source);
    note("PAUSE HTTP source=%d rc=%08x",status.source,status.rc);
    status.state=status.rc?Failed:Pausing;started_at=now_ms();
}
static void ensure() {
    status.rc=0;status.detail=0;
    if(cleanup_poisoned){status.state=Failed;status.detail=22;return;}
    if(selected()) {
        bool playing=false,paused=false;
        if(!api.playing(status.source,&playing)&&playing){status.state=Running;return;}
        if(!api.paused(status.source,&paused)&&paused) {
            if(!unobstructed(status.source)||!still_requested()) {
                if(stop_owned())status.state=Refused;
                return;
            }
            status.rc=api.play(status.source);status.state=status.rc?Failed:Starting;started_at=now_ms();
            note("RESUME HTTP source=%d rc=%08x",status.source,status.rc);return;
        }
        if(!stop_owned())return;
    }
    if(!still_requested()||!unobstructed(-1,true)){status.state=Refused;return;}
    for(unsigned pass=0;pass<2;++pass) {
        for(unsigned i=0;i<inactive_sources.count;++i)if(!legacy_inactive(inactive_sources.ids[i])) {
            note("REFUSED old USB source is not natively stopped/absent id=%d",inactive_sources.ids[i]);status.state=Refused;return;
        }
        for(unsigned i=0;i<prior_sources.count;++i)if(!native_absent(prior_sources.entries[i].source)) {
            note("REFUSED previous source still registered id=%d",prior_sources.entries[i].source);status.state=Refused;return;
        }
        if(pass==0&&(prior_sources.count||inactive_sources.count))usleep(100000);
    }
    if(!prepare_feature||!prepare_feature()) {
        const bool restored=restore_feature&&restore_feature();
        cleanup_poisoned=!restored;status.state=Failed;status.detail=20;return;
    }
    if(!still_requested()||!unobstructed()) {
        (void)stop_owned();if(!cleanup_poisoned)status.state=Refused;return;
    }
    __atomic_store_n(&callback_errors,0,__ATOMIC_RELAXED);
    __atomic_store_n(&callback_ends,0,__ATOMIC_RELAXED);
    usb_free::Source source(session.token);int id=-1;
    status.rc=api.add(source.bytes,&marker_callbacks,&id);
    note("ADD HTTP source=%d rc=%08x duration_seconds=%u",id,status.rc,usb_free::stream_seconds);
    if(status.rc||id<=0) {
        const int rc=status.rc;(void)stop_owned();status.state=Failed;status.rc=rc;status.detail=4;return;
    }
    status.source=id;
    if(!still_requested()||!unobstructed(id)) {(void)stop_owned();if(!cleanup_poisoned)status.state=Refused;return;}
    status.rc=api.play(id);const int rc=status.rc;
    note("PLAY HTTP source=%d rc=%08x",id,rc);
    if(rc){(void)stop_owned();status.state=Failed;status.rc=rc;status.detail=4;return;}
    status.state=Starting;started_at=now_ms();
}
