#-- SPDX-License-Identifier: GPL-3.0-or-later
#-- Copyright (C) 2026 Willem Aandewiel

idf_component_register(SRCS "ftp_protocol.c" "ftp_server.c" INCLUDE_DIRS "include" REQUIRES esp_netif)
