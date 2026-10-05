#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DBMAP_NODE_ID_MAX 32
#define DBMAP_HOST_MAX 128
#define DBMAP_TOPIC_MAX 160
#define DBMAP_TOKEN_MAX 128

typedef struct {
    char node_id[DBMAP_NODE_ID_MAX];
    char hardware_revision[64];
    char provisioning_state[32];
} dbmap_identity_t;

typedef struct {
    char ssid[33];
    char password[65];
    char hub_http_url[DBMAP_HOST_MAX];
    char mqtt_host[DBMAP_HOST_MAX];
    uint16_t mqtt_port;
    bool mqtt_use_tls;
    char mqtt_username[64];
    char mqtt_password[128];
} dbmap_network_t;

typedef struct {
    uint32_t config_version;
    uint32_t calibration_version;
    char classifier_version[32];
    char profile[32];
} dbmap_config_t;

typedef struct {
    char hello[DBMAP_TOPIC_MAX];
    char keepalive[DBMAP_TOPIC_MAX];
    char health[DBMAP_TOPIC_MAX];
    char status[DBMAP_TOPIC_MAX];
    char command[DBMAP_TOPIC_MAX];
    char ack[DBMAP_TOPIC_MAX];
    char config[DBMAP_TOPIC_MAX];
} dbmap_topics_t;

typedef struct {
    dbmap_identity_t identity;
    dbmap_network_t network;
    dbmap_config_t config;
    dbmap_topics_t topics;
    char bootstrap_token[DBMAP_TOKEN_MAX];
    bool provisioned;
} dbmap_effective_t;

void dbmap_effective_defaults(dbmap_effective_t *out);
void dbmap_rebuild_topics(dbmap_effective_t *cfg);
