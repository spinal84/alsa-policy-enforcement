#ifndef LOGGING_H
#define LOGGING_H

#include <stdarg.h>
#include "options.h"

typedef enum log_level {
  LOG_LEVEL_ERROR   = 1,
  LOG_LEVEL_INFO    = 2,
  LOG_LEVEL_WARNING = 3,
  LOG_LEVEL_NOTICE  = 4,
  LOG_LEVEL_MAX
} log_level_t;

enum log_flag {
  LOG_FLAG_NULL    = 1 << 0,
  LOG_FLAG_ERROR   = 1 << 1,
  LOG_FLAG_INFO    = 1 << 2,
  LOG_FLAG_WARNING = 1 << 3,
  LOG_FLAG_NOTICE  = 1 << 4,
};

#define LOG_MASK_NONE 0;

#define LOG_MASK_ALL (LOG_FLAG_NULL | LOG_FLAG_ERROR | LOG_FLAG_INFO | \
                      LOG_FLAG_WARNING | LOG_FLAG_NOTICE)


int log_init(struct options *options);

void alsaped_log(log_level_t level, const char *format, ...);

#define log_error(...)   alsaped_log(LOG_LEVEL_ERROR,   __VA_ARGS__)
#define log_info(...)    alsaped_log(LOG_LEVEL_INFO,    __VA_ARGS__)
#define log_warning(...) alsaped_log(LOG_LEVEL_WARNING, __VA_ARGS__)
#define log_notice(...)  alsaped_log(LOG_LEVEL_NOTICE,  __VA_ARGS__)


#endif  /* LOGGING_H */
