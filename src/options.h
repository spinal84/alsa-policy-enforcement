#ifndef OPTIONS_H
#define OPTIONS_H

#include <sys/types.h>

struct dbusif_options {
  char *ifname;
  char *mypath;
  char *pdpath;
  char *pdname;
  int   log;
};

struct alsaif_options {
  int log_info;
  int log_ctl;
  int log_val;
};

struct options {
  int   daemon;
  int   interact;
  uid_t uid;
  char *work_dir;
  int   rt_prio;
  char *config_path;
  int   log_parsed_rules;
  int   log_rule_execution;
  int   list_and_exit;
  int   log_mask;
  struct dbusif_options dbusif;
  struct alsaif_options alsaif;
};


#endif  /* OPTIONS_H */
