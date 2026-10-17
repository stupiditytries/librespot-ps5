// SPDX-License-Identifier: GPL-3.0-or-later
#define USB_TRANSPORT_TEST 1
#include "worker.cpp"
#include <sys/stat.h>
#include <cassert>
#include <cstdio>
static int currents[21], starts, stops, resumed, devices=1, start_result;
static bool audible, held, ended, wrong_path, matches_uri=true;
static int get_current(int service){return currents[service];}
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
    else {assert(cmd==2);++resumed;audible=true;held=false;ended=false;}
    return 0;
}
static void reset(){
    std::memset(currents,0,sizeof currents);starts=stops=resumed=0;devices=1;start_result=0;
    audible=held=ended=wrong_path=false;matches_uri=true;status=Status{};status.token=123;status.sequence=1;
    session=Request{};session.token=123;
    api={get_devices,get_files,start,command,get_current,get_time,get_playing,get_paused,get_stopped,get_error,get_info};
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
    assert(streamer("usb:/Music/Start Librespot/Librespot Streamer.flac"));
    assert(!streamer("/Music/Other/Librespot Streamer.flac"));
    std::puts("11 native USB ownership/start/stop/cancellation cases passed");
}
