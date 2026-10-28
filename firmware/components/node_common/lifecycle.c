#include "dbmap_lifecycle.h"

static bool s_ota_reboot_pending;

void dbmap_lifecycle_enter_ota_reboot_pending(void)
{
    s_ota_reboot_pending = true;
}

bool dbmap_lifecycle_ota_reboot_pending(void)
{
    return s_ota_reboot_pending;
}

void dbmap_lifecycle_reset_for_test(void)
{
    s_ota_reboot_pending = false;
}
