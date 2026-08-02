#ifndef CPU_H
#define CPU_H

#include <stdint.h>
#include <stdio.h>

#include "bus.h"
#include "state.h"

#define PSX_CPU_CPS 33868800 // 33868800 Clocks/s
#define PSX_CPU_FREQ 33.868800f // 33.868800 MHz

struct psx_cpu_t;
struct psx_cpu_cache_t;

typedef struct psx_cpu_t psx_cpu_t;

typedef enum {
    PSX_CPU_INTERPRETER = 0,
    PSX_CPU_CACHED_INTERPRETER = 1
} psx_cpu_execution_mode_t;

typedef struct {
    uint64_t hits;
    uint64_t misses;
    uint64_t invalidations;
} psx_cpu_cache_stats_t;

typedef void (*psx_cpu_kcall_hook_t)(psx_cpu_t*);

/*
    cop0r0      - N/A
    cop0r1      - N/A
    cop0r2      - N/A
    cop0r3      - BPC - Breakpoint on execute (R/W)
    cop0r4      - N/A
    cop0r5      - BDA - Breakpoint on data access (R/W)
    cop0r6      - JUMPDEST - Randomly memorized jump address (R)
    cop0r7      - DCIC - Breakpoint control (R/W)
    cop0r8      - BadVaddr - Bad Virtual Address (R)
    cop0r9      - BDAM - Data Access breakpoint mask (R/W)
    cop0r10     - N/A
    cop0r11     - BPCM - Execute breakpoint mask (R/W)
    cop0r12     - SR - System status register (R/W)
    cop0r13     - CAUSE - Describes the most recently recognised exception (R)
    cop0r14     - EPC - Return Address from Trap (R)
    cop0r15     - PRID - Processor ID (R)
*/

#define COP0_BPC      3
#define COP0_BDA      5
#define COP0_JUMPDEST 6
#define COP0_DCIC     7
#define COP0_BADVADDR 8
#define COP0_BDAM     9
#define COP0_BPCM     11
#define COP0_SR       12
#define COP0_CAUSE    13
#define COP0_EPC      14
#define COP0_PRID     15

/*
  Name       Alias    Common Usage
  R0         zero     Constant (always 0)
  R1         at       Assembler temporary (destroyed by some assembler pseudoinstructions!)
  R2-R3      v0-v1    Subroutine return values, may be changed by subroutines
  R4-R7      a0-a3    Subroutine arguments, may be changed by subroutines
  R8-R15     t0-t7    Temporaries, may be changed by subroutines
  R16-R23    s0-s7    Static variables, must be saved by subs
  R24-R25    t8-t9    Temporaries, may be changed by subroutines
  R26-R27    k0-k1    Reserved for kernel (destroyed by some IRQ handlers!)
  R28        gp       Global pointer (rarely used)
  R29        sp       Stack pointer
  R30        fp(s8)   Frame Pointer, or 9th Static variable, must be saved
  R31        ra       Return address (used so by JAL,BLTZAL,BGEZAL opcodes)
  -          pc       Program counter
  -          hi,lo    Multiply/divide results, may be changed by subroutines
*/

typedef struct __attribute__((__packed__)) {
    union {
        uint32_t xy;
        int16_t p[2];
    };

    int16_t z;
} gte_vertex_t;

typedef struct __attribute__((__packed__)) {
    union {
        uint32_t xy;
        int16_t p[2];
    };
} gte_vec2_t;

typedef struct __attribute__((__packed__)) {
    int32_t x, y, z;
} gte_vec3_t;

typedef struct __attribute__((__packed__)) {
    union {
        uint32_t rgbc;
        uint8_t c[4];
    };
} gte_color_t;

typedef struct __attribute__((__packed__)) {
    union {
        uint32_t u32;
        int16_t c[2];
    } m[4];

    int16_t m33;
} gte_matrix_t;

struct __attribute__((__packed__)) psx_cpu_t {
    uint32_t r[32];
    uint32_t opcode;
    uint32_t pc, next_pc, saved_pc;
    uint32_t hi, lo;
    uint32_t load_d, load_v;
    uint32_t last_cycles;
    uint32_t total_cycles;
    int branch, delay_slot, branch_taken;

    uint32_t cop0_r[16];

    struct {
        gte_vertex_t v[3];
        gte_color_t rgbc;
        uint16_t otz;
        int16_t ir[4];
        gte_vec2_t sxy[4];
        uint16_t sz[4];
        gte_color_t rgb[3];
        uint32_t res1;
        int32_t mac[4];
        uint16_t irgb, orgb;
        int32_t lzcs, lzcr;
    } cop2_dr;

    struct {
        gte_matrix_t rt;
        gte_vec3_t tr;
        gte_matrix_t l;
        gte_vec3_t bk;
        gte_matrix_t lr;
        gte_vec3_t fc;
        uint32_t ofx, ofy;
        uint32_t h;
        int16_t dqa;
        int32_t dqb;
        int16_t zsf3, zsf4;
        uint32_t flag;
    } cop2_cr;

    int gte_lm;
    int gte_sf;
    int gte_mx;
    int gte_v;
    int gte_cv;
    int64_t s_mac0;
    int64_t s_mac3;

    psx_bus_t* bus;

    psx_cpu_kcall_hook_t a_function_hook;
    psx_cpu_kcall_hook_t b_function_hook;
    psx_cpu_execution_mode_t execution_mode;
    struct psx_cpu_cache_t* cache;
};

/*
  0     IEc Current Interrupt Enable  (0=Disable, 1=Enable) ;rfe pops IUp here
  1     KUc Current Kernel/User Mode  (0=Kernel, 1=User)    ;rfe pops KUp here
  2     IEp Previous Interrupt Disable                      ;rfe pops IUo here
  3     KUp Previous Kernel/User Mode                       ;rfe pops KUo here
  4     IEo Old Interrupt Disable                       ;left unchanged by rfe
  5     KUo Old Kernel/User Mode                        ;left unchanged by rfe
  6-7   -   Not used (zero)
  8-15  Im  8 bit interrupt mask fields. When set the corresponding
            interrupts are allowed to cause an exception.
  16    Isc Isolate Cache (0=No, 1=Isolate)
              When isolated, all load and store operations are targetted
              to the Data cache, and never the main memory.
              (Used by PSX Kernel, in combination with Port FFFE0130h)
  17    Swc Swapped cache mode (0=Normal, 1=Swapped)
              Instruction cache will act as Data cache and vice versa.
              Use only with Isc to access & invalidate Instr. cache entries.
              (Not used by PSX Kernel)
  18    PZ  When set cache parity bits are written as 0.
  19    CM  Shows the result of the last load operation with the D-cache
            isolated. It gets set if the cache really contained data
            for the addressed memory location.
  20    PE  Cache parity error (Does not cause exception)
  21    TS  TLB shutdown. Gets set if a programm address simultaneously
            matches 2 TLB entries.
            (initial value on reset allows to detect extended CPU version?)
  22    BEV Boot exception vectors in RAM/ROM (0=RAM/KSEG0, 1=ROM/KSEG1)
  23-24 -   Not used (zero)
  25    RE  Reverse endianness   (0=Normal endianness, 1=Reverse endianness)
              Reverses the byte order in which data is stored in
              memory. (lo-hi -> hi-lo)
              (Affects only user mode, not kernel mode) (?)
              (The bit doesn't exist in PSX ?)
  26-27 -   Not used (zero)
  28    CU0 COP0 Enable (0=Enable only in Kernel Mode, 1=Kernel and User Mode)
  29    CU1 COP1 Enable (0=Disable, 1=Enable) (none in PSX)
  30    CU2 COP2 Enable (0=Disable, 1=Enable) (GTE in PSX)
  31    CU3 COP3 Enable (0=Disable, 1=Enable) (none in PSX)
*/

#define SR_IEC 0x00000001
#define SR_KUC 0x00000002
#define SR_IEP 0x00000004
#define SR_KUP 0x00000008
#define SR_IEO 0x00000010
#define SR_KUO 0x00000020
#define SR_IM  0x0000ff00
#define SR_IM0 0x00000100
#define SR_IM1 0x00000200
#define SR_IM2 0x00000400
#define SR_IM3 0x00000800
#define SR_IM4 0x00001000
#define SR_IM5 0x00002000
#define SR_IM6 0x00004000
#define SR_IM7 0x00008000
#define SR_ISC 0x00010000
#define SR_SWC 0x00020000
#define SR_PZ  0x00040000
#define SR_CM  0x00080000
#define SR_PE  0x00100000
#define SR_TS  0x00200000
#define SR_BEV 0x00400000
#define SR_RE  0x02000000
#define SR_CU0 0x10000000
#define SR_CU1 0x20000000
#define SR_CU2 0x40000000
#define SR_CU3 0x80000000

psx_cpu_t* psx_cpu_create(void);
void psx_cpu_init(psx_cpu_t*, psx_bus_t*);
void psx_cpu_destroy(psx_cpu_t*);
void psx_cpu_cycle(psx_cpu_t*);
void psx_cpu_set_irq_pending(psx_cpu_t*);
/* Save state. Everything up to (not including) psx_cpu_t::bus is machine
   state; the bus pointer, the kcall hooks, the execution mode and the
   cached-interpreter block cache are host/derived and are preserved in place
   across a load. */
void psx_cpu_save_state(psx_cpu_t*, psx_state_writer_t*);
int psx_cpu_load_state(psx_cpu_t*, psx_state_reader_t*);
void psx_cpu_fetch(psx_cpu_t*);
void psx_cpu_set_a_kcall_hook(psx_cpu_t*, psx_cpu_kcall_hook_t);
void psx_cpu_set_b_kcall_hook(psx_cpu_t*, psx_cpu_kcall_hook_t);
void psx_cpu_set_execution_mode(psx_cpu_t*, psx_cpu_execution_mode_t);
psx_cpu_execution_mode_t psx_cpu_get_execution_mode(const psx_cpu_t*);
void psx_cpu_invalidate_cache(psx_cpu_t*);
void psx_cpu_invalidate_range(psx_cpu_t*, uint32_t, uint32_t);
psx_cpu_cache_stats_t psx_cpu_get_cache_stats(const psx_cpu_t*);
int psx_cpu_execute(psx_cpu_t*);

/*
    Widescreen hack ([video] widescreen_hack).

    The GTE projects with OFX + IR1*(H/SZ3): OFX is the screen centre, so scaling ONLY the
    IR1 product shrinks projected X about that centre without moving it. Shrinking by
    (4/3)/(16/9) = 3/4 and then presenting the result stretched to 16:9 puts every 3D
    proportion back where it started and widens the visible field instead — which is the
    whole point. It is a HACK, not emulation: 2D and UI elements are not projected through
    the GTE at all, so they still stretch with the display aspect.

    Process-global, matching psx/pgxp.c's reasoning (one machine per process in every
    frontend here) and cheap enough to test per RTPS/RTPT: one predictable branch on a
    global that is 0 in the shipping default. Read directly by the GTE macros in cpu.c.
*/
extern int g_psx_widescreen_hack;

#define psx_widescreen_active() (g_psx_widescreen_hack)

void psx_cpu_set_widescreen_hack(int enabled);
int  psx_cpu_widescreen_hack(void);

/*
    00h INT     Interrupt
    01h MOD     TLB modification (none such in PSX)
    02h TLBL    TLB load         (none such in PSX)
    03h TLBS    TLB store        (none such in PSX)
    04h AdEL    Address error, Data load or Instruction fetch
    05h AdES    Address error, Data store
                The address errors occur when attempting to read
                outside of KUseg in user mode and when the address
                is misaligned. (See also: BadVaddr register)
    06h IBE     Bus error on Instruction fetch
    07h DBE     Bus error on Data load/store
    08h Syscall Generated unconditionally by syscall instruction
    09h BP      Breakpoint - break instruction
    0Ah RI      Reserved instruction
    0Bh CpU     Coprocessor unusable
    0Ch Ov      Arithmetic overflow
*/

#define CAUSE_INT       (0x00 << 2)
#define CAUSE_MOD       (0x01 << 2)
#define CAUSE_TLBL      (0x02 << 2)
#define CAUSE_TLBS      (0x03 << 2)
#define CAUSE_ADEL      (0x04 << 2)
#define CAUSE_ADES      (0x05 << 2)
#define CAUSE_IBE       (0x06 << 2)
#define CAUSE_DBE       (0x07 << 2)
#define CAUSE_SYSCALL   (0x08 << 2)
#define CAUSE_BP        (0x09 << 2)
#define CAUSE_RI        (0x0a << 2)
#define CAUSE_CPU       (0x0b << 2)
#define CAUSE_OV        (0x0c << 2)

/*
  31   Error Flag (Bit30..23, and 18..13 ORed together) (Read only)
  30   MAC1 Result positive 44bit overflow (max +7FFFFFFFFFFh) ;\triggered
  29   MAC2 Result positive 44bit overflow (max +7FFFFFFFFFFh) ; during
  28   MAC3 Result positive 44bit overflow (max +7FFFFFFFFFFh) ; calculations
  27   MAC1 Result negative 44bit overflow (min -80000000000h) ;
  26   MAC2 Result negative 44bit overflow (min -80000000000h) ;
  25   MAC3 Result negative 44bit overflow (min -80000000000h) ;/
  24   IR1 saturated to +0000h..+7FFFh (lm=1) or to -8000h..+7FFFh (lm=0)
  23   IR2 saturated to +0000h..+7FFFh (lm=1) or to -8000h..+7FFFh (lm=0)
  22   IR3 saturated to +0000h..+7FFFh (lm=1) or to -8000h..+7FFFh (lm=0)
  21   Color-FIFO-R saturated to +00h..+FFh
  20   Color-FIFO-G saturated to +00h..+FFh
  19   Color-FIFO-B saturated to +00h..+FFh
  18   SZ3 or OTZ saturated to +0000h..+FFFFh
  17   Divide overflow. RTPS/RTPT division result saturated to max=1FFFFh
  16   MAC0 Result positive 32bit overflow (max +7FFFFFFFh)    ;\triggered on
  15   MAC0 Result negative 32bit overflow (min -80000000h)    ;/final result
  14   SX2 saturated to -0400h..+03FFh
  13   SY2 saturated to -0400h..+03FFh
*/

#define GTEF_SY2SAT 0x00002000
#define GTEF_SX2SAT 0x00004000
#define GTEF_M0POVF 0x00008000
#define GTEF_M0NOVF 0x00010000
#define GTEF_DIVOVF 0x00020000
#define GTEF_SZ3SAT 0x00040000
#define GTEF_CFRSAT 0x00080000
#define GTEF_CFGSAT 0x00100000
#define GTEF_CFBSAT 0x00200000
#define GTEF_IR3SAT 0x00400000
#define GTEF_IR2SAT 0x00800000
#define GTEF_IR1SAT 0x01000000
#define GTEF_M3NOVF 0x02000000
#define GTEF_M2NOVF 0x04000000
#define GTEF_M1NOVF 0x08000000
#define GTEF_M3POVF 0x10000000
#define GTEF_M2POVF 0x20000000
#define GTEF_M1POVF 0x40000000
#define GTEF_ERRORF 0x80000000

#endif
