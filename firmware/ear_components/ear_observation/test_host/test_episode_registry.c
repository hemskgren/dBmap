/**
 * Host unit tests for episode registry.
 *
 *   cc -std=c11 -Wall -Wextra -O0 -g \
 *     -I firmware/ear_components/ear_observation/include \
 *     firmware/ear_components/ear_observation/episode_registry.c \
 *     firmware/ear_components/ear_observation/test_host/test_episode_registry.c \
 *     -o /tmp/test_episode_registry && /tmp/test_episode_registry
 */
#include "dbmap_episode_registry.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;
static int g_uuid_n = 0;

#define EXPECT(cond)                                                           \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

static void fake_uuid(char out[DBMAP_OBS_EPISODE_ID_LEN])
{
    snprintf(out, DBMAP_OBS_EPISODE_ID_LEN, "00000000-0000-4000-8000-%012d", g_uuid_n++);
}

static void test_capacity_and_reuse(void)
{
    dbmap_episode_registry_t reg;
    dbmap_episode_registry_init(&reg);
    dbmap_ep_handle_t h0, h1, h2;
    EXPECT(dbmap_episode_registry_allocate(&reg, &h0, 1000, 5000, 1, fake_uuid, true));
    EXPECT(dbmap_episode_registry_allocate(&reg, &h1, 2000, 5000, 2, fake_uuid, true));
    EXPECT(!dbmap_episode_registry_allocate(&reg, &h2, 3000, 5000, 3, fake_uuid, true));
    EXPECT(dbmap_episode_registry_in_use_count(&reg) == 2);

    char id0[37];
    strncpy(id0, dbmap_episode_registry_get(&reg, h0)->observation_id, 36);
    id0[36] = '\0';
    dbmap_episode_registry_release(&reg, h0);
    EXPECT(dbmap_episode_registry_in_use_count(&reg) == 1);
    EXPECT(dbmap_episode_registry_allocate(&reg, &h2, 4000, 5000, 3, fake_uuid, false));
    EXPECT(strcmp(dbmap_episode_registry_get(&reg, h2)->observation_id, id0) != 0);
    EXPECT(h2.index == 0 || h2.index == 1);
}

static void test_deferred_complete_vs_start(void)
{
    dbmap_episode_registry_t reg;
    dbmap_episode_registry_init(&reg);
    dbmap_ep_handle_t h;
    g_uuid_n = 10;
    EXPECT(dbmap_episode_registry_allocate(&reg, &h, 0, 5000, 1, fake_uuid, true));
    const dbmap_episode_slot_t *s = dbmap_episode_registry_get_const(&reg, h);
    EXPECT(s != NULL);
    EXPECT(s->first_periodic_due_mono_us == 5000000);
    EXPECT(strcmp(dbmap_episode_registry_deferred_decision(s, 1000000, true), "complete") == 0);
    EXPECT(strcmp(dbmap_episode_registry_deferred_decision(s, 5000000, false), "start") == 0);

    dbmap_episode_registry_mark_start_accepted(&reg, h, 5000000);
    s = dbmap_episode_registry_get_const(&reg, h);
    EXPECT(strcmp(dbmap_episode_registry_deferred_decision(s, 6000000, true), "end") == 0);
    EXPECT(dbmap_episode_registry_deferred_decision(s, 6000000, false) == NULL);
}

int main(void)
{
    test_capacity_and_reuse();
    test_deferred_complete_vs_start();
    if (g_failures) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    puts("ok");
    return 0;
}
