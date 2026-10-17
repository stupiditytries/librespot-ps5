// SPDX-License-Identifier: GPL-3.0-or-later
#define PCM_TRANSPORT_TEST 1
#include "worker.cpp"
#include <sys/stat.h>
#include <cassert>
#include <cstdio>
static int currents[21], starts, stops, resumed, devices=1, start_result,closes,close_result,pauses,pause_result;
static int close_port(int port){assert(port==9);++closes;return close_result;}
static bool audible, held, ended, wrong_path, matches_uri=true;
static int get_current(int service){return currents[service];}
static int opens;
static int open_port(int,int,int,unsigned,unsigned,unsigned){++opens;return 9;}
static int set_volume(int,int,const int*){return 0;}
static int get_playing(int,bool* p){*p=audible;return 0;}
static int get_paused(int,bool* p){*p=held;return 0;}
static int get_stopped(int,bool* p){*p=ended;return 0;}
static int get_error(int,int* p,int* d){*p=0;*d=0;return 0;}
static int get_time(int,double* p){*p=1;return 0;}
static int get_info(int service,void* out){
    auto* b=static_cast<unsigned char*>(out);int id=currents[service];
    std::memcpy(b+4,&id,4);std::memcpy(b+8,&service,4);
    std::snprintf(reinterpret_cast<char*>(b+0xc4),4096,"usb:%s",matches_uri?clip_path:"/Music/Other/song.flac");return 0;
}
static int get_devices(int* count,DeviceList* out){
    *count=devices;out->count=devices;for(int i=0;i<devices;++i)out->entries[i].id=77+i;return 0;
}
static int get_files(std::uint32_t dev,const char* folder,int page,int* n,TrackList* tracks,int* nf,FolderList* folders){
    assert(!std::strcmp(folder,clip_folder)&&page==0);*n=1;*nf=0;tracks->count=1;folders->count=0;
    auto& t=tracks->entries[0];t.index=0;t.device=dev;
    std::snprintf(t.path,sizeof t.path,"%s",wrong_path?"/Music/Start Librespot/not-streamer.flac":clip_path);return 0;
}
static int start(int service,const char* uri,int index,std::uint32_t dev,int* source,int playback,bool advance){
    assert(service==2&&streamer(uri)&&index==0&&dev==77&&playback==1&&!advance);
    ++starts;*source=42;currents[2]=42;audible=true;held=false;ended=false;return start_result;
}
static int command(int cmd,int source,int playback){
    assert(source>0&&source==currents[2]&&playback==1);
    if(cmd==6){++stops;audible=false;held=false;ended=true;}
    else if(cmd==3){++pauses;if(pause_result)return pause_result;audible=false;held=true;ended=false;}
    else {assert(cmd==2);++resumed;audible=true;held=false;ended=false;}
    return 0;
}
static void reset(){
    std::memset(currents,0,sizeof currents);starts=stops=resumed=0;devices=1;start_result=0;
    audible=held=ended=wrong_path=false;matches_uri=true;closes=pauses=0;close_result=pause_result=0;status=Status{};status.token=123;status.sequence=1;
    session=Request{};session.token=123;
    api={get_devices,get_files,start,command,get_current,get_time,get_playing,get_paused,get_stopped,get_error,get_info,open_port,nullptr,close_port,set_volume,nullptr,nullptr,nullptr};
    opens=0;wanted=true;
    assert(write_file(worker_directory,"heartbeat.bin",Heartbeat{123,now_ms()}));
    assert(write_file(worker_directory,"command.bin",CommandRequest{123,1,1,0}));
}
int main(){
    mkdir(directory,0755);
    reset();ensure();assert(starts==1&&status.state==Starting&&status.source==42);
    assert(stop_owned()&&stops==1&&status.state==StoppedOk);
    reset();currents[1]=80;audible=true;ensure();assert(status.state==Refused&&starts==0);
    reset();currents[2]=81;held=true;matches_uri=false;ensure();assert(starts==1);
    reset();wrong_path=true;ensure();assert(status.state==Refused&&starts==0);
    reset();devices=2;ensure();assert(status.state==Refused&&starts==0);
    reset();write_file(worker_directory,"heartbeat.bin",Heartbeat{999,now_ms()});ensure();assert(starts==0);
    reset();write_file(worker_directory,"command.bin",CommandRequest{123,2,2,0});ensure();assert(starts==0);
    reset();status.source=42;currents[2]=43;assert(stop_owned()&&stops==0);
    reset();currents[2]=42;audible=true;ensure();assert(starts==0&&status.source==42&&status.state==Running);
    reset();currents[2]=42;held=true;ensure();assert(starts==0&&resumed==1&&status.state==Starting);
    reset();start_result=-7;ensure();assert(starts==1&&stops==1&&status.state==Failed&&status.rc==-7);
    assert(streamer("usb:/Music/librespot-ps5 Service/librespot-ps5 Service.flac"));
    assert(!streamer("/Music/Other/Librespot Streamer.flac"));
    reset();status.source=42;currents[2]=42;status.port=9;
    assert(stop_owned()&&closes==1&&stops==1&&status.port==-1);
    reset();status.source=42;currents[2]=43;status.port=9;
    assert(stop_owned()&&closes==1&&stops==0&&status.port==-1);
    reset();status.source=42;currents[2]=42;status.port=9;close_result=-7;
    assert(!stop_owned()&&closes==1&&stops==1&&status.port==9&&status.state==Failed);
    reset();status.source=42;currents[2]=42;currents[1]=80;audible=true;status.port=9;
    assert(!unobstructed(status.source));assert(stop_owned()&&closes==1&&stops==1&&currents[1]==80);
    reset();status.source=42;currents[2]=42;audible=true;status.port=9;pause_owned();
    assert(pauses==1&&closes==1&&status.state==Pausing&&held&&status.source==42&&status.port==-1);
    ensure();assert(resumed==1&&starts==0&&status.source==42);
    reset();status.source=42;currents[2]=43;status.port=9;pause_owned();
    assert(pauses==0&&closes==1&&status.state==StoppedOk);
    reset();status.source=42;currents[2]=42;held=true;pause_owned();
    assert(pauses==0&&status.state==ServicePaused&&status.source==42);
    reset();status.source=42;currents[2]=42;audible=true;pause_result=-8;pause_owned();
    assert(pauses==1&&status.state==Failed&&status.rc==-8);
    // A just-started source is not lost while it is still unselected or Stopped.
    reset();ensure();currents[2]=0;observe(now_ms());assert(status.state==Starting&&status.source==42);
    currents[2]=42;audible=false;ended=true;observe(now_ms());assert(status.state==Starting&&status.source==42);
    ended=false;audible=true;observe(now_ms());assert(status.state==Running&&opens==1&&status.port==9);
    // After the start deadline the same answers are a loss.
    reset();ensure();started_at=now_ms()-20000;currents[2]=0;observe(now_ms());assert(status.state==Lost);
    // Console resume while Spotify is paused: reported, not paused back, no audio yet.
    reset();status.source=42;currents[2]=42;audible=true;status.state=ServicePaused;wanted=false;
    observe(now_ms());assert(status.state==ConsoleResumed&&pauses==0&&opens==0);
    wanted=true;observe(now_ms());assert(status.state==Running&&opens==1);
    // Paused again at the console before Spotify reacted.
    reset();status.source=42;currents[2]=42;held=true;status.state=ConsoleResumed;wanted=false;
    observe(now_ms());assert(status.state==ServicePaused);
    // A resume that has not taken effect yet keeps waiting instead of re-pausing.
    reset();status.source=42;currents[2]=42;held=true;status.state=Starting;started_at=now_ms();
    observe(now_ms());assert(status.state==Starting);
    // Paused at the console while Spotify played, versus our own Pause.
    reset();status.source=42;currents[2]=42;held=true;status.state=Running;status.port=9;
    observe(now_ms());assert(status.state==ConsolePaused&&status.port==-1);
    observe(now_ms());assert(status.state==ConsolePaused);
    reset();status.source=42;currents[2]=42;held=true;status.state=Pausing;
    observe(now_ms());assert(status.state==ServicePaused);
    std::puts("27 native PCM marker ownership/start/stop/cancellation/observe cases passed");
}
