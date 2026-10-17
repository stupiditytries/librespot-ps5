// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "protocol.hpp"
namespace usb_transport {
// Layouts verified against Media.cs Pack=4 declarations and native copy sizes.
struct Device {
    std::uint32_t id;
    char manufacturer[256],product[256],serial[256];
    std::int32_t root_folders,root_tracks;
};
struct DeviceList {std::int32_t count;Device entries[8];};
struct Track {
    std::int32_t index;
    std::uint32_t device;
    char path[512],title[256],album[256],artist[256],image[512];
    std::int64_t image_offset,image_size,duration;
};
struct TrackList {std::int32_t count,page;Track entries[100];};
struct Folder {std::uint32_t device;char path[512];std::int32_t type,result;};
struct FolderList {std::int32_t count,page;Folder entries[100];};
struct Buffers {DeviceList devices;TrackList tracks;FolderList folders;};
static_assert(sizeof(Device)==0x30c&&sizeof(DeviceList)==0x1864);
static_assert(sizeof(Track)==0x720&&sizeof(Folder)==0x20c);
static_assert(sizeof(TrackList)==182408&&sizeof(FolderList)==52408);
struct Api {
    int (*devices)(int*,DeviceList*);
    int (*files)(std::uint32_t,const char*,int,int*,TrackList*,int*,FolderList*);
    int (*start)(int,const char*,int,std::uint32_t,std::int32_t*,int,bool);
    int (*command)(int,std::int32_t,int);
    int (*current)(int);
    int (*time)(std::int32_t,double*);
    int (*playing)(std::int32_t,bool*);
    int (*paused)(std::int32_t,bool*);
    int (*stopped)(std::int32_t,bool*);
    int (*error)(std::int32_t,int*,int*);
    int (*info)(int,void*);
};
}
