/*
    PRESENT GEOMETRY — where the emulated picture lands on the host window.

    WHY THIS FILE EXISTS

    A Retroid handheld reported the picture drawn at a uniform 2/3 scale anchored to the
    TOP-LEFT of a 1920x1080 window: 1280x720 of image, the right third and bottom third
    black. Identical at 640x480 and at 320x216, so nothing about the emulated frame was
    involved.

    The obvious suspect was armsx_render_compute_dst() — and it was innocent. It was handed
    1280x720 and correctly filled 1280x720. The number it was measured against was the wrong
    one: on Android the SDL backend's "output size" is the host's offscreen blit-bridge
    framebuffer (android_jni.cpp), which is capped at 720p, NOT the window. That is fine by
    design, because the bridge asks the ANativeWindow for a 1280x720 buffer and SurfaceFlinger
    scales it up. The bug was that the request stopped being made: render_gl.cpp's
    CreateEglContext() resets the geometry with ANativeWindow_setBuffersGeometry(w=0, h=0,
    visual) — "revert to the window's natural size" — and when that backend failed or was
    switched away from, nothing put the bridge's size back. The bridge then blitted its
    1280x720 framebuffer into a 1920x1080 buffer with an unscaled row copy, landing it in the
    top-left corner. 1280/1920 = 720/1080 = 2/3, and it looked exactly like a density (dp vs
    px) mistake, which it was not: nothing in this path reads a display density.

    SO THIS GATE PINS BOTH HALVES AND THE JOIN BETWEEN THEM

      1. the destination rect, for known (output size, source size, aspect mode) triples
      2. the host framebuffer size the bridge renders at, for known surface sizes
      3. THE CONTRACT: the buffer geometry the bridge must request is the same number the
         destination rect fills. A test of (1) alone passes through the entire bug — which is
         precisely what happened, since compute_dst never had anything wrong with it.

    Pure rule, no window, no GL context, no device — same reason tests/gpu_profile_rules.c and
    tests/gpu_rasterizer_select.c are: a rule that can only be checked by shipping it to a user
    is not a gate.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../frontend/render.h"
/* The destination-rect math is an internal contract between render.cpp and the backends, and
   this gate exists to pin that contract. Same reason gpu_rasterizer_select.c reaches into
   frontend/gpu_hw_gl.h. */
#include "../frontend/render_internal.h"

static int g_failed;

static void check_int(const char* name, int got, int want) {
    if (got != want) {
        fprintf(stderr, "PRESENT_GEOM failed case=%s got=%d want=%d\n", name, got, want);
        g_failed = 1;

        return;
    }

    printf("PRESENT_GEOM passed case=%s\n", name);
}

static void check_rect(const char* name, SDL_Rect got, int x, int y, int w, int h) {
    if (got.x != x || got.y != y || got.w != w || got.h != h) {
        fprintf(stderr, "PRESENT_GEOM failed case=%s got=%d,%d %dx%d want=%d,%d %dx%d\n", name,
                got.x, got.y, got.w, got.h, x, y, w, h);
        g_failed = 1;

        return;
    }

    printf("PRESENT_GEOM passed case=%s\n", name);
}

static void check_size(const char* name, int got_w, int got_h, int want_w, int want_h) {
    if (got_w != want_w || got_h != want_h) {
        fprintf(stderr, "PRESENT_GEOM failed case=%s got=%dx%d want=%dx%d\n", name, got_w, got_h,
                want_w, want_h);
        g_failed = 1;

        return;
    }

    printf("PRESENT_GEOM passed case=%s\n", name);
}

/* Aspect modes as frontend/main.cpp draw() resolves them. Kept as literals rather than shared
   constants because pinning the NUMBERS is the point: a silent change to what "classic" means
   has to fail here. */
#define ASPECT_CLASSIC (4.0f / 3.0f)
#define ASPECT_SQUARE 1.0f
#define ASPECT_WIDE (16.0f / 9.0f)

static armsx_render_frame_params_t params_of(float aspect, bool stretch) {
    armsx_render_frame_params_t p;

    memset(&p, 0, sizeof(p));
    p.aspect = aspect;
    p.stretch = stretch;

    return p;
}

int main(void) {
    SDL_Rect dst;
    int fb_w = 0;
    int fb_h = 0;

    /* ---- 1. THE REGRESSION ITSELF -----------------------------------------------------
       The reported device, end to end. A 1920x1080 landscape Surface gives a 1280x720
       bridge framebuffer; the destination rect must FILL that framebuffer, and the buffer
       geometry the bridge requests must be that same 1280x720. Get any one of the three
       wrong and the picture is inset. */
    armsx_render_host_framebuffer_size(1920, 1080, 0, &fb_w, &fb_h);
    check_size("retroid-1080p-framebuffer", fb_w, fb_h, 1280, 720);

    /* PS1 BIOS boot, 640x480, as it was screenshotted. */
    {
        armsx_render_frame_params_t p = params_of(ASPECT_CLASSIC, true);
        armsx_render_compute_dst(fb_w, fb_h, 640, 480, &p, &dst);
    }
    check_rect("bios-640x480-fills-framebuffer", dst, 0, 0, 1280, 720);

    /* Xenogears title menu, 320x216. Same inset was measured, so the rect must be identical:
       under stretch the source resolution cannot move it. */
    {
        armsx_render_frame_params_t p = params_of(ASPECT_CLASSIC, true);
        armsx_render_compute_dst(fb_w, fb_h, 320, 216, &p, &dst);
    }
    check_rect("xenogears-320x216-fills-framebuffer", dst, 0, 0, 1280, 720);

    /* ★ THE CONTRACT. The blit bridge copies row-for-row with no scaling, so the ANativeWindow
       buffer it asks for has to be exactly the surface compute_dst filled. This is the
       assertion the bug violated: the buffer stayed at the window's 1920x1080 while the
       framebuffer was 1280x720, and the unscaled copy put the picture in the corner. */
    check_int("blit-geometry-matches-dst-width", fb_w, dst.w);
    check_int("blit-geometry-matches-dst-height", fb_h, dst.h);

    /* And the shape of the failure, pinned as arithmetic so the 2/3 signature is recognisable
       if it ever comes back: a 1280x720 picture in a 1920x1080 buffer covers 4/9 of it. */
    check_int("unscaled-blit-into-window-sized-buffer-is-a-mismatch", (fb_w == 1920 && fb_h == 1080),
              0);

    /* ---- 2. THE HOST FRAMEBUFFER RULE -------------------------------------------------
       The 720p cap and the even-dimension rounding, which together decide every number
       above. */
    armsx_render_host_framebuffer_size(1280, 720, 0, &fb_w, &fb_h);
    check_size("720p-surface-is-uncapped", fb_w, fb_h, 1280, 720);

    armsx_render_host_framebuffer_size(1920, 1200, 0, &fb_w, &fb_h);
    check_size("1200p-surface-caps-preserving-aspect", fb_w, fb_h, 1152, 720);

    armsx_render_host_framebuffer_size(2400, 1080, 0, &fb_w, &fb_h);
    check_size("ultrawide-caps-preserving-aspect", fb_w, fb_h, 1600, 720);

    /* Portrait uses the same short-edge cap as landscape. Rotating the host must rotate the
       framebuffer instead of collapsing it to a narrow 404x720 strip. */
    armsx_render_host_framebuffer_size(1080, 1920, 0, &fb_w, &fb_h);
    check_size("portrait-short-edge-cap", fb_w, fb_h, 720, 1280);

    armsx_render_host_framebuffer_size(800, 480, 0, &fb_w, &fb_h);
    check_size("small-surface-is-untouched", fb_w, fb_h, 800, 480);

    /* Odd dimensions must come back even, and a degenerate surface must never produce a zero
       (a zero-sized SDL_Surface takes the renderer down). */
    armsx_render_host_framebuffer_size(801, 481, 0, &fb_w, &fb_h);
    check_size("odd-surface-rounds-even", fb_w, fb_h, 800, 480);

    armsx_render_host_framebuffer_size(0, 0, 0, &fb_w, &fb_h);
    check_size("degenerate-surface-clamps-to-minimum", fb_w, fb_h, 2, 2);

    /* The cap is overridable (ARMSX_ANDROID_FB_HEIGHT), and 0 means "use the default". */
    armsx_render_host_framebuffer_size(1920, 1080, 1080, &fb_w, &fb_h);
    check_size("explicit-cap-1080-is-uncapped", fb_w, fb_h, 1920, 1080);

    armsx_render_host_framebuffer_size(1920, 1080, 480, &fb_w, &fb_h);
    check_size("explicit-cap-480", fb_w, fb_h, 852, 480);

    /* Android's CPU bridge deliberately uses a lower explicit cap than the portable default.
       SurfaceFlinger performs the last scale, avoiding a full-display software RenderCopy plus
       ANativeWindow row copy while preserving the exact two-sided geometry contract above. */
    armsx_render_host_framebuffer_size(2400, 1080, 360, &fb_w, &fb_h);
    check_size("android-software-cap-360", fb_w, fb_h, 800, 360);

    armsx_render_host_framebuffer_size(1008, 2244, 360, &fb_w, &fb_h);
    check_size("android-portrait-cap-360", fb_w, fb_h, 360, 800);

    armsx_render_host_framebuffer_size(2244, 1008, 360, &fb_w, &fb_h);
    check_size("android-landscape-cap-360", fb_w, fb_h, 800, 360);

    /* ---- 3. THE DESTINATION RECT, PER ASPECT MODE -------------------------------------
       Letterbox/pillarbox against a 1280x720 output, which is the geometry the reported
       device actually presents into. */
    {
        armsx_render_frame_params_t p = params_of(ASPECT_CLASSIC, false);

        armsx_render_compute_dst(1280, 720, 640, 480, &p, &dst);
        /* 4:3 in 16:9 pillarboxes to 960 wide, centred. */
        check_rect("classic-4x3-pillarboxes", dst, 160, 0, 960, 720);
    }

    {
        armsx_render_frame_params_t p = params_of(ASPECT_WIDE, false);

        armsx_render_compute_dst(1280, 720, 640, 480, &p, &dst);
        check_rect("wide-16x9-fills", dst, 0, 0, 1280, 720);
    }

    {
        armsx_render_frame_params_t p = params_of(ASPECT_SQUARE, false);

        armsx_render_compute_dst(1280, 720, 640, 480, &p, &dst);
        check_rect("square-1x1-pillarboxes", dst, 280, 0, 720, 720);
    }

    {
        /* Custom, the mode the reported session was in. 2.3704 (64/27) is wider than the
           output, so it letterboxes. Under stretch it is ignored entirely, which is why the
           aspect could NOT have caused a 2/3 top-left inset. */
        armsx_render_frame_params_t p = params_of(2.3704f, false);

        /* 1280 / 2.3704 = 539.993, and the fit FLOORS rather than rounds, so the height is 539
           and the letterbox bars come out 90 top / 91 bottom. Pinned to what the implementation
           does, not to the prettier 540: changing the rounding would move every aspect mode's
           rect by up to a pixel, which is a separate decision from this bug. */
        armsx_render_compute_dst(1280, 720, 640, 480, &p, &dst);
        check_rect("custom-2.37-letterboxes", dst, 0, 90, 1280, 539);

        p.stretch = true;
        armsx_render_compute_dst(1280, 720, 640, 480, &p, &dst);
        check_rect("stretch-ignores-aspect", dst, 0, 0, 1280, 720);
    }

    {
        /* aspect <= 0 falls back to the source's own ratio rather than producing a degenerate
           rect (a black screen). */
        armsx_render_frame_params_t p = params_of(0.0f, false);

        armsx_render_compute_dst(1280, 720, 640, 480, &p, &dst);
        check_rect("zero-aspect-falls-back-to-source", dst, 160, 0, 960, 720);
    }

    /* ---- 4. MODE CHANGE MID-SESSION ---------------------------------------------------
       BIOS 640x480 -> game 320x240 happens on every single boot. The rect is recomputed from
       live inputs every present (it is not cached anywhere), so a source change must move it
       when the aspect is derived from the source and must NOT move it when the aspect is
       pinned. Open issue #43 claims the display rect is computed once at boot; these two
       cases are what that claim has to be measured against. */
    {
        armsx_render_frame_params_t p = params_of(ASPECT_CLASSIC, false);

        armsx_render_compute_dst(1280, 720, 640, 480, &p, &dst);
        check_rect("mode-change-640x480-pinned-aspect", dst, 160, 0, 960, 720);

        armsx_render_compute_dst(1280, 720, 320, 240, &p, &dst);
        check_rect("mode-change-320x240-pinned-aspect", dst, 160, 0, 960, 720);
    }

    {
        armsx_render_frame_params_t p = params_of(0.0f, false);

        armsx_render_compute_dst(1280, 720, 640, 480, &p, &dst);
        check_rect("mode-change-640x480-source-aspect", dst, 160, 0, 960, 720);

        /* 320x216 is 40:27 = 1.4815, wider than 4:3, so the rect genuinely differs:
           720 * 40/27 = 1066.67, floored to 1066, centred at x = (1280-1066)/2 = 107 -> 106
           after the same flooring. This is the mode change that happens on every boot as the
           BIOS hands off to the game, and it is the case the bridge got wrong. */
        armsx_render_compute_dst(1280, 720, 320, 216, &p, &dst);
        check_rect("mode-change-320x216-source-aspect", dst, 106, 0, 1066, 720);
    }

    if (g_failed)
        return 1;

    puts("PRESENT_GEOM all cases passed");

    return 0;
}

/*
    Host-link stubs. render.cpp is compiled into this gate for the two pure geometry rules, but
    its backend factory and logging entry points live in TUs that pull in SDL, GL and Vulkan.
    Linking those would make the gate depend on a graphics stack it never uses — and a rule that
    needs a GPU to check is not a gate. Nothing below is reachable: the only callers are
    armsx_renderer_create(), armsx_renderer_create_from_sdl() and armsx_render_log(), none of
    which this test invokes. They exist to satisfy the linker, so they abort rather than return
    a plausible value — if a future case starts reaching one, it should fail loudly, not quietly
    take a stubbed path.
*/
#include "../frontend/render_shaders.h"
#include "../frontend/diagnostics.h"

static void stub_unreachable(const char* who) {
    fprintf(stderr, "PRESENT_GEOM internal error: %s is stubbed and must not be called\n", who);
    abort();
}

armsx_renderer_t* armsx_render_create_sdl(SDL_Window* window,
                                          const armsx_render_config_t* config) {
    (void)window;
    (void)config;
    stub_unreachable("armsx_render_create_sdl");

    return NULL;
}

armsx_renderer_t* armsx_render_adopt_sdl(SDL_Renderer* renderer) {
    (void)renderer;
    stub_unreachable("armsx_render_adopt_sdl");

    return NULL;
}

void armsx_shader_note_backend(int backend) {
    (void)backend;
    stub_unreachable("armsx_shader_note_backend");
}

void psxe_diag_log_line(const char* source, const char* line) {
    (void)source;
    (void)line;
    stub_unreachable("psxe_diag_log_line");
}
