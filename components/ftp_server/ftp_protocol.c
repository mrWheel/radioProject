//-- SPDX-License-Identifier: GPL-3.0-or-later
//-- Copyright (C) 2026 Willem Aandewiel

#include "ftp_protocol.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

bool ftp_normalize_path(const char *cwd, const char *arg, char *out, size_t capacity)
{
  char temporary[FTP_PROTOCOL_PATH_MAX];
  char work[FTP_PROTOCOL_PATH_MAX];
  char *save = NULL;

  if (!cwd || !out || capacity < 2)
  {
    return false;
  }

  if (!arg || !*arg)
  {
    if (snprintf(temporary, sizeof(temporary), "%s", cwd) >= (int)sizeof(temporary))
    {
      return false;
    }
  }
  else if (arg[0] == '/')
  {
    if (snprintf(temporary, sizeof(temporary), "%s", arg) >= (int)sizeof(temporary))
    {
      return false;
    }
  }
  else if (snprintf(temporary, sizeof(temporary), "%s%s%s", cwd,
                    strcmp(cwd, "/") ? "/" : "", arg) >= (int)sizeof(temporary))
  {
    return false;
  }

  if (snprintf(work, sizeof(work), "%s", temporary) >= (int)sizeof(work))
  {
    return false;
  }

  out[0] = '/';
  out[1] = '\0';
  for (char *token = strtok_r(work, "/", &save); token;
       token = strtok_r(NULL, "/", &save))
  {
    if (!strcmp(token, ".") || !*token)
    {
      continue;
    }
    if (!strcmp(token, ".."))
    {
      char *separator = strrchr(out, '/');
      if (separator && separator != out)
      {
        *separator = '\0';
      }
      else
      {
        out[1] = '\0';
      }
      continue;
    }
    if (strchr(token, '\\'))
    {
      return false;
    }
    size_t length = strlen(out);
    size_t token_length = strlen(token);
    if (length + token_length + 2 > capacity)
    {
      return false;
    }
    if (length > 1)
    {
      strcat(out, "/");
    }
    strcat(out, token);
  }
  return true;
}

bool ftp_parser_feed(char *line, size_t *used, char input, bool *complete, bool *overflow)
{
  if (!line || !used || !complete || !overflow)
  {
    return false;
  }

  *complete = false;
  *overflow = false;
  if (input == '\n')
  {
    if (*used && line[*used - 1] == '\r')
    {
      --*used;
    }
    line[*used] = '\0';
    *complete = true;
    return true;
  }
  if (*used + 1 >= FTP_PROTOCOL_LINE_MAX)
  {
    *used = 0;
    *overflow = true;
    return true;
  }
  line[(*used)++] = input;
  return true;
}

int ftp_format_response_v(int code, char *response, size_t capacity, const char *format, va_list arguments)
{
  if (!response || !capacity || !format)
  {
    return -1;
  }

  char text[384];
  int text_length = vsnprintf(text, sizeof(text), format, arguments);
  if (text_length < 0 || text_length >= (int)sizeof(text))
  {
    return -1;
  }

  int response_length = snprintf(response, capacity, "%d %s\r\n", code, text);
  return response_length < 0 || response_length >= (int)capacity ? -1 : response_length;
}

int ftp_format_response(int code, char *response, size_t capacity, const char *format, ...)
{
  va_list arguments;
  va_start(arguments, format);
  int result = ftp_format_response_v(code, response, capacity, format, arguments);
  va_end(arguments);
  return result;
}
