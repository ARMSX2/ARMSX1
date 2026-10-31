#ifndef INPUT_H
#define INPUT_H

#include <stdint.h>

struct psx_input_t;

typedef struct psx_input_t psx_input_t;

typedef void (*psx_input_write_t)(void*, uint16_t);
typedef uint32_t (*psx_input_read_t)(void*);
typedef void (*psx_input_on_button_press_t)(void*, uint32_t);
typedef void (*psx_input_on_button_release_t)(void*, uint32_t);
typedef void (*psx_input_on_analog_change_t)(void*, uint32_t, uint16_t);
typedef int (*psx_input_query_fifo_t)(void*);

/* Identifies what psx_input_t::udata actually points at. Set by the concrete
   controller's *_init_input(). Without it a save state would have to guess the
   type of an opaque void*, which is exactly the kind of type confusion that
   turns a bad state into a crash. */
enum {
    PSX_INPUT_KIND_NONE = 0,
    PSX_INPUT_KIND_SDA = 1,
    PSX_INPUT_KIND_GUNCON = 2,
    /* A multitap: four sub-controllers behind one port (psx/input/multitap.c).
       Appended, never inserted — the value is written into save states, so a new
       kind must not renumber the existing ones. A state taken with a tap plugged
       in refuses to load into a machine without one (and vice versa) through the
       existing kind check in pad_load_joy(); no existing payload changed shape,
       so PSX_STATE_CORE_ABI does NOT move and every state already on a device
       still loads. */
    PSX_INPUT_KIND_MULTITAP = 3
};

struct psx_input_t {
    void* udata;
    uint32_t kind;

    psx_input_write_t write_func;
    psx_input_read_t read_func;
    psx_input_on_button_press_t on_button_press_func;
    psx_input_on_button_release_t on_button_release_func;
    psx_input_on_analog_change_t on_analog_change_func;
    psx_input_query_fifo_t query_fifo_func;
};

psx_input_t* psx_input_create(void);
void psx_input_init(psx_input_t*);
void psx_input_set_write_func(psx_input_t*, psx_input_write_t);
void psx_input_set_read_func(psx_input_t*, psx_input_read_t);
void psx_input_set_on_button_press_func(psx_input_t*, psx_input_on_button_press_t);
void psx_input_set_on_button_release_func(psx_input_t*, psx_input_on_button_release_t);
void psx_input_set_on_analog_change_func(psx_input_t*, psx_input_on_analog_change_t);
void psx_input_set_query_fifo_func(psx_input_t*, psx_input_query_fifo_t);
void psx_input_destroy(psx_input_t*);

#endif