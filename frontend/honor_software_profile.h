#ifndef ARMSX_HONOR_SOFTWARE_PROFILE_H
#define ARMSX_HONOR_SOFTWARE_PROFILE_H

#include <string.h>

/* Scope the software shading experiment to the device in the supplied report.
   GPU model alone is insufficient: SDL software can leave its vendor unknown. */
static inline int armsx_honor_deferred_shading_device(const char* manufacturer,
                                                      const char* model,
                                                      const char* device) {
    return manufacturer && model && device &&
        strcmp(manufacturer, "HONOR") == 0 &&
        strcmp(model, "ELI-NX9") == 0 && strcmp(device, "HNELIX") == 0;
}

#endif
