#ifndef CONTROL_H
#define CONTROL_H

#include "options.h"

enum rule_type {
  rule_unknown = 0,
  /* sink route rule */
  rule_sink,
  /* source route rule */
  rule_source,
  /* context rule */
  rule_context,
  rule_max
};

enum action_type {
  action_unknown = 0,
  /* Set control element value */
  action_alsa_setting,
  /* Delay following rules execution */
  action_outband_execution,
  /* Abort outband execution */
  action_outband_cancellation,
  /* Sleep */
  action_suspend_execution
};

/* Sound card definition */
struct card_def {
  struct card_def *next;
  char *id;
  char *name;
  /* Bugfix, removed
   * int unknown; */
  int num;
  struct elem_def *elem_list;
  struct rule_def *deflt;
};

/* Control element definition */
struct elem_def {
  struct elem_def *next;
  char *ifname;
  char *name;
  int index;
  int dev;
  int subdev;
  int numid;
  struct rule_def *rule;
};

/* Rule definition */
struct rule_def {
  struct rule_def *next;
  enum   action_type action_type;
  int    lineno;
  union {
    /* delay is for suspend and outband rules */
    int delay;
    /* struct is used by alsa_setting rules */
    struct {
      struct card_def *card_def;
      struct elem_def *elem_def;
      struct rule_def *elem_rule;
      char  *value_str;
      long   value;
    };
  };
};

/* Entry definition */
struct entry_def {
  struct entry_def *next;
  const char *name;
  struct rule_def *rules[4];
};

int control_init                (struct options *options);

int control_set_cb              (void);

int control_run_rules_for_entry (enum rule_type rule_type,
                                 const char *entry);

struct card_def *
control_define_card             (const char *id,
                                 const char *name);

struct entry_def *
control_define_entry            (char *entry);

struct elem_def *
control_define_elem             (struct card_def *card_def,
                                 const char *ifname,
                                 const char *name,
                                 int index,
                                 int dev,
                                 int subdev);

struct rule_def *
control_define_rule_alsa_setting(enum rule_type rule_type,
                                 struct entry_def *entry_def,
                                 struct card_def *card_def,
                                 struct elem_def *elem_def,
                                 const char *value,
                                 int lineno);

struct rule_def *
control_define_rule_outband     (enum rule_type rule_type,
                                 struct entry_def *entry_def,
                                 int delay_msec,
                                 int lineno);

struct rule_def *
control_define_rule_suspend     (enum rule_type rule_type,
                                 struct entry_def *entry_def,
                                 int delay_msec,
                                 int lineno);

struct rule_def *
control_define_deflt            (struct card_def *card_def,
                                 struct elem_def *elem_def,
                                 const char *value,
                                 int lineno);


#endif  /* CONTROL_H */
