#ifndef DBUSIF_H
#define DBUSIF_H

#include "control.h"

/** Audio action parsed data */
struct action_data {
  enum rule_type rule_type;
  union {
    char *route_dev;
    struct {
      char *variable;
      char *value;
    };
  };
};

/** Audio actions handler */
typedef int (*action_handler) (struct action_data *data);

int  dbusif_init   (struct options *options);
int  dbusif_create (void);
void dbusif_set_cb (action_handler cb);


#endif  /* DBUSIF_H */
