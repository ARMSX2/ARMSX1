/*
    This file is part of the PSXE Emulator Project

    Namco GunCon emulation
*/

#ifndef GUNCON_H
#define GUNCON_H

#include "../dev/input.h"
#include "../state.h"

/*
  __Halfword 0 (Controller Info)___________________
  0-15  Controller Info  (5A63h=Namco Lightgun; GunCon/Cinch Type)
  __Halfword 1 (Buttons)___________________________
  0-2   Not used              (All bits always 1)
  3     Button A (Left Side)  (0=Pressed, 1=Released) ;aka Joypad Start
  4-12  Not used              (All bits always 1)
  13    Trigger Button        (0=Pressed, 1=Released) ;aka Joypad O-Button
  14    Button B (Right Side) (0=Pressed, 1=Released) ;aka Joypad X-Button
  15    Not used              (All bits always 1)
  __Halfword 2 (X)_________________________________
  0-15  8MHz clks since HSYNC (01h=Error, or 04Dh..1CDh)
  __Halfword 3 (Y)_________________________________
  0-15  Scanlines since VSYNC (05h/0Ah=Error, PAL=20h..127h, NTSC=19h..F8h)
*/

#define GUNCON_IDL              0x63
#define GUNCON_IDH              0x5a

#define PSXI_SW_GUNCON_A        0x00000008
#define PSXI_SW_GUNCON_TRIGGER  0x00002000
#define PSXI_SW_GUNCON_B        0x00004000
#define PSXI_AX_GUNCON_X        0x00010000
#define PSXI_AX_GUNCON_Y        0x00020000
#define PSXI_AX_GUNCON_SX       0x00030000
#define PSXI_AX_GUNCON_SY       0x00040000

enum {
    GUNCON_STATE_TX_HIZ = 0,
    GUNCON_STATE_TX_IDL,
    GUNCON_STATE_TX_IDH,
    GUNCON_STATE_TX_SWL,
    GUNCON_STATE_TX_SWH,
    GUNCON_STATE_TX_XPL,
    GUNCON_STATE_TX_XPH,
    GUNCON_STATE_TX_YPL,
    GUNCON_STATE_TX_YPH
};

typedef struct {
    int state;
    uint16_t sw;
    uint8_t tx_data;
    int tx_data_ready;
    uint16_t x;
    uint16_t y;
    uint16_t sx;
    uint16_t sy;
} psxi_guncon_t;

psxi_guncon_t* psxi_guncon_create(void);
void psxi_guncon_init(psxi_guncon_t*);
void psxi_guncon_init_input(psxi_guncon_t*, psx_input_t*);
/* GunCon serial transfer state machine + latched trigger position. */
void psxi_guncon_save_state(psxi_guncon_t*, psx_state_writer_t*);
int psxi_guncon_load_state(psxi_guncon_t*, psx_state_reader_t*);
void psxi_guncon_destroy(psxi_guncon_t*);

#endif