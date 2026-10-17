#pragma once
#include "files.hpp"
#include "feature.hpp"
#include "migration.hpp"
#include <dirent.h>
namespace usb_free {
template<class T> static std::size_t read_wire(const char* base,const char* name,T& value) {
    char p[256];path(p,sizeof p,base,name);FILE* f=std::fopen(p,"rb");if(!f)return 0;
    unsigned char bytes[sizeof(T)]{};const auto n=std::fread(bytes,1,sizeof bytes,f);
    const bool end=std::fgetc(f)==EOF;std::fclose(f);
    if(!end)return 0;
    std::memcpy(&value,bytes,n);return n;
}
static bool collect_previous(const char* base,pid_t pid,std::uint64_t shell_epoch,PriorSources& prior) {
    Resident r;
    if(!read_file(base,"resident.bin",r)) {
        char p[256];struct stat info{};path(p,sizeof p,base,"resident.bin");
        if(stat(p,&info)==0||errno!=ENOENT){note("REFUSED malformed/unreadable previous resident: %s",base);return false;}
        Resident attempt;
        if(read_file(base,"attempt.bin",attempt)&&attempt.shell_pid==pid&&attempt.shell_epoch==shell_epoch) {
            note("REFUSED unresolved previous injection: %s",base);return false;
        }
        return true;
    }
    if(r.shell_pid!=pid||r.shell_epoch!=shell_epoch)return true;
    Request q;
    const auto size=read_wire(base,"request.bin",q);
    if((size!=120&&size!=128)||q.size!=size||!prior_identity(q,r,pid,shell_epoch)) {
        note("REFUSED unsupported/mismatched previous request: %s",base);return false;
    }
    if(q.protocol>=3) {
        char p[256];path(p,sizeof p,base,"retire.txt");FILE* f=std::fopen(p,"w");
        if(!f)return false;
        const bool written=std::fprintf(f,"retire %016llx\n",(unsigned long long)q.token)>0;
        if(std::fclose(f)||!written)return false;
        note("RETIRE requested for previous generation %s; no process/thread is killed",base);
    }
    const auto began=now_ms();Status s;bool ready=false;
    do {
        const auto status_size=read_wire(base,"status.bin",s);
        Heartbeat h;
        const bool parent_gone=read_file(base,"heartbeat.bin",h)&&h.token==q.token&&
            h.at_ms<=now_ms()&&now_ms()-h.at_ms>=3000;
        bool retired=q.protocol<3;
        if(q.protocol>=3) {
            Retirement a;
            retired=read_file(base,"retired.bin",a)&&a.signature==magic&&a.build==q.build&&
                a.token==q.token&&a.shell_pid==pid&&a.shell_epoch==shell_epoch&&a.protocol==q.protocol;
        }
        ready=status_size==(q.protocol==1?168U:192U)&&parent_gone&&retired&&prior_terminal(q,r,s,pid,shell_epoch);
        if(ready)break;
        usleep(100000);
    }while(now_ms()-began<8000);
    if(!ready){note("REFUSED previous worker is not confirmed idle with successful Stop/Remove: %s",base);return false;}
    char feature_path[256];path(feature_path,sizeof feature_path,base,"feature-plan.bin");
    struct stat feature_info{};
    if(!stat(feature_path,&feature_info)) {
        FeaturePlan plan;FeatureProgress progress;
        if(!read_file(base,"feature-plan.bin",plan)||!valid(plan,q.token)||
           !read_file(base,"feature-status.bin",progress)||!feature_clean(plan,progress)) {
            note("REFUSED prior AudioTag restoration unconfirmed: %s",base);return false;
        }
    } else if(errno!=ENOENT){note("REFUSED cannot inspect prior AudioTag plan: %s",base);return false;}
    if(s.source>0) {
        bool duplicate=false;for(unsigned i=0;i<prior.count;++i)if(prior.entries[i].source==s.source)duplicate=true;
        if(!duplicate) {
            if(prior.count==8){note("REFUSED too many previous source records");return false;}
            prior.entries[prior.count++]={q.build,q.token,s.source,q.protocol};
        }
    }
    note("PRIOR recorded directory=%s build=%016llx source=%d phase=%u stop=%08x remove=%08x; live native absence still required",
        base,(unsigned long long)q.build,s.source,s.phase,s.stop_rc,s.remove_rc);
    return true;
}
static bool collect_generations(pid_t pid,std::uint64_t shell_epoch,PriorSources& prior) {
    if(!collect_previous("/data/custom-overlays/usb-free-music-test",pid,shell_epoch,prior))return false;
    DIR* dir=opendir("/data/custom-overlays");if(!dir)return false;
    bool ok=true;
    while(auto* e=readdir(dir)) {
        constexpr const char* prefix="usb-free-music-test-v";
        if(std::strncmp(e->d_name,prefix,std::strlen(prefix)))continue;
        const char* suffix=e->d_name+std::strlen(prefix);
        if(!*suffix||std::strspn(suffix,"0123456789")!=std::strlen(suffix))continue;
        char base[256];std::snprintf(base,sizeof base,"/data/custom-overlays/%s",e->d_name);
        if(!std::strcmp(base,directory))continue;
        if(!collect_previous(base,pid,shell_epoch,prior)){ok=false;break;}
    }
    closedir(dir);return ok;
}
}
