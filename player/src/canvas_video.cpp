// SPDX-License-Identifier: GPL-3.0-or-later
// Decorative, silent Canvas. No audio decoder/device or Spotify commands.
#include "canvas_video.hpp"
#include "perf.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <cctype>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>
#include <stb_image.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using Socket=SOCKET;
#define VIDEO_CLOSE closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <pthread.h>
using Socket=int;
#define VIDEO_CLOSE close
#endif
#ifdef PLAYER_CANVAS_FFMPEG
extern "C"{
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}
#endif
namespace player::canvas {
namespace {
using Clock=std::chrono::steady_clock;
struct Frame{std::vector<unsigned char> rgba;int w=0,h=0;double pts=0;};
std::vector<unsigned char> download(const std::string& path){
 if(path.rfind("/media/canvas/",0)!=0||path.size()!=54)return {};
 Socket fd=socket(AF_INET,SOCK_STREAM,0);if(fd==Socket(-1))return {};
#ifdef _WIN32
 DWORD timeout=2000;setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char*>(&timeout),sizeof timeout);
 setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,reinterpret_cast<const char*>(&timeout),sizeof timeout);
#else
 timeval timeout{2,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof timeout);setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof timeout);
#ifdef SO_NOSIGPIPE
 int yes=1;setsockopt(fd,SOL_SOCKET,SO_NOSIGPIPE,&yes,sizeof yes);
#endif
#endif
 sockaddr_in address{};address.sin_family=AF_INET;address.sin_port=htons(38577);address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
 if(connect(fd,reinterpret_cast<sockaddr*>(&address),sizeof address)){VIDEO_CLOSE(fd);return {};}
 std::string request="GET "+path+" HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";std::size_t sent=0;
 while(sent<request.size()){int n=send(fd,request.data()+sent,int(request.size()-sent),0);if(n<=0){VIDEO_CLOSE(fd);return {};}sent+=n;}
 std::vector<unsigned char> bytes;char block[32768];int n=0;
 while((n=recv(fd,block,sizeof block,0))>0){
  if(bytes.size()+std::size_t(n)>16*1024*1024+4096){VIDEO_CLOSE(fd);return {};}
  bytes.insert(bytes.end(),block,block+n);
 }
 VIDEO_CLOSE(fd);if(n<0)return {};
 const std::string header(bytes.begin(),bytes.begin()+std::min<std::size_t>(bytes.size(),4096));
 const auto split=header.find("\r\n\r\n");if(split==std::string::npos||header.compare(0,13,"HTTP/1.1 200 "))return {};
 auto length_header=header.substr(0,split);std::transform(length_header.begin(),length_header.end(),length_header.begin(),[](unsigned char c){return char(std::tolower(c));});
 auto length_at=length_header.find("\r\ncontent-length:");if(length_at==std::string::npos)return {};
 const auto declared=std::strtoull(length_header.c_str()+length_at+17,nullptr,10);
 if(!declared||declared>16*1024*1024||declared!=bytes.size()-split-4)return {};
 return {bytes.begin()+std::ptrdiff_t(split+4),bytes.end()};
}
class Video{
public:
 std::mutex mutex;std::condition_variable wake;std::atomic<bool> stop{false};std::atomic<unsigned> generation{1};
 std::string wanted,kind,error;std::deque<Frame> frames;
 bool moving=false;int image=0,w=0,h=0;unsigned uploaded_generation=0;
 float fade=0;double clock=0;Clock::time_point last=Clock::now();
 // H.264 slice decoding needs far more stack than the console's default thread
 // stack (a SIGSEGV in ff_h264_queue_decode_slice at the stack pointer). The
 // thread starts last, after every member exists.
#ifdef _WIN32
 std::thread worker;
 Video(){worker=std::thread([this]{run();});}
 void join(){if(worker.joinable())worker.join();}
#else
 pthread_t worker{};bool started=false;
 Video(){
  pthread_attr_t attr;pthread_attr_init(&attr);pthread_attr_setstacksize(&attr,4u<<20);
  started=pthread_create(&worker,&attr,[](void* self)->void*{static_cast<Video*>(self)->run();return nullptr;},this)==0;
  pthread_attr_destroy(&attr);
 }
 void join(){if(started){pthread_join(worker,nullptr);started=false;}}
#endif
 ~Video(){shutdown();}
 void shutdown(){stop=true;wake.notify_all();join();}
 bool cancelled(unsigned g){return stop||generation!=g;}
 bool enqueue(Frame frame,unsigned g){
  std::unique_lock lock(mutex);wake.wait(lock,[&]{return cancelled(g)||frames.size()<2;});
  if(cancelled(g))return false;frames.push_back(std::move(frame));return true;
 }
 void fail(unsigned g,const char* reason){std::lock_guard lock(mutex);if(generation==g)error=reason;}
#ifdef PLAYER_CANVAS_FFMPEG
 struct Input{const std::vector<unsigned char>* bytes;std::size_t offset=0;};
 static int read(void* p,uint8_t* out,int size){auto& i=*static_cast<Input*>(p);const auto left=i.bytes->size()-i.offset;if(!left)return AVERROR_EOF;size=int(std::min<std::size_t>(std::max(0,size),left));std::memcpy(out,i.bytes->data()+i.offset,size);i.offset+=size;return size;}
 static int64_t seek(void* p,int64_t offset,int whence){auto& i=*static_cast<Input*>(p);if(whence==AVSEEK_SIZE)return i.bytes->size();whence&=~AVSEEK_FORCE;int64_t base=whence==SEEK_SET?0:whence==SEEK_CUR?i.offset:whence==SEEK_END?i.bytes->size():-1;
  if(base<0||offset<-base||offset>int64_t(i.bytes->size())-base)return AVERROR(EINVAL);i.offset=base+offset;return i.offset;}
 void decode(const std::vector<unsigned char>& bytes,unsigned g){
  Input input{&bytes};auto* io=avio_alloc_context(static_cast<unsigned char*>(av_malloc(32768)),32768,0,&input,read,nullptr,seek);
  if(!io){fail(g,"Canvas decoder allocation failed");return;}
  AVFormatContext* format=avformat_alloc_context();AVCodecContext* decoder=nullptr;AVFrame* frame=av_frame_alloc();AVPacket* packet=av_packet_alloc();SwsContext* convert=nullptr;
  const auto clean=[&]{sws_freeContext(convert);av_packet_free(&packet);av_frame_free(&frame);avcodec_free_context(&decoder);avformat_close_input(&format);av_freep(&io->buffer);avio_context_free(&io);};
  if(!format||!frame||!packet){clean();fail(g,"Canvas decoder allocation failed");return;}
  format->pb=io;format->flags|=AVFMT_FLAG_CUSTOM_IO;format->probesize=1024*1024;format->max_analyze_duration=1000000;
  if(avformat_open_input(&format,nullptr,nullptr,nullptr)<0||avformat_find_stream_info(format,nullptr)<0){clean();fail(g,"Unsupported Canvas file");return;}
  int stream=av_find_best_stream(format,AVMEDIA_TYPE_VIDEO,-1,-1,nullptr,0);
  if(stream<0){clean();fail(g,"Canvas has no video");return;}
  auto* track=format->streams[stream];const auto* codec=avcodec_find_decoder(track->codecpar->codec_id);
  if(!codec||track->codecpar->codec_id!=AV_CODEC_ID_H264||track->codecpar->width<=0||track->codecpar->height<=0||track->codecpar->width>1920||track->codecpar->height>1920||
     (format->duration>0&&format->duration>20*AV_TIME_BASE)){clean();fail(g,"Unsupported Canvas codec or dimensions");return;}
  decoder=avcodec_alloc_context3(codec);
  if(!decoder||avcodec_parameters_to_context(decoder,track->codecpar)<0){clean();fail(g,"Canvas codec setup failed");return;}
  decoder->thread_count=1;decoder->max_pixels=1920*1920;
  if(avcodec_open2(decoder,codec,nullptr)<0){clean();fail(g,"Canvas codec setup failed");return;}
  const auto rate=av_guess_frame_rate(format,track,nullptr);double step=rate.num>0&&rate.den>0?double(rate.den)/rate.num:1./30.;
  if(step<1./60.){clean();fail(g,"Canvas frame rate is unsupported");return;}
  double loop=0,first=-1,last_pts=0;unsigned loops=0;bool failed=false;
  const auto receive=[&](){
   while(!cancelled(g)){
    const auto decode_at=perf::Clock::now();
    const int rc=avcodec_receive_frame(decoder,frame);
    if(rc==AVERROR(EAGAIN)||rc==AVERROR_EOF)return true;
    if(rc<0)return false;
    const double scale=std::min(1.,960./std::max(frame->width,frame->height));Frame out;
    out.w=std::max(1,int(frame->width*scale));out.h=std::max(1,int(frame->height*scale));
    if(out.w>1920||out.h>1920)return false;
    convert=sws_getCachedContext(convert,frame->width,frame->height,AVPixelFormat(frame->format),out.w,out.h,AV_PIX_FMT_RGBA,SWS_BILINEAR,nullptr,nullptr,nullptr);
    if(!convert)return false;
    out.rgba.resize(std::size_t(out.w)*out.h*4);uint8_t* dest[]={out.rgba.data(),nullptr,nullptr,nullptr};int pitch[]={out.w*4,0,0,0};
    if(sws_scale(convert,frame->data,frame->linesize,0,frame->height,dest,pitch)<=0)return false;
    double pts=frame->best_effort_timestamp!=AV_NOPTS_VALUE?frame->best_effort_timestamp*av_q2d(track->time_base):last_pts+step;
    if(first<0)first=pts;pts=std::max(last_pts,std::max(0.,pts-first));last_pts=pts;out.pts=loop+pts;
    av_frame_unref(frame);perf::sample(perf::CanvasDecode,std::chrono::duration_cast<std::chrono::nanoseconds>(perf::Clock::now()-decode_at).count());if(!enqueue(std::move(out),g))return true;
   }
   return true;
  };
  while(!cancelled(g)){
   int rc=av_read_frame(format,packet);
   if(rc<0){
    avcodec_send_packet(decoder,nullptr);if(!receive()){failed=true;break;}
    if(last_pts<.01||++loops>100000||av_seek_frame(format,stream,0,AVSEEK_FLAG_BACKWARD)<0){failed=true;break;}
    loop+=last_pts+step;first=-1;last_pts=0;avcodec_flush_buffers(decoder);continue;
   }
   if(packet->stream_index==stream){
    rc=avcodec_send_packet(decoder,packet);
    if(rc==AVERROR(EAGAIN)){if(!receive()){failed=true;break;}rc=avcodec_send_packet(decoder,packet);}
    if(rc<0||!receive()){failed=true;break;}
   }
   av_packet_unref(packet); // All audio packets are discarded, never decoded.
  }
  clean();if(failed&&!cancelled(g))fail(g,"Canvas video could not be decoded");
 }
#endif
 void run(){
#ifdef _WIN32
  WSADATA data{};WSAStartup(MAKEWORD(2,2),&data);
#endif
  unsigned consumed=0;
  while(!stop){
   unsigned g;std::string path,type;
   {std::unique_lock lock(mutex);wake.wait(lock,[&]{return stop||generation!=consumed;});if(stop)break;g=generation;consumed=g;path=wanted;type=kind;}
   if(path.empty())continue;++perf::canvas_loads;auto bytes=download(path);if(cancelled(g))continue;if(bytes.empty()){fail(g,"Canvas could not be loaded");continue;}
   if(type=="image"){
    int w=0,h=0,n=0;if(!stbi_info_from_memory(bytes.data(),int(bytes.size()),&w,&h,&n)||w<=0||h<=0||w>1920||h>1920){fail(g,"Canvas image is unsupported");continue;}
    auto* rgba=stbi_load_from_memory(bytes.data(),int(bytes.size()),&w,&h,&n,4);
    if(!rgba){fail(g,"Canvas image is unsupported");continue;}
    Frame frame;frame.w=w;frame.h=h;frame.rgba.assign(rgba,rgba+std::size_t(w)*h*4);stbi_image_free(rgba);enqueue(std::move(frame),g);
   }else{
#ifdef PLAYER_CANVAS_FFMPEG
    decode(bytes,g);
#else
    fail(g,"Video Canvas is available on the PS5 build");
#endif
   }
  }
#ifdef _WIN32
 WSACleanup();
#endif
 }
};
Video& video(){static Video v;return v;}
}
void update(NVGcontext* vg,const Model& m,bool visible){
 auto& v=video();const auto at=Clock::now();
 {std::lock_guard lock(v.mutex);const auto wanted=m.has_canvas?m.canvas_path:std::string();
  if(wanted!=v.wanted){v.wanted=wanted;v.kind=m.canvas_kind;v.error.clear();v.frames.clear();++v.generation;++perf::canvas_resets;v.clock=0;v.fade=0;v.wake.notify_all();}
  const unsigned g=v.generation;
  if(v.uploaded_generation!=g){if(v.image)nvgDeleteImage(vg,v.image);v.image=0;v.w=v.h=0;v.uploaded_generation=g;}
  if(v.image&&m.moving&&visible)v.clock+=std::clamp(std::chrono::duration<double>(at-v.last).count(),0.,.1);
  if(v.image)v.fade=std::min(1.f,v.fade+float(std::chrono::duration<double>(at-v.last).count())/.3f);
  v.last=at;
  // Keep the last texture while another page covers it. The bounded decoder queue
  // blocks after two frames; returning to the player never restarts its asset.
  if(!visible)return;
  if(!v.frames.empty()&&(!v.image||v.frames.front().pts<=v.clock+.01)){
   perf::Scope upload_time(perf::CanvasUpload);
   auto f=std::move(v.frames.front());v.frames.pop_front();v.wake.notify_all();
   if(v.image&&v.w==f.w&&v.h==f.h)nvgUpdateImage(vg,v.image,f.rgba.data());
   else{if(v.image)nvgDeleteImage(vg,v.image);v.image=nvgCreateImageRGBA(vg,f.w,f.h,0,f.rgba.data());v.w=f.w;v.h=f.h;}
  }
 }
}
bool paint(NVGcontext* vg,float x,float y,float w,float h,float alpha){
 auto& v=video();if(!v.image||v.w<=0||v.h<=0)return false;
 const float scale=std::max(w/v.w,h/v.h),iw=v.w*scale,ih=v.h*scale;
 nvgBeginPath(vg);nvgRect(vg,x,y,w,h);nvgFillPaint(vg,nvgImagePattern(vg,x+(w-iw)/2,y+(h-ih)/2,iw,ih,0,v.image,alpha*v.fade));nvgFill(vg);return true;
}
void shutdown(){video().shutdown();}
}
