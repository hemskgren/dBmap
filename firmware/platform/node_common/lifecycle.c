#include "sp_lifecycle.h"

static bool s_ota_reboot_pending;

void sp_lifecycle_enter_ota_reboot_pending(void)
{
    s_ota_reboot_pending = true;
}

bool sp_lifecycle_ota_reboot_pending(void)
{
    return s_ota_reboot_pending;
}

void sp_lifecycle_reset_for_test(void)
{
    s_ota_reboot_pending = false;
}
