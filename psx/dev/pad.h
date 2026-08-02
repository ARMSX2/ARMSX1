#ifndef PAD_H
#define PAD_H

#include <stdint.h>

#include "ic.h"
#include "input.h"
#include "mcd.h"

#include "../input/sda.h"
#include "../input/guncon.h"
#include "../input/multitap.h"
#include "../state.h"

#define PSX_PAD_BEGIN 0x1f801040
#define PSX_PAD_SIZE  0x10
#define PSX_PAD_END   0x1f80104f

/*
  0     TX Ready Flag 1   (1=Ready/Started)
  1     RX FIFO Not Empty (0=Empty, 1=Not Empty)
  2     TX Ready Flag 2   (1=Ready/Finished)
  3     RX Parity Error   (0=No, 1=Error; Wrong Parity, when enabled)  (sticky)
  4     Unknown (zero)    (unlike SIO, this isn't RX FIFO Overrun flag)
  5     Unknown (zero)    (for SIO this would be RX Bad Stop Bit)
  6     Unknown (zero)    (for SIO this would be RX Input Level AFTER Stop bit)
  7     /ACK Input Level  (0=High, 1=Low)
  8     Unknown (zero)    (for SIO this would be CTS Input Level)
  9     Interrupt Request (0=None, 1=IRQ7) (See JOY_CTRL.Bit4,10-12)   (sticky)
  10    Unknown (always zero)
  11-31 Baudrate Timer    (21bit timer, decrementing at 33MHz)
*/

#define STAT_TXR1 0x0001
#define STAT_RXNE 0x0002
#define STAT_TXR2 0x0004
#define STAT_RXPE 0x0008
#define STAT_UNK4 0x0010
#define STAT_UNK5 0x0020
#define STAT_UNK6 0x0040
#define STAT_ACKL 0x0080
#define STAT_UNK8 0x0100
#define STAT_IRQ7 0x0200

/*
  0     TX Enable (TXEN)  (0=Disable, 1=Enable)
  1     /JOYn Output      (0=High, 1=Low/Select) (/JOYn as defined in Bit13)
  2     RX Enable (RXEN)  (0=Normal, when /JOYn=Low, 1=Force Enable Once)
  3     Unknown? (read/write-able) (for SIO, this would be TX Output Level)
  4     Acknowledge       (0=No change, 1=Reset JOY_STAT.Bits 3,9)          (W)
  5     Unknown? (read/write-able) (for SIO, this would be RTS Output Level)
  6     Reset             (0=No change, 1=Reset most JOY_registers to zero) (W)
  7     Not used             (always zero) (unlike SIO, no matter of FACTOR)
  8-9   RX Interrupt Mode    (0..3 = IRQ when RX FIFO contains 1,2,4,8 bytes)
  10    TX Interrupt Enable  (0=Disable, 1=Enable) ;when JOY_STAT.0-or-2 ;Ready
  11    RX Interrupt Enable  (0=Disable, 1=Enable) ;when N bytes in RX FIFO
  12    ACK Interrupt Enable (0=Disable, 1=Enable) ;when JOY_STAT.7  ;/ACK=LOW
  13    Desired Slot Number  (0=/JOY1, 1=/JOY2) (set to LOW when Bit1=1)
  14-15 Not used             (always zero)
*/

#define CTRL_TXEN 0x0001
#define CTRL_JOUT 0x0002
#define CTRL_RXEN 0x0004
#define CTRL_UNK3 0x0008
#define CTRL_ACKN 0x0010
#define CTRL_UNK5 0x0020
#define CTRL_REST 0x0040
#define CTRL_NUS7 0x0080
#define CTRL_RXIM 0x0300
#define CTRL_TXIE 0x0400
#define CTRL_RXIE 0x0800
#define CTRL_ACIE 0x1000
#define CTRL_SLOT 0x2000

enum {
    DEST_JOY = 0x01,
    DEST_MCD = 0x81
};

enum {
    PAD_CONTROLLER_SDA,
    PAD_CONTROLLER_MOUSE,
    PAD_CONTROLLER_NAMCO_VOLUME,
    PAD_CONTROLLER_SANKYO_NASUKA,
    PAD_WHEEL_NEGCON,
    PAD_WHEEL_MADCATZ,
    PAD_WHEEL_MADCATZ_MC2
};

/*
    To-do: Design API to interface any type of controller.

    Possible names:
    - psx_im (Input Method)
    - psx_controller
    - psx_input

    Private API should contain a way to get the ID of
    this controller, public API should contain the following
    functions: (WIP)
    - _write(data)
    - _read()
    _ _on_button_press(id)
    - _on_button_release(id)
    - _on_analog_change(id)
*/

typedef struct {
    uint32_t bus_delay;
    uint32_t io_base, io_size;

    psx_ic_t* ic;
    psx_input_t* joy_slot[2];
    psx_mcd_t* mcd_slot[2];

    int enable_once;
    int cycles_until_irq;
    int cycle_counter;
    int dest[2];
    int irq_bit;

    uint16_t mode, ctrl, baud, stat;
} psx_pad_t;

psx_pad_t* psx_pad_create(void);
void psx_pad_init(psx_pad_t*, psx_ic_t*);
uint32_t psx_pad_read32(psx_pad_t*, uint32_t);
uint16_t psx_pad_read16(psx_pad_t*, uint32_t);
uint8_t psx_pad_read8(psx_pad_t*, uint32_t);
void psx_pad_write32(psx_pad_t*, uint32_t, uint32_t);
void psx_pad_write16(psx_pad_t*, uint32_t, uint16_t);
void psx_pad_write8(psx_pad_t*, uint32_t, uint8_t);
void psx_pad_destroy(psx_pad_t*);
void psx_pad_button_press(psx_pad_t*, int, uint32_t);
void psx_pad_button_release(psx_pad_t*, int, uint32_t);
void psx_pad_analog_change(psx_pad_t*, int, uint32_t, uint16_t);
/* The same three, addressed to one of the four PLAYERS behind a multitap in
   `slot`. player 0 is the port itself, so with no tap plugged in these behave
   exactly like the three above and players 1..3 are dropped — which is what
   makes them safe to call unconditionally from a front-end that does not know
   whether a tap is attached. */
void psx_pad_button_press_player(psx_pad_t*, int slot, int player, uint32_t);
void psx_pad_button_release_player(psx_pad_t*, int slot, int player, uint32_t);
void psx_pad_analog_change_player(psx_pad_t*, int slot, int player, uint32_t, uint16_t);
/* How many players the device in `slot` accepts: 4 for a multitap, 1 for a pad,
   0 for an empty slot. Lets a host size its routing without knowing the type. */
int psx_pad_player_count(psx_pad_t*, int slot);
void psx_pad_attach_joy(psx_pad_t*, int, psx_input_t*);
void psx_pad_detach_joy(psx_pad_t*, int);
int psx_pad_attach_mcd(psx_pad_t*, int, const char*);
void psx_pad_detach_mcd(psx_pad_t*, int);
void psx_pad_update(psx_pad_t*, int);
/* JOY_STAT/CTRL/MODE/BAUD, the ACK countdown, and tagged sub-blocks for
   whatever is in the two controller and two memory-card slots. */
void psx_pad_save_state(psx_pad_t*, psx_state_writer_t*);
int psx_pad_load_state(psx_pad_t*, psx_state_reader_t*);

#endif