/**
 * @file alsaif.c Logging functions
 * @copyright GNU GPLv2 or later
 *
 * @{ */

#include <stdarg.h>
#include <syslog.h>
#include <stdio.h>
#include <time.h>

#include "logging.h"

#define MSG_SIZE 1024

static struct {
  int syslog;
  int mask;
} priv;

int
log_init(struct options *options)
{
  priv.syslog = options->daemon && !options->list_and_exit;
  priv.mask   = options->log_mask | LOG_FLAG_NOTICE;

  if (priv.syslog)
    openlog("alsaped", LOG_NOWAIT, LOG_DAEMON);

  return 0;
}

void
alsaped_log(log_level_t level, const char *format, ...)
{
  va_list args;

  if (!level || level >= LOG_LEVEL_MAX || !(priv.mask >> level & 1))
    return;

  va_start(args, format);

  if (priv.syslog)
  {
    int prio;

    if (level == LOG_LEVEL_WARNING)
      prio = LOG_WARNING;
    else if (level == LOG_LEVEL_NOTICE || level == LOG_LEVEL_INFO)
      prio = LOG_INFO;
    else
      prio = LOG_ERR;

    vsyslog(prio, format, args);
  }
  else
  {
    char msg[MSG_SIZE];
    time_t timer;
    struct tm *timeinfo;
    char *prefix = "";

    time(&timer);
    timeinfo = localtime(&timer);

    if (level == LOG_LEVEL_ERROR)
      prefix = " [ERROR]";
    else if (level == LOG_LEVEL_WARNING)
      prefix = " [WARNING]";

    snprintf(msg, MSG_SIZE,
             "%02d:%02d:%02d alsaped%s: %s\n",
             timeinfo->tm_hour, timeinfo->tm_min,
             timeinfo->tm_sec, prefix, format);

    vfprintf(stderr, msg, args);
  }

  va_end(args);
}

/** @} */
