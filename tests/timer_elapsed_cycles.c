#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "../psx/dev/timer.h"

static unsigned interrupts;
void psx_ic_irq(psx_ic_t* ic, int irq) { (void)ic; interrupts |= irq; }

static void init(psx_timer_t* timer, psx_gpu_t* gpu) {
    psx_timer_init(timer, NULL, gpu);
    gpu->udata[1] = timer;
    for (int i = 0; i < 3; ++i)
        psx_timer_write16(timer, i * 16 + 8, 65535);
}

int main(void) {
    static psx_gpu_t gpu;
    psx_timer_t timer;
    init(&timer, &gpu);
    // Zero elapsed time must not advance any system-clock timer.
    psx_timer_update(&timer, 0);
    for (int i = 0; i < 3; ++i) assert(timer.timer[i].counter == 0);
    const int cycles[] = {2, 15, 1, 23, 8, 44, 3};
    unsigned elapsed = 0;
    for (unsigned i = 0; i < sizeof(cycles)/sizeof(cycles[0]); ++i) {
        elapsed += cycles[i];
        psx_timer_update(&timer, cycles[i]);
        for (int j = 0; j < 3; ++j) assert(timer.timer[j].counter == elapsed);
    }
    // Different instruction counts with the same elapsed time must agree.
    init(&timer, &gpu);
    psx_timer_update(&timer, elapsed);
    for (int i = 0; i < 3; ++i) assert(timer.timer[i].counter == elapsed);

    init(&timer, &gpu);
    psx_timer_write16(&timer, 0x24, 2 << 8); // timer 2 system clock / 8
    psx_timer_update(&timer, 7);
    psx_timer_update(&timer, 17);
    assert(timer.timer[2].counter == 3);

    init(&timer, &gpu);
    psx_timer_write16(&timer, 0x14, 1 << 8); // timer 1 HBlank clock
    psx_timer_update(&timer, 23);
    assert(timer.timer[1].counter == 0);
    psxe_gpu_hblank_event_cb(&gpu);
    assert(timer.timer[1].counter == 1);
    psxe_gpu_hblank_end_event_cb(&gpu);
    psx_timer_update(&timer, 15);
    assert(timer.timer[1].counter == 1);

    init(&timer, &gpu);
    psx_timer_write16(&timer, 0x24, 1); // timer 2 stopped by sync mode 0
    psx_timer_update(&timer, 44);
    assert(timer.timer[2].counter == 0);

    init(&timer, &gpu);
    psx_timer_write16(&timer, 0x04, 1 << 8); // timer 0 dot clock
    psx_timer_update(&timer, 70);
    assert(fabsf(timer.timer[0].counter - 11.0f) < 0.0001f);

    init(&timer, &gpu);
    psx_timer_write16(&timer, 0x28, 100);
    psx_timer_write16(&timer, 0x24, 1 << 4); // target interrupt
    psx_timer_update(&timer, 80);
    assert(interrupts == 0);
    psx_timer_update(&timer, 23);
    assert(interrupts == (16 << 2));
    puts("Timer elapsed-cycle, divider, HBlank, pause and IRQ checks passed");
    return 0;
}
