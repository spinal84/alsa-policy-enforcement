#ifndef ALSAIF_H
#define ALSAIF_H

#include <alsa/asoundlib.h>
#include "options.h"

typedef struct _alsaif_event      alsaif_event;
typedef union  _value_descriptor  value_descriptor;
typedef void   (*alsaif_event_cb) (alsaif_event*);

enum alsaif_event_type {
  /** Add sound card */
  EVENT_CARD = 1 << 0,
  /** Add all control elements */
  EVENT_CTLS = 1 << 1,
  /** Add single control element */
  EVENT_ELEM = 1 << 2
};

struct alsaif_event_card {
  char *id;
  char *name;
  int   num;
};

struct alsaif_event_elem {
  char *ifname;
  char *name;
  int   index;
  int   dev;
  int   subdev;
  int   card_num;
  int   numid;
};

struct _alsaif_event {
  enum alsaif_event_type  type;
  union {
    struct alsaif_event_card card;
    struct alsaif_event_elem elem;
  };
};

struct value_descriptor_int {
  long min;
  long max;
  long step;
};

struct value_descriptor_enum {
  int count;
  char **names;
};

union _value_descriptor {
  struct value_descriptor_enum enum_t;
  struct value_descriptor_int  int_t;
};


void alsaif_set_cb    (alsaif_event_cb cb);
int  alsaif_create    (void);
int  alsaif_init      (struct options *options);
int  alsaif_get_value (int cardnum, int numid, long *value);
int  alsaif_set_value (int cardnum, int numid, long *value);

snd_ctl_elem_type_t
alsaif_get_value_descriptor(int cardnum,
                            int numid,
                            value_descriptor **descriptor);


#endif  /* ALSAIF_H */
