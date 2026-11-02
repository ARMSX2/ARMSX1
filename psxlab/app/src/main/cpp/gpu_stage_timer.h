#pragma once
#include <GLES3/gl31.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#ifndef GL_TIME_ELAPSED_EXT
#define GL_TIME_ELAPSED_EXT 0x88BF
#endif
#ifndef GL_GPU_DISJOINT_EXT
#define GL_GPU_DISJOINT_EXT 0x8FBB
#endif

/** GPU execution time per renderer stage (GL_EXT_disjoint_timer_query), for reports. Results are read
 * only once the GPU marks them available, so timing never stalls rendering. Stages nest: entering one
 * pauses the enclosing stage, so each nanosecond is counted once. GL thread only, except the totals. */
struct GpuStageTimer {
    enum Stage {DRAW,RESOLVE,OUTPUT,TRANSFER,UPLOAD,COUNT};
    std::array<std::atomic<int64_t>,COUNT> nanos{},spans{};
    std::atomic<int64_t> skipped{0},disjoint{0},available{0};
    bool supported=false;
    void init(const std::string& extensions){
        supported=extensions.find("GL_EXT_disjoint_timer_query")!=std::string::npos;available=supported?1:0;
        if(!supported)return;
        grow();
        GLint clear=0;glGetIntegerv(GL_GPU_DISJOINT_EXT,&clear);
    }
    void release(){
        if(!supported)return;
        for(const auto& p:pending)free.push_back(p.query);pending.clear();if(active)free.push_back(active);active=0;
        if(!free.empty())glDeleteQueries(GLsizei(free.size()),free.data());free.clear();supported=false;
    }
    struct Scope {
        GpuStageTimer& timer;
        Scope(GpuStageTimer& t,Stage stage):timer(t){timer.enter(stage);}
        ~Scope(){timer.leave();}
        Scope(const Scope&)=delete;Scope& operator=(const Scope&)=delete;
    };
    void poll(){
        if(!supported||pending.empty())return;
        GLint lost=0;glGetIntegerv(GL_GPU_DISJOINT_EXT,&lost);
        while(!pending.empty()){
            GLuint ready=0;glGetQueryObjectuiv(pending.front().query,GL_QUERY_RESULT_AVAILABLE,&ready);if(!ready)break;
            GLuint value=0;glGetQueryObjectuiv(pending.front().query,GL_QUERY_RESULT,&value);
            if(lost)disjoint++;else{nanos[pending.front().stage]+=value;spans[pending.front().stage]++;}
            free.push_back(pending.front().query);pending.pop_front();
        }
    }
private:
    struct Pending{GLuint query;int stage;};
    std::vector<GLuint> free;std::deque<Pending> pending;std::vector<int> stack;
    GLuint active=0;int activeStage=0;
    // A heavy scene keeps ~30 spans a frame in flight for a second or more; grow rather than skip.
    static constexpr size_t MAX_QUERIES=8192;size_t created=0;
    void grow(){size_t n=created==0?512:std::min(created,MAX_QUERIES-created);if(!n)return;size_t at=free.size();free.resize(at+n);glGenQueries(GLsizei(n),free.data()+at);created+=n;}
    void start(int stage){
        poll();
        if(free.empty())grow();
        if(free.empty()){skipped++;return;}
        active=free.back();free.pop_back();activeStage=stage;glBeginQuery(GL_TIME_ELAPSED_EXT,active);
    }
    void stop(){if(!active)return;glEndQuery(GL_TIME_ELAPSED_EXT);pending.push_back({active,activeStage});active=0;}
    void enter(Stage stage){if(!supported)return;stop();stack.push_back(stage);start(stage);}
    void leave(){if(!supported||stack.empty())return;stop();stack.pop_back();if(!stack.empty())start(stack.back());}
};
