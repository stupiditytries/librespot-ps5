// SPDX-License-Identifier: GPL-3.0-or-later
// Borealis client only. Playback remains in the background receiver.
#include "draw.hpp"
#include "screen.hpp"
#include "canvas_video.hpp"
#include "link.hpp"
#include "keyboard.hpp"
#include "perf.hpp"
#include <borealis/core/activity.hpp>
#include <borealis/core/application.hpp>
#include <borealis/core/platform.hpp>
#include <borealis/core/theme.hpp>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#include <stdexcept>
#include "display.hpp"
#define NANOVG_GL3_IMPLEMENTATION
#include <nanovg_gl.h>
extern "C" {
int sceUserServiceInitialize(void*);int sceUserServiceGetInitialUser(int*);
int scePadInit();int scePadOpen(int,int,int,void*);int scePadReadState(int,void*);
int sceKernelDebugOutText(int,const char*);
int sceSystemServiceLoadExec(const char*,const char* const*);
}
void log_line(const char* format,...){
    char text[512];va_list args;va_start(args,format);std::vsnprintf(text,sizeof text,format,args);va_end(args);
    if(auto* f=std::fopen("/download0/player.log","a")){std::fprintf(f,"%s\n",text);std::fclose(f);}
    sceKernelDebugOutText(0,text);
}
namespace {
class Video final:public VideoContext {
    Display display_;Canvas canvas_;NVGcontext* vg_=nullptr;unsigned present_failures_=0;
public:
    Video(){if(!display_.open()||!canvas_.create(display_.width,display_.height))throw std::runtime_error("Display setup failed");
        vg_=nvgCreateGL3(NVG_ANTIALIAS|NVG_STENCIL_STROKES);if(!vg_)throw std::runtime_error("NanoVG setup failed");
        sizeW=display_.width;sizeH=display_.height;brls::Application::onWindowResized(display_.width,display_.height);}
    ~Video()override{if(vg_)nvgDeleteGL3(vg_);}
    void clear(NVGcolor c)override{glClearColor(c.r,c.g,c.b,c.a);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT|GL_STENCIL_BUFFER_BIT);}
    void beginFrame()override{canvas_.begin();}
    void endFrame()override{
        canvas_.show(display_.width,display_.height);
        bool presented;
        {player::perf::Scope present_time(player::perf::Present);presented=display_.present();}
        if(!presented){
            if(!present_failures_||present_failures_%60==0)log_line("Display present failed EGL=%x consecutive=%u",eglGetError(),present_failures_+1);
            ++present_failures_;usleep(16000);
            if(present_failures_>=180)brls::Application::quit();
        }else present_failures_=0;
        player::perf::frame();
    }
    void resetState()override{canvas_.begin();}
    double getScaleFactor()override{return 1;}
    NVGcontext* getNVGContext()override{return vg_;}
    void setSwapInterval(int n)override{eglSwapInterval(display_.display,n);}
};
class Input final:public brls::InputManager {
    int handle_=-1;
public:
    Input(){int user=0;sceUserServiceInitialize(nullptr);if(sceUserServiceGetInitialUser(&user)>=0&&scePadInit()>=0)handle_=scePadOpen(user,0,0,nullptr);}
    short getControllersConnectedCount()override{return handle_>=0?1:0;}
    void updateUnifiedControllerState(brls::ControllerState* state)override{
        *state={};if(handle_<0)return;alignas(8)unsigned char data[136]{};if(scePadReadState(handle_,data)<0)return;
        unsigned buttons=0;std::memcpy(&buttons,data,4);int x=int(data[4])-128,y=int(data[5])-128;
        if(x<-70)buttons|=0x80;if(x>70)buttons|=0x20;if(y<-70)buttons|=0x10;if(y>70)buttons|=0x40;
        const struct{unsigned mask;brls::ControllerButton button;}mapping[]={{0x10,brls::BUTTON_NAV_UP},{0x20,brls::BUTTON_NAV_RIGHT},
            {0x40,brls::BUTTON_NAV_DOWN},{0x80,brls::BUTTON_NAV_LEFT},{0x4000,brls::BUTTON_A},{0x2000,brls::BUTTON_B},
            {0x8000,brls::BUTTON_X},{0x1000,brls::BUTTON_Y},{0x400,brls::BUTTON_LB},{0x800,brls::BUTTON_RB}};
        for(const auto& m:mapping)state->buttons[m.button]=(buttons&m.mask)!=0;
    }
    void updateControllerState(brls::ControllerState* state,int)override{updateUnifiedControllerState(state);}
    bool getKeyboardKeyState(brls::BrlsKeyboardScancode)override{return false;}
    void updateTouchStates(std::vector<brls::RawTouchState>* s)override{s->clear();}
    void updateMouseStates(brls::RawMouseState* s)override{*s={};}
    void sendRumble(unsigned short,unsigned short,unsigned short)override{}
};
class Fonts final:public brls::FontLoader{void loadFonts()override{if(!player::draw::load_fonts("/app0/assets/fonts"))log_line("Font loading failed");}};
class Keyboard final:public brls::ImeManager {
    ps5::Keyboard keyboard_;std::function<void(std::string)> callback_;
public:
    bool openForText(std::function<void(std::string)> callback,std::string,std::string,int,std::string initial,int)override{
        if(!keyboard_.open(initial))return false;callback_=std::move(callback);return true;}
    bool openForNumber(std::function<void(long)>,std::string,std::string,int,std::string,std::string,std::string,int)override{return false;}
    void tick(){std::string text;auto result=keyboard_.poll(text);if(result==ps5::Keyboard::Result::Done&&callback_)callback_(text);
        if(result==ps5::Keyboard::Result::Cancelled&&callback_)callback_("");
        if(result!=ps5::Keyboard::Result::None)callback_={};}
};
class Platform final:public brls::Platform {
    std::unique_ptr<Video> video_;std::unique_ptr<Input> input_;Fonts fonts_;Keyboard keyboard_;brls::NullAudioPlayer audio_;
public:
    void createWindow(std::string,uint32_t,uint32_t,float,float)override{video_=std::make_unique<Video>();input_=std::make_unique<Input>();}
    bool mainLoopIteration()override{keyboard_.tick();return true;}
    std::string getName()override{return "PS5 player";}
    VideoContext* getVideoContext()override{return video_.get();}
    brls::InputManager* getInputManager()override{return input_.get();}
    brls::AudioPlayer* getAudioPlayer()override{return &audio_;}
    brls::ImeManager* getImeManager()override{return &keyboard_;}
    brls::FontLoader* getFontLoader()override{return &fonts_;}
    brls::ThemeVariant getThemeVariant()override{return brls::ThemeVariant::DARK;}
    void setThemeVariant(brls::ThemeVariant)override{}
    std::string getLocale()override{return brls::LOCALE_EN_US;}
    bool canShowBatteryLevel()override{return false;}bool canShowWirelessLevel()override{return false;}
    int getBatteryLevel()override{return 100;}bool isBatteryCharging()override{return false;}int getWirelessLevel()override{return 0;}
    std::string getIpAddress()override{return {};}std::string getDnsServer()override{return {};}
    void disableScreenDimming(bool,const std::string&,const std::string&)override{}bool isScreenDimmingDisabled()override{return false;}
    void setBacklightBrightness(float)override{}float getBacklightBrightness()override{return 1;}bool canSetBacklightBrightness()override{return false;}
    bool isApplicationMode()override{return true;}void exitToHomeMode(bool)override{}void forceEnableGamePlayRecording()override{}void openBrowser(std::string)override{}
};
}
brls::Platform* brls::Platform::createPlatform(){return new ::Platform;}
// The loader shim's POSIX exit syscall raises SIGSYS in a native big app.
// main's local workers have already joined before this hook runs.
extern "C" void catchReturnFromMain(int status) {
    log_line("App stopped status=%d; requesting system exit",status);
    sceSystemServiceLoadExec("exit",nullptr);
    for(;;)usleep(100000);
}
int main(){
    player::perf::output="/download0/player-perf.log";
    const int app_lock=open("/app0/.player-lock",O_RDONLY);
    if(app_lock<0||flock(app_lock,LOCK_EX|LOCK_NB))return 1;
    log_line("Player starting build=librespot-v2-lyrics-synced-only; audio is owned by the receiver; diagnostics=/download0/player-perf.log");
    player::Client link;player::App app;app.model=link.snapshot();
    app.update=[&]{link.snapshot_into(app.model);};
    app.send=[&](player::Cmd cmd,int value){
        if(cmd==player::Cmd::open_search){link.send(cmd,0,app.model);auto* ime=brls::Application::getPlatform()->getImeManager();
            if(!ime->openForText([&](std::string text){link.search(text);},"Search","",160,"",0))link.search(" ");}
        else link.send(cmd,value,app.model);
    };
    brls::Application::ORIGINAL_WINDOW_WIDTH=1920;brls::Application::ORIGINAL_WINDOW_HEIGHT=1080;
    brls::Logger::setLogLevel(brls::LogLevel::LOG_WARNING);
    if(!brls::Application::init())return 1;brls::Application::createWindow("librespot-ps5");brls::Application::setGlobalQuit(false);
    auto* screen=player::make_screen(&app,{},[]{brls::Application::quit();});player::set_clock_running(screen,false);
    brls::Application::pushActivity(new brls::Activity(player::as_view(screen)),brls::TransitionAnimation::NONE);player::give_focus(screen);
    while(brls::Application::mainLoop()){}
    player::canvas::shutdown();
    flock(app_lock,LOCK_UN);close(app_lock);return 0;
}
