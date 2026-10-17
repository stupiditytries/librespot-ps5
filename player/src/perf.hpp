// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
namespace player::perf {
using Clock=std::chrono::steady_clock;
enum Metric { Snapshot, Ui, ArtUpload, ArtDecode, Blur, CanvasDecode, CanvasUpload, Present, TextFit, Count };
struct Counter {std::atomic<std::uint64_t> ns{0},calls{0},peak{0};};
inline std::array<Counter,Count> counters;
inline std::atomic<std::int64_t> input_at{0};
inline std::atomic<unsigned> inputs{0},canvas_resets{0},canvas_loads{0};
inline std::atomic<std::size_t> art_bytes{0},art_pending{0},playlists{0},albums{0},songs{0};
inline std::atomic<int> page{0},selection{0};
inline const char* output=nullptr;
inline std::int64_t nanos(){return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();}
inline void sample(Metric m,std::uint64_t ns){auto& c=counters[m];c.ns.fetch_add(ns,std::memory_order_relaxed);c.calls.fetch_add(1,std::memory_order_relaxed);auto old=c.peak.load();while(old<ns&&!c.peak.compare_exchange_weak(old,ns)){};}
struct Scope {Metric metric;Clock::time_point at=Clock::now();explicit Scope(Metric m):metric(m){}~Scope(){sample(metric,std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-at).count());}};
inline void input(){input_at=nanos();++inputs;}
inline void frame(){
  static auto last=Clock::now(),report=last;
  static std::array<double,600> frames{};static std::size_t used=0;static unsigned late=0;
  static double longest=0,input_max=0;const auto now=Clock::now();
  const double ms=std::chrono::duration<double,std::milli>(now-last).count();last=now;
  frames[used++%frames.size()]=ms;longest=std::max(longest,ms);if(ms>25)++late;
  if(const auto at=input_at.exchange(0);at>0)input_max=std::max(input_max,double(nanos()-at)/1e6);
  if(now-report<std::chrono::seconds(5))return;
  const double seconds=std::chrono::duration<double>(now-report).count();report=now;
  const auto n=std::min(used,frames.size());auto sorted=frames;std::sort(sorted.begin(),sorted.begin()+n);
  const auto percentile=[&](double p){return n?sorted[std::min(n-1,std::size_t((n-1)*p))]:0.;};
  FILE* file=nullptr;
  if(output){if(auto* check=std::fopen(output,"rb")){std::fseek(check,0,SEEK_END);const auto size=std::ftell(check);std::fclose(check);if(size>2*1024*1024){char old[320];std::snprintf(old,sizeof old,"%s.previous",output);std::rename(output,old);}}file=std::fopen(output,"a");}
  if(file)std::fprintf(file,"PERF v1 page=%d selected=%d fps=%.1f frame_ms_p50=%.2f p95=%.2f p99=%.2f max=%.2f late_gt25ms=%u inputs=%u input_to_present_max_ms=%.2f playlists=%zu albums=%zu songs=%zu art_bytes=%zu art_pending=%zu canvas_resets=%u canvas_loads=%u",page.load(),selection.load(),used/seconds,percentile(.5),percentile(.95),percentile(.99),longest,late,inputs.exchange(0),input_max,playlists.load(),albums.load(),songs.load(),art_bytes.load(),art_pending.load(),canvas_resets.exchange(0),canvas_loads.exchange(0));
  static const char* names[]={"snapshot","ui_cpu","art_upload_cpu","art_decode","still_blur","canvas_decode","canvas_upload_cpu","present_wait","text_fit"};
  for(int i=0;i<Count;++i){auto& c=counters[i];const auto count=c.calls.exchange(0),total=c.ns.exchange(0),peak=c.peak.exchange(0);if(file)std::fprintf(file," %s_avg_ms=%.3f %s_max_ms=%.3f %s_calls=%llu",names[i],count?double(total)/count/1e6:0.,names[i],double(peak)/1e6,names[i],static_cast<unsigned long long>(count));}
  if(file){std::fputc('\n',file);std::fclose(file);}used=0;late=0;longest=0;input_max=0;
}
}
