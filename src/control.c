/**
 * @file control.c
 * @copyright GNU GPLv2 or later
 *
 * ALSA Policy Enforcement control module.
 * These functions serve to handle alsaped rules.
 * The rules are read from config files (see config.c).
 *
 * @{ */

#include <unistd.h>
#include <math.h>
#include <glib.h>

#include "options.h"
#include "logging.h"
#include "alsaif.h"
#include "dbusif.h"

#include "control.h"

/* Private structure */
static struct {
  struct card_def  *card_def_list;
  struct entry_def *entry_def_list;
  int log_rule_execution;
  int outband_src_id;
} priv;


static void alsa_event_cb(alsaif_event *);
static void alsaped_outband_reset();
static int audio_actions_cb(struct action_data *);
static int rule_def_run(struct rule_def *);
static int alsaped_outband_set(int, struct rule_def *);
static int alsaped_suspend(useconds_t, int);
static gboolean alsaped_outband_cb(gpointer);
static struct entry_def *control_find_entry(const char *);
static struct rule_def *rule_def_new();
static struct rule_def *rule_def_add_to_list(struct rule_def *,
					     struct rule_def *);


/**
 * Initialize control options
 * @param options  options parsed from command line
 * @return  always zero
 */
int control_init(struct options *options)
{
  priv.log_rule_execution = options->log_rule_execution;
  return 0;
}

/**
 * Set callbacks for alsaif and dbusif
 * @return  always 0
 */
int control_set_cb()
{
  alsaif_set_cb(alsa_event_cb);
  dbusif_set_cb(audio_actions_cb);
  return 0;
}

/**
 * Add sound card definition read from config file
 * @param id    sound card id
 * @param name  sound card name
 * @return      card_def instance
 */
struct card_def *
control_define_card(const char *id, const char *name)
{
  struct card_def *card_def;
  struct card_def *i;

  if (!id)
    id = "*";

  if (!name)
    name = "*";

  if (!(card_def = malloc(sizeof(*card_def))))
  {
    log_error("%s(): Can't allocate memory: %s", "control_define_card",
              strerror(errno));
    return NULL;
  }

  memset(card_def, 0, sizeof(*card_def));
  card_def->id   = strdup(id);
  card_def->name = strdup(name);
  card_def->num  = -1;

  for (i = (struct card_def *)&priv.card_def_list;  i->next;  i = i->next)
    ;
  i->next = card_def;
  card_def->next = NULL;

  return card_def;
}

/**
 * Add control element definition read from config file
 *
 * @param card_def  sound card definition this element belongs to
 * @param ifname    ALSA interface name
 * @param name      control element name
 * @param index     ALSA index for the control
 * @param dev       ALSA device for the control
 * @param subdev    ALSA sub-device for the control
 *
 * @return  elem_def instance
 */
struct elem_def *
control_define_elem(struct card_def *card_def,
                    const char *ifname,
                    const char *name,
                    int index,
                    int dev,
                    int subdev)
{
  struct elem_def *elem_def;
  struct elem_def *i;

  if (!card_def)
  {
    errno = EINVAL;
    return NULL;
  }

  if (!ifname)
    ifname = "*";

  if (!name)
    name = "*";

  if (!(elem_def = malloc(sizeof(*elem_def))))
  {
    log_error("%s(): Can't allocate memory: %s", "control_define_elem",
              strerror(errno));
    return NULL;
  }

  memset(elem_def, 0, sizeof(*elem_def));
  elem_def->ifname = strdup(ifname);
  elem_def->name   = strdup(name);
  elem_def->index  = index;
  elem_def->dev    = dev;
  elem_def->subdev = subdev;
  elem_def->numid  = -1;

  for (i = (struct elem_def *)&card_def->elem_list;  i->next;  i = i->next)
    ;
  i->next = elem_def;
  elem_def->next = NULL;

  return elem_def;
}

/**
 * Add alsaped rule entry definition read from config file
 * @param name  entry name
 * @return      entry_def instance
 */
struct entry_def *
control_define_entry(char *name)
{
  struct entry_def *entry_def;
  struct entry_def *i;

  if (!name)
  {
    errno = EINVAL;
    return NULL;
  }

  if (control_find_entry(name))
  {
    log_error("Attempt for multiple definition of entry '%s'", name);
    errno = EAGAIN;  /* EAGAIN == Try again */
    return NULL;
  }

  if (!(entry_def = malloc(sizeof(*entry_def))))
  {
    log_error("%s(): Can't allocate memory: %s", "control_define_entry",
              strerror(errno));
    return NULL;
  }

  memset(entry_def, 0, sizeof(*entry_def));
  entry_def->name = strdup(name);
  for (i = (struct entry_def *)&priv.entry_def_list;  i->next;  i = i->next)
    ;
  i->next = entry_def;
  entry_def->next = NULL;

  return entry_def;
}

/**
 * Create new rule definition and add references to entry_def and elem_def.
 * Previous rule of elem_def is stored as rule_def->elem_next_rule.
 * These rules are needed to change ALSA control elements values.
 *
 * @param rule_type  rule type
 * @param entry_def  rule entry
 * @param card_def   sound card
 * @param elem_def   control element
 * @param value      control element value to set
 * @param lineno     config file reference
 *
 * @return  rule_def instance
 */
struct rule_def *
control_define_rule_alsa_setting(enum rule_type rule_type,
                                 struct entry_def *entry_def,
                                 struct card_def *card_def,
                                 struct elem_def *elem_def,
                                 const char *value,
                                 int lineno)
{
  struct rule_def *rule;

  if (!entry_def || !card_def || !elem_def || !value ||
      rule_type == rule_unknown || rule_type >= rule_max)
  {
    errno = EINVAL;
    return NULL;
  }

  rule = rule_def_new();
  if (!rule)
    return NULL;

  rule->lineno = lineno;
  rule->action_type = action_alsa_setting;
  rule->card_def = card_def;
  rule->elem_def = elem_def;
  /* Save the pointer to elem rule to make a chain we will need later
   * in alsaped_update_elem_values() */
  rule->elem_rule = elem_def->rule;
  rule->value_str = strdup(value);

  rule_def_add_to_list((struct rule_def *)&entry_def->rules[rule_type], rule);
  elem_def->rule = rule;

  return rule;
}

/**
 * Create new rule definition
 */
static struct rule_def *
rule_def_new()
{
  struct rule_def *rule = malloc(sizeof(*rule));

  if (rule)
    memset(rule, 0, sizeof(*rule));
  else
    log_error("Can't allocate memory for rule: %s", strerror(errno));

  return rule;
}

/**
 * Add rule definition to the end of list.
 * @return  the list
 */
static struct rule_def *
rule_def_add_to_list(struct rule_def *list,
                     struct rule_def *rule)
{
  struct rule_def *i;

  for (i = list;  i->next;  i = i->next)
    ;
  i->next = rule;

  return list;
}

/**
 * Define outband execution/cancellation for a rule
 *
 * @param rule_type   rule type
 * @param entry_def   entry to add the rule to
 * @param delay_msec  outband delay value
 * @param lineno      config file reference
 *
 * @return  rule_def instance for the rule
 */
struct rule_def *
control_define_rule_outband(enum rule_type rule_type,
                            struct entry_def *entry_def,
                            int delay_msec,
                            int lineno)
{
  enum action_type action_type;
  struct rule_def *rule;

  if (delay_msec > 3000 ||
      rule_type == rule_unknown ||
      rule_type >= rule_max)
  {
    errno = EINVAL;
    return NULL;
  }

  if (delay_msec == -1)
  {
    action_type = action_outband_cancellation;
    delay_msec = 0;
  }
  else
  {
    action_type = action_outband_execution;
  }

  rule = rule_def_new();

  if (!rule)
    return NULL;

  rule->lineno = lineno;
  rule->action_type = action_type;
  /* Delay is in milliseconds for outband and useconds for suspend */
  rule->delay = delay_msec;
  rule_def_add_to_list((struct rule_def *)&entry_def->rules[rule_type], rule);

  return rule;
}

/**
 * Define suspend rule
 *
 * @param rule_type   rule type
 * @param entry_def   entry to add the rule to
 * @param delay_msec  outband delay value
 * @param lineno      config file reference
 *
 * @return  rule_def instance for the rule
 */
struct rule_def *
control_define_rule_suspend(enum rule_type rule_type,
                            struct entry_def *entry_def,
                            int delay_msec,
                            int lineno)
{
  struct rule_def *rule;

  if (delay_msec > 500 || rule_type == rule_unknown || rule_type >= rule_max)
  {
    errno = EINVAL;
    return NULL;
  }

  rule = rule_def_new();
  if (!rule)
    return NULL;

  rule->lineno = lineno;
  rule->action_type = action_suspend_execution;
  /* Delay is in useconds for suspend and milliseconds for outband */
  rule->delay = 1000 * delay_msec;
  rule_def_add_to_list((struct rule_def *)&entry_def->rules[rule_type], rule);

  return rule;
}

/**
 * Define default value for sound card element
 *
 * @param card_def  sound card definition
 * @param elem_def  control element definition
 * @param value     default value
 * @param lineno    config file reference
 *
 * @return  rule_def instance
 */
struct rule_def *
control_define_deflt(struct card_def *card_def,
                     struct elem_def *elem_def,
                     const char *value,
                     int lineno)
{
  struct rule_def *rule;
  struct rule_def *i;

  if (!card_def || !elem_def || !value)
  {
    errno = EINVAL;
    return NULL;
  }

  rule = rule_def_new();
  if (!rule)
  {
    log_error("%s(): Can't allocate memory: %s", "control_define_deflt",
              strerror(errno));
    return NULL;
  }

  rule->lineno = lineno;
  rule->action_type = action_alsa_setting;
  rule->card_def = card_def;
  rule->elem_def = elem_def;
  rule->elem_rule = elem_def->rule;
  rule->value_str = strdup(value);

  for (i = (struct rule_def *)&card_def->deflt;  i->next;  i = i->next)
    ;
  i->next = rule;
  elem_def->rule = rule;

  return rule;
}

int
control_run_rules_for_entry(enum rule_type rule_type,
                            const char *entry_name)
{
  struct entry_def *entry;

  if (rule_type == rule_unknown ||
      rule_type >= rule_max)
  {
    errno = EINVAL;
    return -1;
  }

  entry = control_find_entry(entry_name);

  return entry ? rule_def_run(entry->rules[rule_type]) : 0;
}

static int
alsaped_run_context_rules(const char *context)
{
  struct entry_def *entry = control_find_entry(context);
  return entry ? rule_def_run(entry->rules[rule_context]) : 0;
}

/**
 * Find sound card definition for ALSA card number
 * @param num  card number
 * @return     card_def instance or NULL
 */
struct card_def *
card_def_find_by_num(int num)
{
  struct card_def *card;

  if (num >= 0)
  {
    for (card = priv.card_def_list;  card;  card = card->next)
    {
      if (card->num == num)
        return card;
    }
  }

  return NULL;
}

/**
 * Find control element definition
 * @param numid  control element id number
 * @return       elem_def instance or NULL
 */
struct elem_def *
card_def_find_ctl_elem(struct card_def *card, int numid)
{
  struct elem_def *ctl_elem;

  if (card && numid != -1)
  {
    for (ctl_elem = card->elem_list;  ctl_elem;  ctl_elem = ctl_elem->next)
    {
      if (ctl_elem->numid == numid)
        return ctl_elem;
    }
  }

  return NULL;
}

/**
 * Find rules entry definition instance
 * @param name  entry name
 * @return      entry_def instance or NULL
 */
static struct entry_def *
control_find_entry(const char *name)
{
  struct entry_def *entry;

  if (!name)
    return NULL;

  for (entry = priv.entry_def_list;  entry;  entry = entry->next)
  {
    if (!strcmp(entry->name, name))
      return entry;
  }

  return NULL;
}

/* How many should be added to match the step? */
static inline int
step_correction(int value, int step)
{
  return (step - value) % step;
}

/* This function was mostly rewritten */
static long long
get_value_from_percent(long long percent, int min, int max, int step)
{
  long long value_int;
  double value_float;

  if (percent > 99)
    return max;

  if (percent <= 0)
    return min;

  value_float = min + percent / 100.0 * (max - min);

  if (step)
  {
    value_int = floor(value_float);
    value_int += step_correction(value_int - min, step);
  }
  else
  {
    value_int = ceil(value_float);
  }

  return value_int <= max ? value_int : max;
}

/**
 * Update control element values from their string representation.
 *
 * @param alsaped_elem  ALSA control element to update value for
 * @param content_type  type of control element (int, bool, enum)
 * @param descriptor    value descriptor needed for conversion
 */
static void
alsaped_update_elem_values(struct elem_def     *alsaped_elem,
                           snd_ctl_elem_type_t  content_type,
                           value_descriptor    *descriptor)
{
  struct rule_def *rule;
  long long value_int;
  char **enum_names;
  int enum_count, i;
  char *value_str;
  char *end_ptr;

  for (rule = alsaped_elem->rule; rule; rule = rule->elem_rule)
  {
    value_str = rule->value_str;

    switch (content_type)
    {
      case SND_CTL_ELEM_TYPE_INTEGER:
        value_int = strtoll(value_str, &end_ptr, 10);
        if (end_ptr > value_str &&
            (*end_ptr == '\0' || !strcmp(end_ptr, "U") || !strcmp(end_ptr, "%")))
        {
          int min  = descriptor->int_t.min;
          int max  = descriptor->int_t.max;
          int step = descriptor->int_t.step;

          if (*end_ptr == '%')
            value_int = get_value_from_percent(value_int, min, max, step);

          if (value_int < min || value_int > max)
          {
            log_error("Value %lld is out of range (%ld - %ld) in line %d",
                      value_int, min, max, rule->lineno);
          }
          else if (step && step_correction(value_int - min, step))
          {
            log_error("Value %lld is out of range (%ld - %ld, step %ld) in line %d",
                      value_int, min, max, step, rule->lineno);
          }
          else
          {
            rule->value = value_int;
          }
        }
        else
        {
          log_error("Invalid integer value '%s' in line %d", value_str, rule->lineno);
        }
        break;

      case SND_CTL_ELEM_TYPE_ENUMERATED:
        enum_count = descriptor->enum_t.count;
        enum_names = descriptor->enum_t.names;

        for (i = 0;  i < enum_count;  ++i)
          if (!strcmp(value_str, enum_names[i]))
          {
            rule->value = i;
            break;
          }

        if (i >= enum_count)
        {
          log_error("Invalid enumeration value '%s' in line %d", value_str, rule->lineno);
          log_error("The possible values are:");
          for (i = 0;  i < enum_count;  ++i)
            log_error("  '%s'", enum_names[i]);
        }

        break;

      case SND_CTL_ELEM_TYPE_BOOLEAN:
        if (!strcasecmp(value_str, "true") || !strcasecmp(value_str, "yes") || !strcasecmp(value_str, "on"))
          rule->value = 1;
        else if (!strcasecmp(value_str, "false") || !strcasecmp(value_str, "no") || !strcasecmp(value_str, "off"))
          rule->value = 0;
        else
          log_error("Invalid boolean value string '%s' in line %d", value_str, rule->lineno);
        break;

      /* Prevent compiler warning */
      default:
        break;
    }
  }
}

/**
 * Set sound card controls to default values.
 * @param card  sound card definition instance pointer
 * @return      0 on success, -1 on error
 */
int
card_def_set_defaults(struct card_def *card)
{
  if (!card)
    return -1;

  return rule_def_run(card->deflt);
}

/**
 * Execute actions of a rule.
 * @param rule  the rule
 * @return      0 on succes, -1 on error
 */
static int
rule_def_run(struct rule_def *rule)
{
  int retval = 0;

  for ( ; rule;  rule = rule->next)
  {
    switch (rule->action_type)
    {
      case action_alsa_setting:
        if (alsaif_set_value(rule->card_def->num,
                             rule->elem_def->numid,
                             &rule->value) < 0)
          retval = -1;
        continue;

      case action_outband_execution:
        return alsaped_outband_set(rule->delay, rule->next);

      case action_outband_cancellation:
        alsaped_outband_reset();
        break;

      case action_suspend_execution:
        retval = alsaped_suspend(rule->delay, rule->lineno);
        break;

      default:
        log_error("Invalid rule action in line %d", rule->lineno);
    }
  }

  return retval;
}

/**
 * Execute outband execution rule. It's similar to suspend, but can be
 * cancelled and scheduled on idle.
 *
 * @param msec  Execute the rule after delay in milliseconds; execute on idle
 *              if this value is zero.
 * @param rule  The rule we are delaying.
 *
 * @param       0 if success, -1 on error
 */
static int
alsaped_outband_set(int msec, struct rule_def *rule)
{
  if (priv.outband_src_id)
    return -1;

  if (!rule)
    return 0;

  if (priv.log_rule_execution)
    log_info("set outband execution (line %d, delay %umsec)", rule->lineno, msec);

  if (msec)
    priv.outband_src_id = g_timeout_add(msec, alsaped_outband_cb, rule);
  else
    priv.outband_src_id = g_idle_add(alsaped_outband_cb, rule);

  return priv.outband_src_id ? 0 : -1;
}

/**
 * Remove outband execution from schedule
 */
static void
alsaped_outband_reset()
{
  if (priv.outband_src_id)
  {
    if (priv.log_rule_execution)
      log_info("remove outband execution");

    if (!g_source_remove(priv.outband_src_id))
      log_error("Failed to cancel outband execution");

    priv.outband_src_id = 0;
  }
}

/**
 * Delay rules execution.
 * @param usec    delay time in microseconds
 * @param lineno  config file line number of this rule
 * @return        0 on success, -1 on error
 */
static int
alsaped_suspend(useconds_t usec, int lineno)
{
  int result;

  if (priv.log_rule_execution)
    log_info("suspend execution for %u msec (line %d)", usec / 1000, lineno);

  do
    result = usleep(usec);
  while (result < 0 && errno == EINTR);  /* EINTR == Interrupted system call */

  if (result >= 0)
  {
    if (priv.log_rule_execution)
      log_info("resuming execution (line %d)", lineno);
  }
  else
  {
    log_error("execution suspension failed (line %d)", lineno);
  }

  return result;
}

/**
 * alsaif events handler callback. These events are generated by alsaped.
 * Possible events:
 * 1. EVENT_SOUNDCARD_ADDED
 * 2. EVENT_CONTROLS_ADDED
 * 3. EVENT_CTL_ELEM_ADDED
 *
 * @param event  an event to handle
 */
static void
alsa_event_cb(alsaif_event *event)
{
  struct card_def *card_def;
  struct elem_def *elem_def;

  if (!event)
    return;

  switch (event->type)
  {
    case EVENT_SOUNDCARD_ADDED:
      /*
       * Sound card added.
       *
       * Initialize card_def->num
       */

      card_def = card_def_find_by_num(event->card.num);
      if (card_def)
      {
        if (strcmp(event->card.id, card_def->id) ||
            strcmp(event->card.name, card_def->name))
        {
          log_error("%s(): confused by adding multiple cards", "alsa_event_cb");
        }
        return;
      }

      for (card_def = priv.card_def_list;  card_def;  card_def = card_def->next)
      {
        if ((!strcmp(card_def->id, "*") || !strcmp(card_def->id, event->card.id)) &&
            (!strcmp(card_def->name, "*") || !strcmp(card_def->name, event->card.name)))
        {
          card_def->num = event->card.num;
          return;
        }
      }

      return;

    case EVENT_CONTROLS_ADDED:
      /*
       * All control elements were added.
       *
       * Set default values from card_def->deflt
       */

      card_def = card_def_find_by_num(event->card.num);
      if (card_def)
        card_def_set_defaults(card_def);
      else
        log_error(
          "%s(): can't find card %d for incoming event",
          "alsa_event_cb",
          event->card.num);

      return;

    case EVENT_CTL_ELEM_ADDED:
      /*
       * Single control element added.
       *
       * 1. Initialize elem_def->numid
       *
       * 2. Initialize rule->value from rule->s_value according to read
       *    value descriptor
       */

      card_def = card_def_find_by_num(event->elem.card_num);
      if (!card_def)
        return;

      elem_def = card_def_find_ctl_elem(card_def, event->elem.numid);
      if (elem_def)
      {
        if (strcmp(event->elem.ifname, elem_def->ifname) ||
            strcmp(event->elem.name, elem_def->name) ||
            event->elem.index != elem_def->index ||
            event->elem.dev != elem_def->dev ||
            event->elem.subdev != elem_def->subdev)
        {
          log_error("%s(): confused by adding multiple elems", "alsa_event_cb");
        }
        return;
      }

      for (elem_def = card_def->elem_list;  elem_def;  elem_def = elem_def->next)
      {
        if ((!strcmp(elem_def->ifname, "*") ||
             !strcmp(elem_def->ifname, event->elem.ifname)) &&
            (!strcmp(elem_def->name, "*") ||
             !strcmp(elem_def->name, event->elem.name)) &&
            (elem_def->index == -1 || elem_def->index == event->elem.index) &&
            (elem_def->dev == -1 || elem_def->dev == event->elem.dev) &&
            (elem_def->subdev == -1 || elem_def->subdev == event->elem.subdev))
        {
          snd_ctl_elem_type_t content_type;
          value_descriptor   *descriptor;

          content_type = alsaif_get_value_descriptor(event->elem.card_num,
                                                     event->elem.numid,
                                                     &descriptor);
          if (content_type)
          {
            elem_def->numid = event->elem.numid;
            alsaped_update_elem_values(elem_def, content_type, descriptor);
          }
          else
          {
            log_error("Can't get value descriptor for hw:%d,%d",
                      event->elem.card_num,
                      event->elem.numid);
          }
          return;
        }
      }

      return;

    default:
      log_error("%s(): unknown event type %d received", "alsa_event_cb", event->type);
  }
}

/*
 * D-Bus interface events callback.
 * @param data  action data
 * @return      0 on success, -1 on error
 */
static int
audio_actions_cb(struct action_data *data)
{
  static char *source_route = NULL;
  static char *sink_route   = NULL;

#define CONTEXT_LENGTH 256
  char context[CONTEXT_LENGTH];
  char *route_device;

  switch (data->rule_type)
  {
    case rule_source:
      route_device = data->route_dev;
      if (source_route && !strcmp(route_device, source_route))
      {
        log_info("Ignoring source route to '%s'. Route already in use.",
                 route_device);
        return 0;
      }

      free(source_route);
      source_route = strdup(route_device);
      log_info("Routing source to '%s'", route_device);

      return control_run_rules_for_entry(rule_source, route_device);

    case rule_sink:
      route_device = data->route_dev;
      if (sink_route && !strcmp(route_device, sink_route))
      {
        log_info("Ignoring sink route to '%s'. Route already in use.",
                 route_device);
        return 0;
      }

      free(sink_route);
      sink_route = strdup(route_device);
      log_info("Routing sink to '%s'", route_device);

      return control_run_rules_for_entry(rule_sink, route_device);

    case rule_context:
      snprintf(context, CONTEXT_LENGTH, "%s-%s",
               data->variable,
               data->value);
      log_info("Setting context '%s'", context);

      return alsaped_run_context_rules(context);

    default:
      log_error("Invalid routing type %d", data->rule_type);
      return -1;
  }
}

/* Outband rule execution callback.
 * @param rule  The rule to be executed by callback
 * @return      Always FALSE / G_SOURCE_REMOVE */
static gboolean
alsaped_outband_cb(gpointer data)
{
  struct rule_def *rule = data;
  priv.outband_src_id = 0;

  if (rule_def_run(rule) >= 0)
  {
    if (priv.log_rule_execution)
      log_info("outband execution of rules succeded (line %d)", rule->lineno);
  }
  else
  {
    log_error("outband execution of rules failed (line %d)", rule->lineno);
  }

  return G_SOURCE_REMOVE;
}

/** @} */
