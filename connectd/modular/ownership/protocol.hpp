// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>

namespace usb_free {
constexpr std::uint64_t magic=0x315453554d465355ULL;
constexpr std::uint32_t version=3;
constexpr unsigned stream_seconds=120, run_seconds=75;
constexpr unsigned sample_rate=48000,channels=2,bytes_per_sample=2;
constexpr std::uint64_t pcm_bytes=std::uint64_t(stream_seconds)*sample_rate*channels*bytes_per_sample;
constexpr std::uint64_t audio_bytes=44+pcm_bytes;
constexpr std::uint16_t port=38576;
constexpr const char* directory="/data/spotifyps5/pcm-transport-v9";
constexpr const char* worker_directory="/user/data/spotifyps5/pcm-transport-v9";
constexpr const char* legacy_directory="/data/custom-overlays/http-audio-test";

enum Phase : std::uint32_t { Idle=0, Checking, Refused, Adding, Starting, Playing,
    Stopping, Complete, Failed, CleanupFailed, Pausing, Paused, Resuming, Retired };
enum Reason : std::uint32_t { None=0, OtherSource, InvalidRequest, AddFailed, PlayFailed,
    MediaError, NoProgress, Deadline, UserStop, ParentLost, ApiSlow, SourceChanged, StateUnknown,
    PauseFailed, ResumeFailed, UnexpectedEnd, ClockJump, PriorNotGone, FeatureFailed };
enum Function : unsigned { Add, Play, Stop, Remove, GetTime, Current, IsPlaying, Pause, Bgm, ErrorQuery, FunctionCount };
struct Request {
    std::uint64_t signature=magic;
    std::uint32_t protocol=version, size=sizeof(Request);
    std::uint64_t build=0, token=0;
    std::int32_t shell_pid=0;
    std::uint32_t seconds=run_seconds;
    std::uint64_t shell_epoch=0; // kinfo_proc.ki_start; PID alone can repeat after reboot
    std::uint64_t functions[FunctionCount]{};
};
struct Resident {
    std::uint64_t signature=magic, build=0;
    std::uint32_t protocol=version;
    std::int32_t shell_pid=0;
    std::uint64_t shell_epoch=0;
};
struct Heartbeat { std::uint64_t token=0, at_ms=0; };
enum StateFunction : unsigned { QueryPlaying, QueryPaused, QueryStopped, StateFunctionCount };
struct StateQueries {
    std::uint64_t signature=magic, token=0;
    std::uint64_t functions[StateFunctionCount]{};
};
struct Status {
    std::uint64_t signature=magic, token=0, at_ms=0, began_ms=0;
    std::uint32_t protocol=version, phase=Idle, reason=None;
    std::int32_t source=-1, add_rc=0, play_rc=0, stop_rc=0, remove_rc=0, time_rc=0;
    std::int64_t position_ms=-1;
    std::uint32_t play_events=0, error_events=0, time_events=0, ended_events=0, source_playing=0;
    std::uint64_t max_call_ms=0;
    std::int32_t pause_rc=0, resume_rc=0, bgm_before=-1, bgm_now=-1, bgm_after=-1;
    std::uint32_t pause_confirmed=0,resume_confirmed=0,stop_confirmed=0,removed_from_table=0;
    std::int64_t paused_position_ms=-1,resumed_position_ms=-1;
    std::int32_t error_rc=-1,media_error=-1,error_detail=-1,remaining_service=0;
    std::uint64_t removal_wait_ms=0;
};
inline bool address(std::uint64_t p) { return p>=0x10000&&p<0x800000000000ULL; }
inline bool valid(const Request& r, std::int32_t pid, std::uint64_t build) {
    if(r.signature!=magic||r.protocol!=version||r.size!=sizeof r||!r.token||
       r.shell_pid!=pid||!r.shell_epoch||r.build!=build||r.seconds<5||r.seconds>run_seconds) return false;
    for(auto p:r.functions) if(!address(p)) return false;
    return true;
}
inline bool finished(std::uint32_t p) {
    return p==Refused||p==Complete||p==Failed||p==CleanupFailed||p==Retired;
}
inline bool valid(const StateQueries& q,std::uint64_t token) {
    if(q.signature!=magic||!token||q.token!=token)return false;
    for(auto p:q.functions)if(!address(p))return false;
    return true;
}
static_assert(sizeof(Request)==128);
static_assert(sizeof(Resident)==32);
static_assert(sizeof(Status)==192);
static_assert(sizeof(StateQueries)==40);
}
