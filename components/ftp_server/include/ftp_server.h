//-- SPDX-License-Identifier: GPL-3.0-or-later
//-- Copyright (C) 2026 Willem Aandewiel

#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    const char *base_path;
    uint16_t control_port;
    uint16_t passive_port_min;
    uint16_t passive_port_max;
    size_t transfer_buffer_size;
    uint32_t control_timeout_ms;
    uint32_t data_timeout_ms;
    size_t max_clients;
} ftp_server_config_t;
#define FTP_SERVER_DEFAULT_CONFIG() { \
    .base_path="/storage", .control_port=21, \
    .passive_port_min=50000, .passive_port_max=50100, \
    .transfer_buffer_size=4096, .control_timeout_ms=300000, \
    .data_timeout_ms=120000, .max_clients=2 }
esp_err_t ftp_server_start(const ftp_server_config_t *config);
esp_err_t ftp_server_stop(void);
bool ftp_server_is_running(void);
#ifdef __cplusplus
}
#endif
