#ifndef CONFIG_H
#define CONFIG_H

#include <stdlib.h>

#include "argparse.h"
#include "toml.h"

typedef struct {
    int use_args;
    int version;
    int help_model;
    int help_region;
    int log_level;
    int quiet;
    int console_source;
    int cpu_engine;
    int scale;
    int vsync_enabled;
#ifdef USE_HARDWARE
    int gpu_backend;      // presentation backend: 0=software,1=sdl-accelerated,2=opengl,3=vulkan
    int gl_driver;        // GLES provider for the opengl backend: 0=system, 1=ANGLE
    // rasterizer: 0=software, 1=hardware/auto (GLES when a GL context exists, CPU
    // otherwise), 2=force the CPU internal-resolution rasterizer, 3=force the GLES one.
    // "hardware" is what the UI writes; 2 and 3 exist so the two can be A/B'd from
    // settings.toml on one device without a UI change.
    int renderer;
    int internal_scale;   // 1..ARMSX_HW_RT_MAX_SCALE; hardware rasterizer only
#endif
    int accurate_mask_bit; // GP0(E6) mask bit; changes output, default on
    int accurate_dither;   // honour GPUSTAT bit 9; changes output, default on
    int accurate_prim_size;// cull polys at hardware's 1023x511, not 2048x1024; default on
    int accurate_tex_modulate; // truncate texture blending like hardware; default on
    int pgxp;              // [video] pgxp: precise-vertex pipeline (psx/pgxp.c), default off
    /* ---- [video] display/video feature set. Every one of these is default OFF/neutral, so a
       settings.toml written before they existed (which is every existing on-device file) must
       parse to exactly these values — that is why each parse site seeds from cfg, never from a
       bare literal. See HW_RENDERER_DESIGN.md §0.5.10. */
    int widescreen_hack;   // [video] widescreen_hack: GTE X-projection scale (psx/cpu.c), default off
    int texture_filter;    // [video] texture_filter: 0=nearest (default), 1=bilinear, 2=xBR-style
    int downsample;        // [video] downsample: 0=off (default), 2..8 box-average factor
    int deinterlace;       // [video] deinterlace: 0=weave (default), 1=bob, 2=adaptive
    int overscan_crop;     // [video] overscan_crop: 0=none (default), 1=small, 2=full
    int display_rotation;  // [video] display_rotation: QUARTER TURNS 0..3 (file holds degrees)
    int line_detect;       // [video] line_detect: 0=disabled (default), 1=quads, 2=basic
    /* [video] texture dumping / replacement (psx/texrep.h). Both default OFF, and the whole
       subsystem allocates nothing until one of them is on. `texture_dir` is the BASE
       directory: dumps land in <dir>/dump, packs are read from <dir>/replacements. Empty
       means "derive it", which main.cpp does from the preferences path; Android pushes a
       per-game path through setPs1TextureOptions instead, because only the app knows the
       disc serial. Not owned — it points either at a literal or at a toml_string_in()
       allocation, exactly like cfg->bios above. */
    int texture_dump;
    int texture_replacements;
    const char* texture_dir;
    int texture_scale_mode;
    int debug_panel;
    int stretch_mode;
    int display_aspect;    // 0=classic,1=square,2=wide16x9,3=custom
    float display_aspect_custom; // width/height, used when display_aspect==3
    int integer_scaling;   // snap the output to a whole multiple of the source
    int upscale_height;    // for wide output (e.g. 480/720/1080/1440/2160)
    int analog_mode_default; // [input] analog_mode_default: pad boots in analog mode (1) or digital (0)
    /* [input] multitap: a Multitap in port 1, so four players share one port
       (psx/input/multitap.c). Default off, and deliberately so — a tap answers
       the pad poll with its own ID, and a game that does not understand one
       reads that as NO controller. */
    int multitap;
    /* [emulation]. Rewind keeps a ring of snapshots and steps backwards through
       it; runahead re-simulates N frames every frame to hide input latency. Both
       are off by default and both are documented in psx/rewind.h — including why
       rewind_seconds x rewind_frequency is a memory figure, not just a duration. */
    int rewind;            // 0 = off; nothing is allocated while it is off
    int rewind_seconds;    // 1..60
    int rewind_frequency;  // 1..8 snapshots per second
    int runahead;          // 0 = off, 1..5 frames
    /* [runtime] frame pacing. One policy in three keys: frame_limit gates the limiter at all,
       speed_percent scales the game's own nominal rate, fps_limit is an optional absolute
       ceiling on top (0 = none), and fast_forward_speed replaces speed_percent while
       fast-forward is engaged (0 = uncapped). */
    int frame_limit;
    int speed_percent;         // 10..1000 % of the game's nominal rate
    int fps_limit;             // absolute cap in fps, 0 = off
    double fast_forward_speed; // multiplier, 0 = uncapped
    /* [runtime] frame_skip. PRESENTATION only: the machine is always stepped to the next
       vblank, so guest timing and the SPU's per-frame sample pull are untouched; what is
       dropped is the framebuffer upload and the present.
         0       off (default)
         1..5    fixed — present one frame, then skip N
         -1      adaptive — skip a present only when the pacer says the previous frame
                 overran its budget, and never more than two in a row */
    int frame_skip;
    /* [runtime] host CPU scheduling levers. Neither changes a single emulated cycle; both act
       on the phone, both are EXPERIMENTAL and both default OFF. Consumed by frontend/perf_hint.c
       — read its header before touching either.
         adpf_clock_hint  report per-frame WORK (not frame time) to Android's performance-hint
                          service so the governor clocks for the deadline. Android 13+, silent
                          no-op below that and on every other platform.
         affinity_mode    0 off (scheduler decides, default) · 1 pin the emulation thread to the
                          detected performance cluster · 2 explicit all-cores · 7 alias of 1. */
    int adpf_clock_hint;
    int affinity_mode;
    /* [audio]. Every one of these is consumed by the SDL audio path in frontend/main.cpp
       (ArmsxSession::queueAudioForFrame / initializeSdl) or by psx/dev/spu.c. */
    int audio_volume;              // 0..200 %
    int audio_ff_volume;           // 0..200 %, used instead while fast-forwarding
    int audio_muted;
    int audio_mute_fast_forward;
    int audio_swap_channels;
    int audio_skip_reverb;
    int audio_buffer_ms;           // 2..100 ms; SDL_AudioSpec::samples is derived from it
    int audio_driver;              // Android SDL backend: 0=default, 1=openslES, 2=aaudio
    int audio_background_playback; // keep emulating + playing with the app off-screen (default 0)
    const char* snap_path;
    const char* settings_path;
    const char* bios;
    const char* bios_search;
    const char* model;
    const char* exe;
    const char* region;
    const char* psxe_version;
    const char* cd_path;
    const char* exp_path;
} psxe_config_t;

psxe_config_t* psxe_cfg_create(void);
void psxe_cfg_init(psxe_config_t*);
void psxe_cfg_load_defaults(psxe_config_t*);
void psxe_cfg_load(psxe_config_t*, int, const char**);
char* psxe_cfg_get_bios_path(psxe_config_t*);
const char* psxe_cfg_get_pref_path(void);
/* Override the preference/data directory before anything reads it. SDL_GetPrefPath()
   resolves through org.libsdl.app.SDLActivity on Android, which does not exist when the
   core is embedded in another Activity (the Jetpack Compose front-end), so the embedded
   host supplies the app's files dir itself. A trailing '/' is added when missing. No-op
   once the path has already been resolved, and no-op for an empty argument. */
void psxe_cfg_set_pref_path(const char* path);
void psxe_cfg_destroy(psxe_config_t*);

#endif
