//-- SPDX-License-Identifier: GPL-3.0-or-later
//-- Copyright (C) 2026 Willem Aandewiel

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdarg.h>

#define FTP_PROTOCOL_PATH_MAX 512
#define FTP_PROTOCOL_LINE_MAX 512

bool ftp_normalize_path(const char *cwd, const char *arg, char *out, size_t capacity);

bool ftp_parser_feed(char *line, size_t *used, char input, bool *complete, bool *overflow);

int ftp_format_response(int code, char *response, size_t capacity, const char *format, ...);
int ftp_format_response_v(int code, char *response, size_t capacity, const char *format, va_list arguments);
