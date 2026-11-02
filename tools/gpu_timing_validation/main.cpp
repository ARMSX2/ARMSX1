// Execute the actual timer's lifecycle against a nonblocking fake GPU clock.
#include "gpu_stage_timer.h"
#include <cassert>
#include <cstdio>
#include <unordered_map>
namespace {
struct Query {unsigned begin=0,elapsed=0;};
std::unordered_map<unsigned,Query> queries;
unsigned next=1,active=0,clockTick=0,generated=0,deleted=0,begins=0,ends=0,results=0,availability=0;
bool ready=true,disjoint=false;
void reset(){assert(queries.empty());next=1;active=clockTick=generated=deleted=begins=ends=results=availability=0;ready=true;disjoint=false;}
}
void glGetIntegerv(GLenum name,GLint* value){assert(name==GL_GPU_DISJOINT_EXT);*value=disjoint;}
void glGenQueries(GLsizei n,GLuint* out){for(int i=0;i<n;i++){out[i]=next++;queries[out[i]]={};}generated+=n;}
void glDeleteQueries(GLsizei n,const GLuint* ids){for(int i=0;i<n;i++)assert(queries.erase(ids[i])==1);deleted+=n;}
void glBeginQuery(GLenum name,GLuint id){assert(name==GL_TIME_ELAPSED_EXT&&active==0&&queries.count(id));active=id;queries[id].begin=clockTick;begins++;}
void glEndQuery(GLenum name){assert(name==GL_TIME_ELAPSED_EXT&&active);queries[active].elapsed=clockTick-queries[active].begin;active=0;ends++;}
void glGetQueryObjectuiv(GLuint id,GLenum name,GLuint* value){assert(queries.count(id));if(name==GL_QUERY_RESULT_AVAILABLE){availability++;*value=ready;}else{assert(name==GL_QUERY_RESULT&&ready);results++;*value=queries[id].elapsed;}}
int main(){
    {
        GpuStageTimer timer;timer.init("GL_EXT_disjoint_timer_query");assert(timer.available==2);
        for(int i=0;i<5000;i++){GpuStageTimer::Scope idle(timer,GpuStageTimer::DRAW);timer.poll();}
        assert(generated==0&&begins==0&&ends==0&&results==0&&availability==0);
        timer.release();assert(queries.empty());
    }
    reset();{
        GpuStageTimer timer;timer.init("GL_EXT_disjoint_timer_query");timer.setEnabled(true);ready=false;
        {GpuStageTimer::Scope draw(timer,GpuStageTimer::DRAW);clockTick+=7;{GpuStageTimer::Scope resolve(timer,GpuStageTimer::RESOLVE);clockTick+=11;}clockTick+=13;}
        timer.poll();assert(results==0&&timer.nanos[0]==0);ready=true;timer.poll();
        assert(timer.nanos[GpuStageTimer::DRAW]==20&&timer.nanos[GpuStageTimer::RESOLVE]==11);
        assert(timer.spans[GpuStageTimer::DRAW]==2&&timer.spans[GpuStageTimer::RESOLVE]==1);
        timer.setEnabled(false);assert(timer.available==2&&queries.empty());
        unsigned started=begins;for(int i=0;i<5000;i++){GpuStageTimer::Scope off(timer,GpuStageTimer::DRAW);timer.poll();}assert(begins==started);
        timer.setEnabled(true);{GpuStageTimer::Scope draw(timer,GpuStageTimer::DRAW);clockTick+=5;}timer.poll();assert(timer.nanos[0]==25);
        timer.release();assert(generated==deleted&&begins==ends&&queries.empty());
    }
    reset();{
        GpuStageTimer timer;timer.init("GL_EXT_disjoint_timer_query");timer.setEnabled(true);ready=false;
        for(int i=0;i<10000;i++){GpuStageTimer::Scope draw(timer,GpuStageTimer::DRAW);clockTick++;}
        assert(generated==8192&&timer.skipped==1808&&results==0);
        ready=true;disjoint=true;timer.poll();assert(timer.nanos[0]==0&&timer.disjoint==8192);
        disjoint=false;{GpuStageTimer::Scope draw(timer,GpuStageTimer::DRAW);clockTick+=17;}timer.poll();assert(timer.nanos[0]==17);
        ready=false;{GpuStageTimer::Scope draw(timer,GpuStageTimer::DRAW);clockTick++;}
        unsigned readBefore=results;timer.setEnabled(false);assert(results==readBefore&&queries.empty());
        timer.release();assert(generated==deleted&&begins==ends);
    }
    reset();{
        GpuStageTimer timer;timer.init("GL_EXT_other_extension");timer.setEnabled(true);{GpuStageTimer::Scope draw(timer,GpuStageTimer::DRAW);}
        timer.poll();timer.release();assert(timer.available==0&&generated==0&&begins==0);
    }
    std::puts("GPU timing lifecycle passed: disabled scopes, nested accounting, nonblocking results, bounded pool, disjoint rejection and toggles.");
}
