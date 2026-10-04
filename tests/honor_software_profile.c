#include "../frontend/honor_software_profile.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    assert(armsx_honor_deferred_shading_device("HONOR", "ELI-NX9", "HNELIX"));
    assert(!armsx_honor_deferred_shading_device("HONOR", "ELI-NX9", "other"));
    assert(!armsx_honor_deferred_shading_device("HONOR", "other", "HNELIX"));
    assert(!armsx_honor_deferred_shading_device("other", "ELI-NX9", "HNELIX"));
    assert(!armsx_honor_deferred_shading_device("AYN", "Thor", "Thor"));
    assert(!armsx_honor_deferred_shading_device("Anbernic", "RG477V", "rg477v"));
    assert(!armsx_honor_deferred_shading_device("motorola", "one", "deen_sprout"));
    assert(!armsx_honor_deferred_shading_device(NULL, "ELI-NX9", "HNELIX"));
    assert(!armsx_honor_deferred_shading_device("HONOR", NULL, "HNELIX"));
    assert(!armsx_honor_deferred_shading_device("HONOR", "ELI-NX9", NULL));
    puts("HONOR exact-device gate: passed");
    return 0;
}
