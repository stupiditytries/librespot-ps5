// SPDX-License-Identifier: GPL-3.0-or-later
#define PCM_TRANSPORT_TEST 1
#include "worker.cpp"
#include <sys/stat.h>
#include <cassert>
#include <cstdio>
static int current[21],adds,plays,pauses,stops,removes,opens,closes,prepares,restores;
static int add_rc,play_rc,pause_rc,close_rc,remove_rc,query_rc,seeks,seek_rc;
static double native_position=1;
static bool audible,held,ended,removed,prepare_ok,restore_ok,cancel_after_prepare,foreign_after_prepare;
static int foreign_playing=0;
static constexpr int missing=static_cast<int>(0x81790003U);
static int get_current(int service){return current[service];}
static int playing(int id,bool* out){if(id!=42){*out=id==foreign_playing;return 0;}*out=audible;return removed?missing:query_rc;}
static int paused(int id,bool* out){*out=id==16?false:id!=42||held;return id==42&&removed?missing:query_rc;}
static int stopped(int id,bool* out){*out=id!=42||ended;return id==42&&removed?missing:query_rc;}
static int error(int id,int* out,int* detail){*out=*detail=0;return id==42&&removed?missing:query_rc;}
static int time_query(int,double* out){*out=native_position;return 0;}
static int seek(int id,double position){assert(id==42&&position==0);++seeks;if(!seek_rc)native_position=position;return seek_rc;}
static int add(const void* data,const void* cb,int* id){
    assert(prepares==1&&restores==0&&!std::strncmp(static_cast<const char*>(data),"http://127.0.0.1:",17));
    assert(static_cast<const usb_free::Callbacks*>(cb)->function==marker_event);
    ++adds;*id=42;removed=false;return add_rc;
}
static int play(int id){assert(id==42);++plays;if(play_rc)return play_rc;audible=true;held=ended=false;return 0;}
static int pause(int id){assert(id==42);++pauses;if(pause_rc)return pause_rc;audible=false;held=true;return 0;}
static int stop(int id){assert(id==42);++stops;audible=held=false;ended=true;return 0;}
static int remove(int id){assert(id==42);++removes;if(remove_rc)return remove_rc;removed=true;return 0;}
static int open_port(int,int,int,unsigned,unsigned,unsigned){++opens;return 9;}
static int close_port(int id){assert(id==9);++closes;return close_rc;}
static int volume(int,int,const int*){return 0;}
static bool prepare(){++prepares;if(cancel_after_prepare)wanted=false;if(foreign_after_prepare){current[1]=80;foreign_playing=80;}return prepare_ok;}
static bool restore(){++restores;return restore_ok;}
static void reset(){
    std::memset(current,0,sizeof current);std::memset(baseline,0,sizeof baseline);
    adds=plays=pauses=stops=removes=opens=closes=prepares=restores=0;
    add_rc=play_rc=pause_rc=close_rc=remove_rc=query_rc=0;
    seeks=seek_rc=0;native_position=1;marker_seek_at=0;
    audible=held=ended=removed=cancel_after_prepare=foreign_after_prepare=false;prepare_ok=restore_ok=true;
    foreign_playing=0;cleanup_poisoned=false;callback_errors=callback_ends=0;prior_sources=usb_free::PriorSources{};
    inactive_sources=InactiveSources{};inactive_sources.token=123;
    status=Status{};status.token=123;status.sequence=1;session=Request{};session.token=123;wanted=true;
    api={add,play,stop,remove,get_current,time_query,playing,paused,stopped,error,pause,open_port,nullptr,close_port,volume,nullptr,nullptr,nullptr,seek};
    prepare_feature=prepare;restore_feature=restore;
    assert(pcm_transport::write_file(worker_directory,"heartbeat.bin",Heartbeat{123,now_ms()}));
    assert(pcm_transport::write_file(worker_directory,"command.bin",CommandRequest{123,1,1,0}));
    assert(pcm_transport::write_file(worker_directory,"gate.bin",Gate{123,1,1,1}));
    assert(pcm_transport::write_file(worker_directory,"stop.bin",std::uint64_t(0)));
}
static void start_running(){ensure();assert(status.state==Starting&&status.source==42);observe(now_ms());assert(status.state==Running&&status.port==9);}
int main(){
    mkdir(directory,0755);
    assert(compatible_resident(version,bootstrap_status_race_build,1234));
    assert(!compatible_resident(version,5678,1234));
    assert(!compatible_resident(version+1,bootstrap_status_race_build,1234));
    assert(publish_bootstrap_idle(0,false,true));
    assert(!publish_bootstrap_idle(123,false,true));
    assert(!publish_bootstrap_idle(0,true,true));
    Status clean;clean.token=123;clean.state=StoppedOk;
    assert(previous_clean(clean,123));assert(!previous_clean(clean,456));
    clean.port=9;assert(!previous_clean(clean,123));clean.port=-1;
    clean.source=42;assert(!previous_clean(clean,123));clean.source=-1;
    clean.state=Failed;assert(!previous_clean(clean,123));
    reset();status.sequence=0;status.state=Failed;status.rc=48;status.detail=10;
    assert(startup_failure(status)&&status_for_command(status,1,false));
    assert(stop_owned()&&status.state==StoppedOk&&status.source==-1&&status.port==-1&&
        !status.rc&&!status.detail&&!adds&&!plays&&!opens&&restores==1);
    reset();status.state=Failed;status.detail=10;
    assert(!startup_failure(status)&&!status_for_command(status,2,false));
    reset();status.sequence=0;status.state=Lost;status.detail=10;assert(!startup_failure(status));
    reset();start_running();assert(adds==1&&plays==1&&opens==1&&restores==0);
    pause_owned();assert(status.state==Pausing&&status.port==-1&&held&&restores==0&&status.source==42);
    observe(now_ms());assert(status.state==ServicePaused);ensure();assert(adds==1&&plays==2&&restores==0);
    observe(now_ms());assert(status.state==Running&&opens==2);
    assert(stop_owned()&&status.source==-1&&status.port==-1&&stops==1&&removes==1&&restores==1);
    reset();current[1]=80;foreign_playing=80;ensure();assert(status.state==Refused&&!adds&&!prepares&&!stops);
    reset();current[2]=31;ensure();assert(status.state==Starting&&adds==1&&current[2]==31);
    reset();inactive_sources.count=1;inactive_sources.ids[0]=16;ensure();assert(status.state==Starting&&adds==1&&prepares==1&&stops==0&&removes==0);
    reset();inactive_sources.count=1;inactive_sources.ids[0]=16;foreign_playing=16;ensure();assert(status.state==Refused&&!adds&&!prepares&&!stops&&!removes);
    reset();inactive_sources.count=1;inactive_sources.ids[0]=16;query_rc=-7;ensure();assert(status.state==Refused&&!adds&&!prepares);
    reset();cancel_after_prepare=true;ensure();assert(status.state==Refused&&!adds&&restores==1);
    reset();foreign_after_prepare=true;ensure();assert(status.state==Refused&&!adds&&restores==1&&current[1]==80);
    reset();prepare_ok=false;ensure();assert(status.state==Failed&&!adds&&restores==1&&!cleanup_poisoned);
    reset();prepare_ok=false;restore_ok=false;ensure();assert(status.state==Failed&&cleanup_poisoned&&!adds);
    reset();add_rc=-5;ensure();assert(status.state==Failed&&adds==1&&!plays&&!stops&&!removes&&restores==1);
    reset();play_rc=-6;ensure();assert(status.state==Failed&&stops==1&&removes==1&&restores==1&&status.source==-1);
    reset();start_running();close_rc=-7;assert(!stop_owned()&&stops==1&&removes==1&&restores==1&&status.port==9&&cleanup_poisoned);
    reset();start_running();restore_ok=false;assert(!stop_owned()&&cleanup_poisoned&&status.source==-1);const auto count=adds;ensure();assert(adds==count&&status.state==Failed);
    reset();start_running();remove_rc=-7;assert(!stop_owned()&&restores==1&&status.source==42&&cleanup_poisoned);
    reset();start_running();current[1]=80;foreign_playing=80;observe(now_ms());assert(status.state==Lost&&status.source==-1&&stops==1&&removes==1&&restores==1&&current[1]==80);
    reset();start_running();pause_owned();observe(now_ms());current[1]=80;foreign_playing=80;ensure();assert(status.state==Refused&&status.source==-1&&restores==1);
    reset();start_running();audible=false;held=true;observe(now_ms());assert(status.state==ConsolePaused&&status.port==-1&&restores==0);
    wanted=false;held=false;audible=true;observe(now_ms());assert(status.state==ConsoleResumed&&opens==1);
    wanted=true;observe(now_ms());assert(status.state==Running&&opens==2);
    reset();start_running();audible=false;ended=true;observe(now_ms());assert(status.state==Lost&&status.source==-1&&removes==1&&restores==1);
    reset();start_running();marker_event(19,0,0,0);observe(now_ms());assert(status.state==Failed&&status.source==-1&&restores==1);
    reset();start_running();marker_event(13,0,0,0);observe(now_ms());assert(status.state==Failed&&status.source==-1&&restores==1);
    reset();start_running();native_position=91;observe(now_ms());assert(seeks==1&&marker_seek_at&&status.state==Running&&status.port==9&&opens==1&&closes==0);
    observe(now_ms());assert(!marker_seek_at&&status.state==Running&&status.port==9&&opens==1&&closes==0&&restores==0);
    reset();start_running();native_position=91;seek_rc=-8;observe(now_ms());assert(status.state==Failed&&status.source==-1&&restores==1);
    reset();start_running();native_position=91;observe(now_ms());pause_owned();observe(now_ms());assert(!marker_seek_at&&status.state==ServicePaused&&held&&restores==0);
    reset();start_running();marker_seek_at=now_ms()-6000;native_position=91;observe(now_ms());assert(status.state==Failed&&status.source==-1&&restores==1);
    reset();pcm_transport::write_file(worker_directory,"heartbeat.bin",Heartbeat{999,now_ms()});ensure();assert(status.state==Refused&&!adds&&!prepares);
    reset();pcm_transport::write_file(worker_directory,"stop.bin",std::uint64_t(123));ensure();assert(status.state==Refused&&!adds&&!prepares);
    reset();pcm_transport::write_file(worker_directory,"gate.bin",Gate{123,1,0,0});ensure();assert(status.state==Refused&&!adds&&!prepares);
    std::puts("PASS USB-free PCM lifecycle: start/audio gate, pause/resume, console transitions, foreign takeover, cancellation, silent-only rewind with retained PCM port, rewind failures/pause race, Add/Play/callback failures, exact teardown and poisoned cleanup refusal");
}
