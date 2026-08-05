#include "psx.h"
#include "state.h"
#include "perf.h"
#include "pgxp.h"
#include "rewind.h"
#include "cheats.h"

psx_t* psx_create(void) {
    return (psx_t*)calloc(1, sizeof(psx_t));
}

int psx_load_bios(psx_t* psx, const char* path) {
    return psx_bios_load(psx->bios, path);
}

/* psx_save_state / psx_load_state live in psx/state.c. */

void psx_load_exe(psx_t* psx, const char* path) {
    psx_exe_load(psx->cpu, path);
}

/*
    CPU overclock.

    There is no clock to turn up — the R3000A's speed is only ever expressed as how much the REST
    of the machine advances per instruction executed. So overclocking is done by charging the
    devices FEWER cycles for the same work: at 200%, the GPU, timers, CD-ROM, pad and DMA each
    advance half as far per instruction, which is indistinguishable from a CPU running twice as
    fast.

    The division is fractional and the remainder is carried, not discarded. Dropping it would lose
    a fraction of a cycle on every instruction — half a million times a frame — and the timers
    would drift steadily out of step with the GPU, which shows up as audio pitch and frame pacing
    slowly going wrong rather than as anything that looks like a clock bug.

    Games that poll a hardware counter to time themselves can misbehave at anything but 100%.
    That is inherent to the technique, not a defect here: it is why this ships default-off.
*/
static unsigned g_overclock_percent = 100;
static uint32_t g_overclock_carry = 0;

void psx_set_cpu_overclock(unsigned percent) {
    if (percent < 10)
        percent = 10;
    if (percent > 1000)
        percent = 1000;

    g_overclock_percent = percent;
    g_overclock_carry = 0;
}

unsigned psx_get_cpu_overclock(void) {
    return g_overclock_percent;
}

static inline uint32_t psx_overclock_device_cycles(uint32_t cycles) {
    uint64_t scaled;

    if (g_overclock_percent == 100)
        return cycles;

    scaled = (uint64_t)cycles * 100u + g_overclock_carry;
    g_overclock_carry = (uint32_t)(scaled % g_overclock_percent);

    return (uint32_t)(scaled / g_overclock_percent);
}

void psx_update(psx_t* psx) {
    /* Instruction boundary, emulation thread: the one place it is safe to
       swap the whole machine out from under the front-end. Costs a single
       atomic load when nothing is parked. */
    psx_state_service_requests();

    psx_cpu_cycle(psx->cpu);

    /* Computed ONCE and shared: every device must be charged the identical amount, or the
       carry would be applied five times over and they would drift apart from each other
       rather than together. */
    const uint32_t device_cycles = psx_overclock_device_cycles(psx->cpu->last_cycles);

    psx_cdrom_update(psx->cdrom, device_cycles);
    psx_gpu_update(psx->gpu, device_cycles);
    psx_pad_update(psx->pad, device_cycles);
    psx_timer_update(psx->timer, device_cycles);
    psx_dma_update(psx->dma, device_cycles);

    /* The SPU is charged here like every other peripheral so that the Sound RAM interrupt
       fires at the emulated instant the voice reaches the watched address, with the CPU
       still running and able to service it. It used to be advanced only by the front-end,
       after this loop had finished the whole frame, which capped the SPU interrupt rate at
       the video frame rate and starved every game that clocks a streaming refill off it.
       Generates nothing unless the front-end has granted a budget for this frame — see
       psx_spu_begin_frame() and the block comment above psx_spu_tick(). */
    psx_spu_tick(psx->spu, device_cycles);
}

void psx_run_frame(psx_t* psx) {
    float framerate = psx_gpu_frame_rate(psx->gpu);

    unsigned int counter = (float)PSX_CPU_CPS / framerate;

    /* R3000A work for the frame, derived rather than instrumented: the step count IS the
       instruction count, and the CPU already sums executed clocks into total_cycles. That
       keeps psx_cpu_cycle() — which runs half a million times per frame — free of any
       counter at all, while still giving the overlay a real per-frame CPI. */
    const unsigned int steps = counter;
    const uint32_t cycles_before = psx->cpu->total_cycles;

    while (counter--) {
        psx_update(psx);
    }

    /* [cheats] — GameShark codes, once per frame (psx/cheats.h). A branch and one atomic load
       when nothing is armed, which is why it sits here and not inside the loop above.

       The SDL front-end does NOT reach this: it drives psx_update() itself and calls
       psx_cheats_apply() from its own frame boundary (ArmsxSession::runFrame in
       frontend/main.cpp), because only that loop knows where the vblank actually fell. This
       call is for any host that uses the core's own frame loop, so cheats are not something a
       future front-end has to remember to wire up. No host calls both, so nothing double-applies. */
    psx_cheats_apply(psx);

    if (g_psx_perf_enabled) {
        g_psx_perf.cpu_instructions += steps;
        /* total_cycles is a wrapping uint32; the unsigned difference is still exact over
           any single frame. */
        g_psx_perf.cpu_cycles += (uint32_t)(psx->cpu->total_cycles - cycles_before);
    }
}

void* psx_get_display_buffer(psx_t* psx) {
    return psx_gpu_get_display_buffer(psx->gpu);
}

void* psx_get_vram(psx_t* psx) {
    return psx->gpu->vram;
}

uint32_t psx_get_display_width(psx_t* psx) {
    int width = psx_get_dmode_width(psx);

    if (width == 368)
        width = 384;

    return width;
}

uint32_t psx_get_display_height(psx_t* psx) {
    return psx_get_dmode_height(psx);
}

uint32_t psx_get_display_format(psx_t* psx) {
    return (psx->gpu->display_mode >> 4) & 1;
}

uint32_t psx_get_dmode_width(psx_t* psx) {
    static int dmode_hres_table[] = {
        256, 320, 512, 640
    };

    if (psx->gpu->display_mode & 0x40) {
        return 368;
    } else {
        return dmode_hres_table[psx->gpu->display_mode & 0x3];
    }
}

uint32_t psx_get_dmode_height(psx_t* psx) {
    if (psx->gpu->display_mode & 0x4)
        return 480;

    int disp = psx->gpu->disp_y2 - psx->gpu->disp_y1;

    if (disp < (255-16))
        return disp;

    return 240;
}

double psx_get_display_aspect(psx_t* psx) {
    /* Second service point for parked save/load requests, and the only one that
       runs while the front-end has the VM paused: psx_update() stops being
       called then, but the host still presents a frame every tick and this is
       the core call it makes on that path (frontend/main.cpp Session::draw).
       A paused machine is quiescent, so applying a state here is as safe as an
       instruction boundary. No-op (one atomic load) when nothing is parked. */
    psx_state_service_requests();

    double width = psx_get_dmode_width(psx);
    double height = psx_get_dmode_height(psx);

    if (height > width)
        return 4.0 / 3.0;

    double aspect = width / height;

    if (aspect > (4.0 / 3.0))
        return 4.0 / 3.0;

    return aspect;
}

void atcons_tx(void* udata, unsigned char c) {
    putchar(c);
}

int psx_init(psx_t* psx, const char* bios_path, const char* exp_path) {
    memset(psx, 0, sizeof(psx_t));

    psx->bios = psx_bios_create();
    psx->ram = psx_ram_create();
    psx->dma = psx_dma_create();
    psx->exp1 = psx_exp1_create();
    psx->exp2 = psx_exp2_create();
    psx->mc1 = psx_mc1_create();
    psx->mc2 = psx_mc2_create();
    psx->mc3 = psx_mc3_create();
    psx->ic = psx_ic_create();
    psx->scratchpad = psx_scratchpad_create();
    psx->gpu = psx_gpu_create();
    psx->spu = psx_spu_create();
    psx->bus = psx_bus_create();
    psx->cpu = psx_cpu_create();
    psx->timer = psx_timer_create();
    psx->cdrom = psx_cdrom_create();
    psx->pad = psx_pad_create();
    psx->mdec = psx_mdec_create();

    psx_bus_init(psx->bus);

    psx_bus_init_bios(psx->bus, psx->bios);
    psx_bus_init_ram(psx->bus, psx->ram);
    psx_bus_init_dma(psx->bus, psx->dma);
    psx_bus_init_exp1(psx->bus, psx->exp1);
    psx_bus_init_exp2(psx->bus, psx->exp2);
    psx_bus_init_mc1(psx->bus, psx->mc1);
    psx_bus_init_mc2(psx->bus, psx->mc2);
    psx_bus_init_mc3(psx->bus, psx->mc3);
    psx_bus_init_ic(psx->bus, psx->ic);
    psx_bus_init_scratchpad(psx->bus, psx->scratchpad);
    psx_bus_init_gpu(psx->bus, psx->gpu);
    psx_bus_init_spu(psx->bus, psx->spu);
    psx_bus_init_timer(psx->bus, psx->timer);
    psx_bus_init_cdrom(psx->bus, psx->cdrom);
    psx_bus_init_pad(psx->bus, psx->pad);
    psx_bus_init_mdec(psx->bus, psx->mdec);

    // Init devices
    psx_bios_init(psx->bios);
    
    if (psx_bios_load(psx->bios, bios_path))
        return 1;

    psx_mc1_init(psx->mc1);
    psx_mc2_init(psx->mc2);
    psx_mc3_init(psx->mc3);
    psx_ram_init(psx->ram, psx->mc2, RAM_SIZE_2MB);
    psx_dma_init(psx->dma, psx->bus, psx->ic);

    if (psx_exp1_init(psx->exp1, psx->mc1, exp_path))
        return 2;

    psx_exp2_init(psx->exp2, atcons_tx, NULL);
    psx_ic_init(psx->ic, psx->cpu);
    psx_scratchpad_init(psx->scratchpad);
    psx_gpu_init(psx->gpu, psx->ic);
    psx_spu_init(psx->spu, psx->ic);
    psx_timer_init(psx->timer, psx->ic, psx->gpu);
    psx_cdrom_init(psx->cdrom, psx->ic);
    psx_pad_init(psx->pad, psx->ic);
    psx_mdec_init(psx->mdec);
    psx_cpu_init(psx->cpu, psx->bus);

    /* Lets a host that only has a JNI entry point (no psx_t handle) park save
       state requests. Cleared in psx_destroy(). */
    psx_state_set_machine(psx);

    return 0;
}

int psx_load_expansion(psx_t* psx, const char* path) {
    return psx_exp1_init(psx->exp1, psx->mc1, path);
}

void psx_hard_reset(psx_t* psx) {
    log_fatal("Hard reset not yet implemented");

    exit(1);
}

void psx_soft_reset(psx_t* psx) {
    psx_cpu_init(psx->cpu, psx->bus);

    /* PGXP shadows are derived from GTE/RAM state that just went away. */
    psx_pgxp_reset();

    /* So is every rewind snapshot and the runahead slot: they describe a
       timeline this machine is no longer on, and putting one back after a reset
       would resume a session the user just ended. */
    psx_rewind_reset();
}

uint32_t* psx_take_screenshot(psx_t* psx) {
    log_fatal("Screenshots not yet supported");

    exit(1);
}

int psx_swap_disc(psx_t* psx, const char* path) {
    /* Same reasoning as the soft reset: a snapshot taken against the previous
       disc would be refused by the state loader's disc fingerprint anyway. */
    psx_rewind_reset();

    psx_cdrom_destroy(psx->cdrom);

    psx->cdrom = psx_cdrom_create();

    psx_bus_init_cdrom(psx->bus, psx->cdrom);

    psx_cdrom_init(psx->cdrom, psx->ic);

    return psx_cdrom_open(psx->cdrom, path);
}

void psx_destroy(psx_t* psx) {
    psx_state_set_machine(NULL);

    /* Frees the rewind ring, which is by far the largest thing the core holds
       that is not the machine itself. */
    psx_rewind_shutdown();

    psx_cpu_destroy(psx->cpu);
    psx_bios_destroy(psx->bios);
    psx_bus_destroy(psx->bus);
    psx_ram_destroy(psx->ram);
    psx_exp1_destroy(psx->exp1);
    psx_mc1_destroy(psx->mc1);
    psx_mc2_destroy(psx->mc2);
    psx_mc3_destroy(psx->mc3);
    psx_ic_destroy(psx->ic);
    psx_scratchpad_destroy(psx->scratchpad);
    psx_gpu_destroy(psx->gpu);
    psx_spu_destroy(psx->spu);
    psx_timer_destroy(psx->timer);
    psx_cdrom_destroy(psx->cdrom);
    psx_pad_destroy(psx->pad);
    psx_mdec_destroy(psx->mdec);

    free(psx);
}

psx_bios_t* psx_get_bios(psx_t* psx) {
    return psx->bios;
}

psx_ram_t* psx_get_ram(psx_t* psx) {
    return psx->ram;
}

psx_dma_t* psx_get_dma(psx_t* psx) {
    return psx->dma;
}

psx_exp1_t* psx_get_exp1(psx_t* psx) {
    return psx->exp1;
}

psx_exp2_t* psx_get_exp2(psx_t* psx) {
    return psx->exp2;
}

psx_mc1_t* psx_get_mc1(psx_t* psx) {
    return psx->mc1;
}

psx_mc2_t* psx_get_mc2(psx_t* psx) {
    return psx->mc2;
}

psx_mc3_t* psx_get_mc3(psx_t* psx) {
    return psx->mc3;
}

psx_ic_t* psx_get_ic(psx_t* psx) {
    return psx->ic;
}

psx_scratchpad_t* psx_get_scratchpad(psx_t* psx) {
    return psx->scratchpad;
}

psx_gpu_t* psx_get_gpu(psx_t* psx) {
    return psx->gpu;
}

psx_spu_t* psx_get_spu(psx_t* psx) {
    return psx->spu;
}

psx_bus_t* psx_get_bus(psx_t* psx) {
    return psx->bus;
}

psx_timer_t* psx_get_timer(psx_t* psx) {
    return psx->timer;
}

psx_cdrom_t* psx_get_cdrom(psx_t* psx) {
    return psx->cdrom;
}

psx_pad_t* psx_get_pad(psx_t* psx) {
    return psx->pad;
}

psx_mdec_t* psx_get_mdec(psx_t* psx) {
    return psx->mdec;
}

psx_cpu_t* psx_get_cpu(psx_t* psx) {
    return psx->cpu;
}
