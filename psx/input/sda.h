/*
    This file is part of the PSXE Emulator Project

    Sony PlayStation Standard Digital/Analog Controller Emulator
*/

#ifndef SDA_H
#define SDA_H

#include "../dev/input.h"
#include "../state.h"

// Controller/Input IDs
#define SDA_MODEL_DIGITAL       0x41
#define SDA_MODEL_ANALOG_PAD    0x73
#define SDA_MODEL_ANALOG_STICK  0x53

#define PSXI_SW_SDA_SELECT      0x00000001
#define PSXI_SW_SDA_L3          0x00000002
#define PSXI_SW_SDA_R3          0x00000004
#define PSXI_SW_SDA_START       0x00000008
#define PSXI_SW_SDA_PAD_UP      0x00000010
#define PSXI_SW_SDA_PAD_RIGHT   0x00000020
#define PSXI_SW_SDA_PAD_DOWN    0x00000040
#define PSXI_SW_SDA_PAD_LEFT    0x00000080
#define PSXI_SW_SDA_L2          0x00000100
#define PSXI_SW_SDA_R2          0x00000200
#define PSXI_SW_SDA_L1          0x00000400
#define PSXI_SW_SDA_R1          0x00000800
#define PSXI_SW_SDA_TRIANGLE    0x00001000
#define PSXI_SW_SDA_CIRCLE      0x00002000
#define PSXI_SW_SDA_CROSS       0x00004000
#define PSXI_SW_SDA_SQUARE      0x00008000
#define PSXI_SW_SDA_ANALOG      0x00010000
#define PSXI_AX_SDA_RIGHT_HORZ  0x00020000
#define PSXI_AX_SDA_RIGHT_VERT  0x00030000
#define PSXI_AX_SDA_LEFT_HORZ   0x00040000
#define PSXI_AX_SDA_LEFT_VERT   0x00050000

enum {
    SDA_STATE_TX_HIZ = 0,
    SDA_STATE_TX_IDL,
    SDA_STATE_TX_IDH,
    SDA_STATE_TX_SWL,
    SDA_STATE_TX_SWH,
    SDA_STATE_TX_ADC0,
    SDA_STATE_TX_ADC1,
    SDA_STATE_TX_ADC2,
    SDA_STATE_TX_ADC3
};

enum {
    SA_MODE_DIGITAL = 0,
    SA_MODE_ANALOG
};

typedef struct {
    uint8_t prev_model;
    uint8_t model;
    int state;
    int sa_mode;
    uint16_t sw;
    uint8_t tx_data;
    int tx_data_ready;
    uint8_t adc0;
    uint8_t adc1;
    uint8_t adc2;
    uint8_t adc3;
} psxi_sda_t;

psxi_sda_t* psxi_sda_create(void);
void psxi_sda_init(psxi_sda_t*, uint16_t);
/* Set (not toggle) the analog/digital mode, so a front-end can decide which mode the pad
   reports when a game boots. Same state change the pad's ANALOG button performs, minus the
   flip — the button keeps working afterwards. Call after psxi_sda_init(). */
void psxi_sda_set_analog_mode(psxi_sda_t*, int enabled);
void psxi_sda_init_input(psxi_sda_t*, psx_input_t*);
/* The vtable entry points, declared so a container device can drive a pad it
   owns directly. The multitap (psx/input/multitap.c) holds four of these and has
   to address them by index, which the void*-udata vtable cannot express. Same
   signatures as the psx_input_t function pointers: the first argument is the
   psxi_sda_t*. */
void psxi_sda_on_button_press(void*, uint32_t);
void psxi_sda_on_button_release(void*, uint32_t);
void psxi_sda_on_analog_change(void*, uint32_t, uint16_t);
/* The pad's serial transfer state machine, model/mode latches, button word and
   the four ADC bytes. Saved as part of the PAD section so a state taken
   mid-poll resumes mid-poll instead of feeding the game a garbage byte. */
void psxi_sda_save_state(psxi_sda_t*, psx_state_writer_t*);
int psxi_sda_load_state(psxi_sda_t*, psx_state_reader_t*);
void psxi_sda_destroy(psxi_sda_t*);

#endif