/*
    ARMSX — mobile GPU identification and per-driver behaviour profile.

    See gpu_profile.h for what each flag means and where it was observed. This file is the
    detection: strings and IDs in, a profile out.

    Two rules govern everything here.

    1. Detection must see THROUGH ANGLE. ANGLE reports itself as the renderer, with the real
       GPU parenthesised inside its string:
           "ANGLE (ARM, Vulkan 1.1.177 (Mali-G77 MC9), ...)"
       A naive vendor match finds "Google"/"ANGLE" and concludes "unknown GPU", losing every
       Mali fact at exactly the moment they matter — ANGLE is most often enabled precisely
       because the native driver misbehaved.

    2. A gate is only ever as wide as the hardware it was measured on. Where a rule is known
       for one model (the Adreno 650 buffer teardown) it keys on that model. Widening it to the
       vendor is what caused a measured regression in ARMSX2, so the narrowness is the point,
       not an oversight.
*/

#include "gpu_profile.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* VkDriverId values we distinguish. Duplicated as plain integers so this file needs no Vulkan
   headers; the names match the spec constants exactly. */
#define VKDRV_AMD_PROPRIETARY        1
#define VKDRV_MESA_RADV              3
#define VKDRV_NVIDIA_PROPRIETARY     4
#define VKDRV_IMGTEC_PROPRIETARY     7
#define VKDRV_QUALCOMM_PROPRIETARY   8
#define VKDRV_ARM_PROPRIETARY        9
#define VKDRV_SAMSUNG_PROPRIETARY   12
#define VKDRV_MESA_TURNIP           16

static armsx_gpu_profile_t g_profile;
static int g_profile_ready = 0;
static int g_profile_locked_by_override = 0;
/* 0 = follow the profile, 1 = force on, -1 = force off. */
static int g_force_fbfetch = 0;

/* ------------------------------------------------------------------------------------------ */

static void lower_copy(char* dst, size_t dst_size, const char* src)
{
    size_t i = 0;
    if (!dst || dst_size == 0) {
        return;
    }
    if (src) {
        for (; src[i] != '\0' && i + 1 < dst_size; ++i) {
            dst[i] = (char)tolower((unsigned char)src[i]);
        }
    }
    dst[i] = '\0';
}

static void str_copy(char* dst, size_t dst_size, const char* src)
{
    if (!dst || dst_size == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, dst_size, "%s", src);
}

/* First run of digits at or after `from`. Returns 0 when there is none — callers must treat 0
   as "unknown", never as a small model number. */
static int parse_number(const char* text)
{
    int value = 0;
    int seen = 0;
    if (!text) {
        return 0;
    }
    while (*text != '\0' && !isdigit((unsigned char)*text)) {
        ++text;
    }
    while (isdigit((unsigned char)*text)) {
        value = value * 10 + (*text - '0');
        ++text;
        seen = 1;
        if (value > 100000) {
            return 0; /* not a model number; bail rather than overflow into nonsense */
        }
    }
    return seen ? value : 0;
}

static void ensure_ready(void)
{
    if (!g_profile_ready) {
        armsx_gpu_profile_reset();
    }
}

/* ------------------------------------------------------------------------------------------ */

void armsx_gpu_profile_reset(void)
{
    memset(&g_profile, 0, sizeof(g_profile));
    g_profile.vendor = ARMSX_GPU_VENDOR_UNKNOWN;
    g_profile.driver = ARMSX_GPU_DRIVER_UNKNOWN;
    str_copy(g_profile.name, sizeof(g_profile.name), "Unknown");

    /* Conservative defaults for an unidentified GPU: assume the capabilities work (so we do not
       pessimise hardware nobody has characterised) but obey the two shader-authoring rules
       unconditionally, since honouring them costs nothing anywhere. */
    g_profile.fbfetch_gl = 1;
    g_profile.fbfetch_vk_roaa = 1;
    g_profile.dual_source_blend = 1;
    g_profile.shader_helpers_must_be_macros = 1;

    g_profile_ready = 1;
    g_profile_locked_by_override = 0;
    g_force_fbfetch = 0;
}

/*
    Derive every behaviour flag from (vendor, driver, model). Called after any identification
    step so the flags never disagree with the identity — the failure mode of hand-setting flags
    at each detection site is that one site updates the model and forgets a flag.
*/
static void apply_known_behaviour(void)
{
    armsx_gpu_profile_t* p = &g_profile;

    /* Start from the conservative baseline, then narrow. */
    p->avoid_push_descriptors = 0;
    p->avoid_persistent_buffer_map = 0;
    p->prefer_buffer_orphaning = 0;
    p->fbfetch_gl = 1;
    p->fbfetch_vk_roaa = 1;
    p->dual_source_blend = 1;
    p->single_fbfetch_attachment = 0;
    p->needs_es31_fallback = p->is_angle ? 1 : 0;
    p->shader_helpers_must_be_macros = 1;

    switch (p->vendor) {
    case ARMSX_GPU_VENDOR_MALI:
        /* vkCmdPushDescriptorSetKHR null-derefs on the first textured draw. */
        p->avoid_push_descriptors = 1;
        /* glBufferSubData forces a sync; orphaning is faster. */
        p->prefer_buffer_orphaning = 1;
        /* No dual-source blending. A renderer assuming it produces wrong output silently. */
        p->dual_source_blend = 0;
        /* MediaTek Mali (across generations) and the Mali-G57 return zero/stale destination
           colour from framebuffer fetch, in both APIs. Other Mali parts keep the fast path —
           note Mali's "broken unless Blending=Max" behaviour is a VULKAN-only story; the GL
           path is fine there, which is the other half of why these two flags are separate. */
        if (p->is_mediatek || p->model == 57) {
            p->fbfetch_gl = 0;
            p->fbfetch_vk_roaa = 0;
        }
        break;

    case ARMSX_GPU_VENDOR_ADRENO:
        /* Push descriptors stall on the per-draw texture-rebind path on Mesa/Turnip, but are
           correct and faster on the proprietary driver. UNKNOWN keeps the conservative
           disable. This is the gate that must key on driver, not vendor. */
        p->avoid_push_descriptors = (p->driver != ARMSX_GPU_DRIVER_QUALCOMM_PROPRIETARY);
        /* The 650's driver tears out persistently-mapped buffers. Model-specific on purpose. */
        p->avoid_persistent_buffer_map = (p->model == 650);
        /* GLES rejects a shader with two framebuffer-fetch `inout` attachments — garbage
           output, no compile error. */
        p->single_fbfetch_attachment = 1;
        /* GLES framebuffer fetch works on Adreno — subject to the one-attachment limit above.
           Its Vulkan ROAA is the broken half: the proprietary blob performs stale reads, while
           Mesa/Turnip does not. */
        p->fbfetch_gl = 1;
        p->fbfetch_vk_roaa = (p->driver == ARMSX_GPU_DRIVER_MESA_TURNIP);
        break;

    case ARMSX_GPU_VENDOR_XCLIPSE:
        /* No working ROAA-based framebuffer fetch. NOTE: Xclipse identification rests on the
           0x144D vendor ID, which no Xclipse device has yet confirmed for us. If that ID is
           wrong this branch is simply never taken — it cannot mis-fire on other hardware — and
           a tester can reach it with the "xclipse" override. */
        p->fbfetch_vk_roaa = 0;
        break;

    case ARMSX_GPU_VENDOR_POWERVR:
    case ARMSX_GPU_VENDOR_UNKNOWN:
    default:
        break;
    }

    /* ANGLE gives a GLES 3.1 context regardless of what the native driver supports, so any
       GLES 3.2 feature needs an ES-3.1 path. This is independent of vendor and must survive
       the switch above. */
    if (p->is_angle) {
        p->needs_es31_fallback = 1;
    }

    /* The user's explicit choice wins, and is applied LAST so it survives every rule above.
       Applied to both APIs: someone A/B-ing this is answering "does fetch work on my device",
       and splitting the switch would just make the answer harder to give. */
    if (g_force_fbfetch > 0) {
        p->fbfetch_gl = 1;
        p->fbfetch_vk_roaa = 1;
    } else if (g_force_fbfetch < 0) {
        p->fbfetch_gl = 0;
        p->fbfetch_vk_roaa = 0;
    }
    p->fbfetch_forced = g_force_fbfetch;
}

int armsx_gpu_profile_force_fbfetch(const char* value)
{
    char lowered[32];

    ensure_ready();
    lower_copy(lowered, sizeof(lowered), value);

    if (lowered[0] == '\0' || strcmp(lowered, "auto") == 0) {
        g_force_fbfetch = 0;
    } else if (strcmp(lowered, "on") == 0 || strcmp(lowered, "true") == 0 ||
               strcmp(lowered, "1") == 0 || strcmp(lowered, "force") == 0) {
        g_force_fbfetch = 1;
    } else if (strcmp(lowered, "off") == 0 || strcmp(lowered, "false") == 0 ||
               strcmp(lowered, "0") == 0 || strcmp(lowered, "disable") == 0) {
        g_force_fbfetch = -1;
    } else {
        return 0;
    }

    apply_known_behaviour();
    return 1;
}

/* ------------------------------------------------------------------------------------------ */

/*
    Records everything the text reveals. When the user has pinned a profile we still gather the
    FACTS — ANGLE, MediaTek, the model number — and suppress only the vendor conclusion.

    That distinction is load-bearing. MediaTek-ness is what turns Mali's framebuffer fetch off,
    so a tester on a MediaTek Mali device who forces "mali" (the natural thing to do when
    detection has failed them) would otherwise be handed fbfetch=yes — the precise
    configuration the override exists to let them escape.
*/
static void identify_from_text(const char* lowered)
{
    armsx_gpu_profile_t* p = &g_profile;
    const armsx_gpu_vendor_t pinned_vendor = p->vendor;
    const char* found;

    if (!lowered || lowered[0] == '\0') {
        return;
    }

    if (strstr(lowered, "angle") != NULL) {
        p->is_angle = 1;
    }

    /* Order matters: check the specific GPU family names before vendor names, because ANGLE's
       string contains both ("ANGLE (ARM, Vulkan ... (Mali-G77 MC9)")  and the family name is
       the more precise signal. */
    if ((found = strstr(lowered, "adreno")) != NULL) {
        p->vendor = ARMSX_GPU_VENDOR_ADRENO;
        if (p->model == 0) {
            p->model = parse_number(found);
        }
    } else if ((found = strstr(lowered, "mali")) != NULL ||
               (found = strstr(lowered, "immortalis")) != NULL) {
        p->vendor = ARMSX_GPU_VENDOR_MALI;
        if (p->model == 0) {
            p->model = parse_number(found);
        }
    } else if ((found = strstr(lowered, "powervr")) != NULL ||
               strstr(lowered, "img") != NULL) {
        p->vendor = ARMSX_GPU_VENDOR_POWERVR;
    } else if (strstr(lowered, "xclipse") != NULL) {
        p->vendor = ARMSX_GPU_VENDOR_XCLIPSE;
    } else if (strstr(lowered, "qualcomm") != NULL) {
        p->vendor = ARMSX_GPU_VENDOR_ADRENO;
    } else if (strstr(lowered, "arm") != NULL) {
        p->vendor = ARMSX_GPU_VENDOR_MALI;
    }

    if (p->vendor == ARMSX_GPU_VENDOR_ADRENO && p->model >= 200 && p->model < 1000) {
        p->generation = p->model / 100;
    } else if (p->vendor == ARMSX_GPU_VENDOR_MALI && p->model > 0) {
        /* Mali model numbers do not encode a generation arithmetically (G57 and G610 are both
           Valhall), so leave generation at 0 rather than inventing one. */
        p->generation = 0;
    }

    /* MediaTek SoCs use compact part numbers such as mt6877 / mt8195 in their board strings. */
    if (strstr(lowered, "mediatek") != NULL || strstr(lowered, "mtk") != NULL) {
        p->is_mediatek = 1;
    } else if ((found = strstr(lowered, "mt")) != NULL && isdigit((unsigned char)found[2])) {
        p->is_mediatek = 1;
    }

    if (g_profile_locked_by_override) {
        p->vendor = pinned_vendor;
    }
}

void armsx_gpu_profile_note_gl(const char* vendor, const char* renderer, const char* version)
{
    char lowered[512];
    char joined[512];

    ensure_ready();

    /* No early return when a profile is pinned: identify_from_text() protects the pinned vendor
       itself, and everything else it learns — ANGLE, MediaTek, the model — is a fact about the
       device that the override was never meant to discard. */
    snprintf(joined, sizeof(joined), "%s %s %s", vendor ? vendor : "",
             renderer ? renderer : "", version ? version : "");
    lower_copy(lowered, sizeof(lowered), joined);

    if (renderer && renderer[0] != '\0') {
        str_copy(g_profile.name, sizeof(g_profile.name), renderer);
    }
    identify_from_text(lowered);

    /* Infer the driver from the GL_VERSION string when Vulkan has not already told us. Without
       this the GL-only path leaves the driver "unknown" forever, and every driver-keyed gate
       then falls to its conservative branch — which on Adreno means permanently assuming the
       Turnip behaviour on a device running the proprietary blob, i.e. exactly the vendor-wide
       over-reach this module exists to avoid.

       Mesa/Turnip says so by name. The Qualcomm blob is recognised by its "v@<build>" version
       marker, which no other driver emits. Anything else stays unknown rather than guessing. */
    if (g_profile.driver == ARMSX_GPU_DRIVER_UNKNOWN) {
        if (strstr(lowered, "turnip") != NULL || strstr(lowered, "mesa") != NULL) {
            g_profile.driver = ARMSX_GPU_DRIVER_MESA_TURNIP;
        } else if (strstr(lowered, "v@") != NULL &&
                   g_profile.vendor == ARMSX_GPU_VENDOR_ADRENO) {
            g_profile.driver = ARMSX_GPU_DRIVER_QUALCOMM_PROPRIETARY;
        } else if (g_profile.vendor == ARMSX_GPU_VENDOR_MALI &&
                   strstr(lowered, "opengl es") != NULL) {
            g_profile.driver = ARMSX_GPU_DRIVER_ARM_PROPRIETARY;
        }
    }

    apply_known_behaviour();
}

void armsx_gpu_profile_note_vk(unsigned int vendor_id, unsigned int driver_id,
                               const char* device_name, const char* driver_name,
                               const char* driver_info)
{
    char lowered[512];

    ensure_ready();

    if (device_name && device_name[0] != '\0') {
        str_copy(g_profile.name, sizeof(g_profile.name), device_name);
    }
    str_copy(g_profile.driver_name, sizeof(g_profile.driver_name), driver_name);
    str_copy(g_profile.driver_info, sizeof(g_profile.driver_info), driver_info);

    switch (driver_id) {
    case VKDRV_QUALCOMM_PROPRIETARY: g_profile.driver = ARMSX_GPU_DRIVER_QUALCOMM_PROPRIETARY; break;
    case VKDRV_MESA_TURNIP:          g_profile.driver = ARMSX_GPU_DRIVER_MESA_TURNIP; break;
    case VKDRV_ARM_PROPRIETARY:      g_profile.driver = ARMSX_GPU_DRIVER_ARM_PROPRIETARY; break;
    case VKDRV_SAMSUNG_PROPRIETARY:  g_profile.driver = ARMSX_GPU_DRIVER_SAMSUNG_PROPRIETARY; break;
    case VKDRV_IMGTEC_PROPRIETARY:   g_profile.driver = ARMSX_GPU_DRIVER_IMGTEC_PROPRIETARY; break;
    default:                         g_profile.driver = ARMSX_GPU_DRIVER_UNKNOWN; break;
    }

    /*
        Fall back to the NAMES when driverID did not identify it.

        driverID is only populated when VkPhysicalDeviceDriverProperties was actually filled in,
        which needs a 1.1 instance AND a driver that supports it. A custom driver loaded through
        adrenotools reached this with driverID 0 and reported "unknown driver" while plainly
        being Turnip — and "unknown" is not cosmetic here, it selects the conservative branch of
        every driver-keyed gate. deviceName carries it too: Turnip prefixes the device with
        "Turnip", which is why that string is checked as well as driverName.
    */
    if (g_profile.driver == ARMSX_GPU_DRIVER_UNKNOWN) {
        char hint[320];
        char joined[320];

        snprintf(joined, sizeof(joined), "%s %s %s",
                 driver_name ? driver_name : "",
                 driver_info ? driver_info : "",
                 device_name ? device_name : "");
        lower_copy(hint, sizeof(hint), joined);

        if (strstr(hint, "turnip") != NULL || strstr(hint, "mesa") != NULL ||
            strstr(hint, "freedreno") != NULL) {
            g_profile.driver = ARMSX_GPU_DRIVER_MESA_TURNIP;
        } else if (strstr(hint, "qualcomm") != NULL) {
            g_profile.driver = ARMSX_GPU_DRIVER_QUALCOMM_PROPRIETARY;
        } else if (strstr(hint, "arm ") != NULL || strstr(hint, "mali") != NULL) {
            g_profile.driver = ARMSX_GPU_DRIVER_ARM_PROPRIETARY;
        }
    }

    if (!g_profile_locked_by_override) {
        /* The vendor ID is the authoritative signal when present; the name refines the model. */
        switch (vendor_id) {
        case ARMSX_GPU_VENDOR_ID_ARM:      g_profile.vendor = ARMSX_GPU_VENDOR_MALI; break;
        case ARMSX_GPU_VENDOR_ID_QUALCOMM: g_profile.vendor = ARMSX_GPU_VENDOR_ADRENO; break;
        case ARMSX_GPU_VENDOR_ID_IMGTEC:   g_profile.vendor = ARMSX_GPU_VENDOR_POWERVR; break;
        case ARMSX_GPU_VENDOR_ID_SAMSUNG:  g_profile.vendor = ARMSX_GPU_VENDOR_XCLIPSE; break;
        default: break;
        }
    }

    /* Outside the guard for the same reason as the GL path: the device name carries MediaTek-ness
       and the model number, which a pinned vendor must not throw away. identify_from_text()
       restores the pinned vendor itself. */
    lower_copy(lowered, sizeof(lowered), device_name);
    identify_from_text(lowered);

    apply_known_behaviour();
}

void armsx_gpu_profile_note_host_hint(const char* hint)
{
    char lowered[256];

    ensure_ready();
    lower_copy(lowered, sizeof(lowered), hint);
    if (lowered[0] == '\0') {
        return;
    }

    /* Android supplies two kinds of pre-renderer facts through this entry point:
       Build.SOC_* identifies the host/MediaTek combination, while GpuInfo's worker-thread
       pbuffer probe supplies the system GL_RENDERER (for example Mali-G715). Feed both through
       the same fact collector used by real GL contexts so a software-only session can report and
       apply the correct physical-GPU profile before its first frame. This does not guess a driver:
       the pbuffer result has no GL_VERSION, so that remains unknown until a real renderer sees it. */
    identify_from_text(lowered);
    apply_known_behaviour();
}

int armsx_gpu_profile_override(const char* value)
{
    char lowered[64];

    ensure_ready();
    lower_copy(lowered, sizeof(lowered), value);

    if (lowered[0] == '\0' || strcmp(lowered, "auto") == 0) {
        g_profile_locked_by_override = 0;
        g_profile.forced = 0;
        return 0;
    }

    if (strcmp(lowered, "adreno") == 0) {
        g_profile.vendor = ARMSX_GPU_VENDOR_ADRENO;
    } else if (strcmp(lowered, "mali") == 0) {
        g_profile.vendor = ARMSX_GPU_VENDOR_MALI;
    } else if (strcmp(lowered, "powervr") == 0) {
        g_profile.vendor = ARMSX_GPU_VENDOR_POWERVR;
    } else if (strcmp(lowered, "xclipse") == 0) {
        g_profile.vendor = ARMSX_GPU_VENDOR_XCLIPSE;
    } else {
        return 0;
    }

    g_profile.forced = 1;
    g_profile_locked_by_override = 1;
    apply_known_behaviour();
    return 1;
}

const armsx_gpu_profile_t* armsx_gpu_profile_get(void)
{
    ensure_ready();
    return &g_profile;
}

const char* armsx_gpu_profile_vendor_name(armsx_gpu_vendor_t vendor)
{
    switch (vendor) {
    case ARMSX_GPU_VENDOR_ADRENO:  return "Adreno";
    case ARMSX_GPU_VENDOR_MALI:    return "Mali";
    case ARMSX_GPU_VENDOR_POWERVR: return "PowerVR";
    case ARMSX_GPU_VENDOR_XCLIPSE: return "Xclipse";
    case ARMSX_GPU_VENDOR_UNKNOWN:
    default:                       return "Unknown";
    }
}

const char* armsx_gpu_profile_driver_name(armsx_gpu_driver_t driver)
{
    switch (driver) {
    case ARMSX_GPU_DRIVER_QUALCOMM_PROPRIETARY: return "Qualcomm proprietary";
    case ARMSX_GPU_DRIVER_MESA_TURNIP:          return "Mesa/Turnip";
    case ARMSX_GPU_DRIVER_ARM_PROPRIETARY:      return "ARM proprietary";
    case ARMSX_GPU_DRIVER_SAMSUNG_PROPRIETARY:  return "Samsung proprietary";
    case ARMSX_GPU_DRIVER_IMGTEC_PROPRIETARY:   return "Imagination proprietary";
    case ARMSX_GPU_DRIVER_UNKNOWN:
    default:                                    return "unknown driver";
    }
}

void armsx_gpu_profile_describe(char* out, size_t out_size)
{
    const armsx_gpu_profile_t* p;
    char info[40];
    char model[16];

    if (!out || out_size == 0) {
        return;
    }
    ensure_ready();
    p = &g_profile;

    /*
        Compact on purpose: this string is appended to the OSD's renderer row, which is a single
        unwrapped line. The first version printed the device name twice — once from the render
        backend and again from here — and pasted Adreno's whole driverInfo, which is a build hash
        and two ids. The result ran off BOTH edges of the screen and was unreadable, which defeats
        the point of putting it in front of a tester.

        So: no device name (the caller already printed it), and driverInfo truncated to its first
        token, which is the part that identifies a driver revision ("r44p1" on Mali). The full
        string is still in the log line at startup for anyone who needs it.
    */
    info[0] = '\0';
    if (p->driver_info[0] != '\0') {
        size_t i = 0;

        for (; i + 1 < sizeof(info) && p->driver_info[i] != '\0'; ++i) {
            const char c = p->driver_info[i];

            if (c == ' ' || c == ',' || c == '(') {
                break;
            }
            info[i] = c;
        }
        info[i] = '\0';

        /* Keep it only if it looks like a VERSION. Mali reports "r44p1", which identifies the
           driver revision a bug report needs; Adreno reports "Driver Build: <hash>, ...", whose
           first token is the bare word "Driver" and says nothing. Requiring a digit keeps the
           former and drops the latter without hard-coding either vendor's format. */
        {
            int has_digit = 0;
            size_t k;

            for (k = 0; info[k] != '\0'; ++k) {
                if (info[k] >= '0' && info[k] <= '9') {
                    has_digit = 1;
                    break;
                }
            }
            if (!has_digit) {
                info[0] = '\0';
            }
        }
    }

    model[0] = '\0';
    if (p->model > 0) {
        snprintf(model, sizeof(model), " %d", p->model);
    }

    snprintf(out, out_size, "%s%s%s %s%s%s fb-gl=%s fb-vk=%s dual=%s push=%s%s%s",
             armsx_gpu_profile_vendor_name(p->vendor),
             model,
             p->forced ? " [forced]" : "",
             armsx_gpu_profile_driver_name(p->driver),
             info[0] != '\0' ? " " : "",
             info,
             p->fbfetch_gl ? "y" : "n",
             p->fbfetch_vk_roaa ? "y" : "n",
             p->dual_source_blend ? "y" : "n",
             p->avoid_push_descriptors ? "n" : "y",
             p->is_mediatek ? " mediatek" : "",
             p->is_angle ? " angle" : "");

    if (p->fbfetch_forced != 0) {
        const size_t used = strlen(out);
        if (used + 1 < out_size) {
            snprintf(out + used, out_size - used, " fb-forced=%s",
                     p->fbfetch_forced > 0 ? "on" : "off");
        }
    }
}
