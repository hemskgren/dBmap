/* Test-only Mosquitto v5 plugin. Keep the ordinary ACLs for non-test users. */
#define _POSIX_C_SOURCE 200809L
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mosquitto.h>
#include <mosquitto_broker.h>
#include <mosquitto_plugin.h>

#define TOKEN_LEN 64
#define MAX_TTL_NS UINT64_C(10000000000)
#define TEST_USER "tst-ear-001"
#define LEASE_PATH "/lease/active"

struct connection {
    struct mosquitto *client;
    char token[TOKEN_LEN + 1];
    struct connection *next;
};

static struct connection *connections;

static bool active_token(char token[TOKEN_LEN + 1])
{
    uint64_t issued;
    uint64_t deadline;
    struct timespec now;
    FILE *file = fopen(LEASE_PATH, "r");
    if (!file) {
        return false;
    }
    int fields = fscanf(file, "%" SCNu64 " %" SCNu64 " %64[0-9a-f]",
                        &issued, &deadline, token);
    fclose(file);
    if (fields != 3 || strlen(token) != TOKEN_LEN ||
        deadline <= issued || deadline - issued > MAX_TTL_NS ||
        clock_gettime(CLOCK_MONOTONIC, &now)) {
        return false;
    }
    uint64_t now_ns = (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
    return issued <= now_ns && now_ns < deadline;
}

static void forget(struct mosquitto *client)
{
    struct connection **entry = &connections;
    while (*entry) {
        if ((*entry)->client == client) {
            struct connection *old = *entry;
            *entry = old->next;
            free(old);
            return;
        }
        entry = &(*entry)->next;
    }
}

static int authenticate(int event, void *data, void *context)
{
    (void)event;
    (void)context;
    struct mosquitto_evt_basic_auth *auth = data;
    if (!auth->username || strcmp(auth->username, TEST_USER)) {
        return MOSQ_ERR_PLUGIN_DEFER;
    }
    forget(auth->client);
    char token[TOKEN_LEN + 1];
    if (!auth->password || !active_token(token) || strcmp(token, auth->password)) {
        return MOSQ_ERR_AUTH;
    }
    struct connection *entry = malloc(sizeof(*entry));
    if (!entry) {
        return MOSQ_ERR_NOMEM;
    }
    entry->client = auth->client;
    memcpy(entry->token, token, sizeof(token));
    entry->next = connections;
    connections = entry;
    return MOSQ_ERR_SUCCESS;
}

static int authorize(int event, void *data, void *context)
{
    (void)event;
    (void)context;
    struct mosquitto_evt_acl_check *check = data;
    const char *username = mosquitto_client_username(check->client);
    if (!username || strcmp(username, TEST_USER)) {
        return MOSQ_ERR_PLUGIN_DEFER;
    }
    char token[TOKEN_LEN + 1];
    if (check->access != MOSQ_ACL_WRITE || check->retain ||
        (strcmp(check->topic, "esp-ear/local/" TEST_USER "/observation") &&
         strcmp(check->topic, "esp-ear/local/" TEST_USER "/keepalive")) ||
        !active_token(token)) {
        return MOSQ_ERR_ACL_DENIED;
    }
    for (struct connection *entry = connections; entry; entry = entry->next) {
        if (entry->client == check->client && !strcmp(entry->token, token)) {
            return MOSQ_ERR_SUCCESS;
        }
    }
    return MOSQ_ERR_ACL_DENIED;
}

static int disconnected(int event, void *data, void *context)
{
    (void)event;
    (void)context;
    struct mosquitto_evt_disconnect *disconnect = data;
    forget(disconnect->client);
    return MOSQ_ERR_SUCCESS;
}

int mosquitto_plugin_version(int count, const int *versions)
{
    for (int i = 0; i < count; ++i) {
        if (versions[i] == 5) {
            return 5;
        }
    }
    return -1;
}

int mosquitto_plugin_init(mosquitto_plugin_id_t *id, void **userdata,
                         struct mosquitto_opt *options, int count)
{
    (void)userdata;
    (void)options;
    (void)count;
    if (mosquitto_callback_register(id, MOSQ_EVT_BASIC_AUTH, authenticate, NULL, NULL) ||
        mosquitto_callback_register(id, MOSQ_EVT_ACL_CHECK, authorize, NULL, NULL) ||
        mosquitto_callback_register(id, MOSQ_EVT_DISCONNECT, disconnected, NULL, NULL)) {
        return MOSQ_ERR_INVAL;
    }
    return MOSQ_ERR_SUCCESS;
}

int mosquitto_plugin_cleanup(void *userdata, struct mosquitto_opt *options, int count)
{
    (void)userdata;
    (void)options;
    (void)count;
    while (connections) {
        forget(connections->client);
    }
    return MOSQ_ERR_SUCCESS;
}
