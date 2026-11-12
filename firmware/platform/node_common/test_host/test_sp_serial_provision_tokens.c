/**
 * Host contract: USB provisioning readiness/result tokens are platform-owned.
 * Compile: cc -std=c11 -Wall -Wextra -I ../include test_sp_serial_provision_tokens.c -o /tmp/t && /tmp/t
 */
#include "sp_serial_protocol.h"

#include <stdio.h>
#include <string.h>

#define EXPECT(cond)                                                           \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            return 1;                                                          \
        }                                                                      \
    } while (0)

int main(void)
{
    EXPECT(strcmp(SP_PROVISION_READY_TOKEN, "SP_PROVISION_READY") == 0);
    EXPECT(strcmp(SP_PROVISION_RESULT_PREFIX, "SP_PROVISION_RESULT:") == 0);
    /* Product must not own the readiness token name. */
    EXPECT(strstr(SP_PROVISION_READY_TOKEN, "DBMAP_") == NULL);
    EXPECT(strstr(SP_PROVISION_RESULT_PREFIX, "DBMAP_") == NULL);
    /* Prefix must end with ':' so JSON can follow on the same line. */
    EXPECT(SP_PROVISION_RESULT_PREFIX[strlen(SP_PROVISION_RESULT_PREFIX) - 1] == ':');
    puts("test_sp_serial_provision_tokens: ok");
    return 0;
}
