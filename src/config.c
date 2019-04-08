#include <glib.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>

#include "control.h"
#include "logging.h"

#include "config.h"

enum section_type {
  section_unknown = 0,
  /* [control] */
  section_control,
  /* [sink-route] */
  section_sink,
  /* [source-route] */
  section_source,
  /* [context] */
  section_context,
  /* [default] */
  section_default,
  section_max
};

/* Parsed control element definition */
struct elemdef {
  char *id;
  char *card;
  char *iface;
  char *name;
  unsigned int index;
  unsigned int dev;
  unsigned int subdev;
};

/* Parsed rule definition */
struct ruldef {
  enum action_type action;
  char *entry;
  union {
    struct {
      char *elemid;
      char *value;
    };
    unsigned int delay;
  };
};

/* Parsed section entry */
struct section {
  enum section_type type;
  int  lineno;
  union {
    void           *any;
    struct elemdef *elem;
    struct ruldef  *rule;
  } def;
};

/* Element of sound cards definitions table */
struct cardtbl_elem {
  char *id;
  char *name;
  struct card_def *card;
};

/* Element of control elements definitions table */
struct elemtbl_elem {
  char *id;
  struct card_def *card;
  struct elem_def *elem;
};

/* Element of entrytbl table */
struct entrytbl_elem {
  char *name;
  struct entry_def *entry;
};

/* Table of sound cards definitions */
struct cardtbl {
  int len;
  struct cardtbl_elem **array;
};

/* Table of control elements definitions */
struct elemtbl {
  int len;
  struct elemtbl_elem **array;
};

/* Table of rule entries from config file */
struct entrytbl {
  int len;
  struct entrytbl_elem **array;
};

/* Private structure */
static struct {
  char *path;
  int log_parsed_rules;
  struct cardtbl *cardtbl;
  struct elemtbl *elemtbl;
  struct entrytbl *entrytbl;
} priv;


static int preprocess_buffer         (int, char *, char *);
static int section_header            (int, char *, enum section_type *);
static int section_open              (enum section_type, struct section *);
static int section_close             (struct section *);
static int elemdef_parse             (int, char *, struct elemdef *);
static int ruldef_parse_outband      (int, char *, struct ruldef *);
static int ruldef_parse_suspend      (int, char *line, struct ruldef *);
static int ruldef_parse_alsa_setting (int, char *, struct ruldef *);
static int ruldef_parse_deflt        (int, char *, struct ruldef *);
static int create_rule_outband       (enum section_type, struct ruldef *, int);
static int create_rule_suspend       (enum section_type, struct ruldef *, int);
static int create_rule_alsa_setting  (enum section_type, struct ruldef *, int);
static int create_deflt              (struct ruldef *, int);
static int create_elem               (struct elemdef *);
static int num_parse                 (int, const char *, unsigned int *);
static int valid_entry               (int, char *);

static int elemtbl_add_elem          (struct elemtbl *, char *,
                                      struct card_def *, char *,
                                      char *, int, int, int);

static struct cardtbl   *cardtbl_create       (void);
static struct elemtbl   *elemtbl_create       (void);
static struct entrytbl  *entrytbl_create      (void);
static struct cardtbl   *cardtbl_free         (struct cardtbl *);
static struct elemtbl   *elemtbl_free         (struct elemtbl *);
static struct entrytbl  *entrytbl_free        (struct entrytbl *);
static struct elemdef   *elemdef_create       (void);
static struct ruldef    *ruldef_create        (void);
static struct elemdef   *elemdef_free         (struct elemdef *);
static struct ruldef    *ruldef_free          (struct ruldef *);
static struct entry_def *entrytbl_get_entry   (struct entrytbl *, char *);

static struct card_def  *cardtbl_get_card_def (struct cardtbl *,
                                               char *, char *);

static struct elem_def  *elemtbl_find_by_id   (struct elemtbl *, char *,
                                               struct card_def **);

/**
 * Initialize config options
 * @param options  Options parsed from command line
 * @return  0 if success, -1 if error
 */
int config_init(struct options *options)
{
  if (!options->config_path)
  {
    errno = EINVAL;  /* Invalid argument */
    return -1;
  }

  priv.path = strdup(options->config_path);

  if (!priv.path)
    return -1;

  priv.log_parsed_rules = options->log_parsed_rules;
  return 0;
}

/**
 * Parse config file and create rules
 * @return  0 if success, -1 otherwise
 */
int config_parse()
{
#define BUFSIZE 512
  FILE *f;
  struct section section;
  char buf[BUFSIZE];
  char line[BUFSIZE];
  int lineno, ret;
  enum section_type newsect;
  struct elemdef *elemdef;
  struct ruldef  *rule;
  int status = 0;

  if (!(f = fopen(priv.path, "r")))
  {
    log_error("Can't open config file '%s': %s", priv.path, strerror(errno));
    return -1;
  }

  priv.cardtbl = cardtbl_create();
  priv.elemtbl = elemtbl_create();
  priv.entrytbl = entrytbl_create();

  memset(&section, 0, sizeof(section));

  for (errno = 0, lineno = 1;  fgets(buf, BUFSIZE, f);  lineno++)
  {
    if (preprocess_buffer(lineno, buf, line) < 0)
      break;

    if (*line == '\0')
      continue;

    if (section_header(lineno, line, &newsect))
    {
      if (section_close(&section) < 0)
        status = -1;

      section.type = newsect;
      section.lineno = lineno;

      if (section_open(newsect, &section) < 0)
        status = -1;

      continue;
    }

    switch (section.type)
    {
      case section_control:
        elemdef = section.def.elem;

        if (elemdef_parse(lineno, line, elemdef) < 0)
          status = -1;
        break;
      case section_sink:
      case section_source:
      case section_context:
        rule = section.def.rule;

        if ((ret = ruldef_parse_outband(lineno, line, rule)) < 0)
          goto invalid;

        if (ret == 0)
        {
          if (create_rule_outband(section.type, rule, lineno) < 0)
            goto invalid;

          break;
        }

        if ((ret = ruldef_parse_suspend(lineno, line, rule)) < 0)
          goto invalid;

        if (ret == 0)
        {
          if (create_rule_suspend(section.type, rule, lineno) < 0)
            goto invalid;

          break;
        }

        if ((ret = ruldef_parse_alsa_setting(lineno, line, rule)) == 0 &&
            create_rule_alsa_setting(section.type, rule, lineno) >= 0)
        {
            break;
        }

invalid:
        status = -1;
        buf[strcspn(buf, "\n")] = 0;
        log_error("Invalid definition '%s' in line %d", buf, lineno);
        break;
      case section_default:
        if (ruldef_parse_deflt(lineno, line, section.def.rule) < 0 ||
            create_deflt(section.def.rule, section.lineno) < 0)
        {
          status = -1;
        }
        break;
      default:
        break;
    }
  }
  section_close(&section);
  priv.cardtbl = cardtbl_free(priv.cardtbl);
  priv.elemtbl = elemtbl_free(priv.elemtbl);
  priv.entrytbl = entrytbl_free(priv.entrytbl);
  if (errno)
  {
    status = -1;
    log_error("Error during read of '%s': %s", priv.path, strerror(errno));
  }
  fclose(f);
  return status;
}

/**
 * Remove excessive blanks and omit comments
 *
 * @param lineno  config file line number
 * @param inbuf   line read from config
 * @param outbuf  buffer to store the result
 *
 * @return        0 on success, -1 on error (errno is set)
 */
static int
preprocess_buffer(int lineno, char *inbuf, char *outbuf)
{
  char c;
  int quote;
  int status = 0;

  for (quote = 0;  *inbuf;  inbuf++)
  {
    c = *inbuf;

    if (!quote && isblank(c))
      continue;

    if (c == '\n' || (!quote && c == '#'))
      break;

    if (c == '"')
    {
      quote ^= 1;
      continue;
    }

    if (c < 0x20)
    {
      log_error("Illegal character 0x%02x in line %d", c, lineno);
      status = -1;
      errno = EILSEQ;  /* Illegal byte sequence */
      break;
    }

    *outbuf++ = c;
  }

  *outbuf = 0;

  if (quote)
    log_warning("unterminated quoted string '%s' in line %d", inbuf, lineno);

  return status;
}

/**
 * Check if current line is a section header
 *
 * @param lineno  config file line number
 * @param line    line from config file
 * @param type    pointer to return section type
 *
 * @return  1 if line is a header of section, 0 otherwise
 */
static int
section_header(int lineno, char *line, enum section_type *type)
{
  if (line[0] != '[')
    return 0;

  if (!strcmp(line, "[control]"))
    *type = section_control;
  else if (!strcmp(line, "[sink-route]"))
    *type = section_sink;
  else if (!strcmp(line, "[source-route]"))
    *type = section_source;
  else if (!strcmp(line, "[context]"))
    *type = section_context;
  else if (!strcmp(line, "[default]"))
    *type = section_default;
  else
  {
    *type = section_unknown;
    log_error("Invalid section type '%s' in line %d", line, lineno);
  }

  return 1;
}

/**
 * Allocate section structure
 *
 * @param type  section type
 * @param sec   section structure
 *
 * @return  -1 if error, 0 if OK
 */
static int
section_open(enum section_type type, struct section *sec)
{
  int status;

  if (!sec)
    return -1;

  switch (type)
  {
    case section_control:
      sec->def.elem = elemdef_create();
      status = 0;
      break;

    case section_sink:
    case section_source:
    case section_context:
    case section_default:
      sec->def.rule = ruldef_create();
      status = 0;
      break;

    default:
      type = section_unknown;
      sec->def.any = NULL;
      status = -1;
  }

  sec->type = type;

  return status;
}

/**
 * Free allocated memory and add control element to daemon if needed
 * @param sec  current section structure instance
 * @return     0 if success, -1 if error
 */
static int
section_close(struct section *sec)
{
  int status;

  if (!sec)
    return -1;

  switch(sec->type)
  {
    case section_control:
      status = create_elem(sec->def.elem);
      sec->def.elem = elemdef_free(sec->def.elem);
      break;
    case section_unknown:
      status = 0;
      break;
    default:
      status = 0;
      sec->def.rule = ruldef_free(sec->def.rule);
  }

  sec->type = section_unknown;
  sec->def.any = NULL;

  return status;
}

/**
 * Add parsed control element to daemon
 * @param elemdef  parsed control element definition
 * @return  0 on success, -1 on error
 */
static int
create_elem(struct elemdef *elemdef)
{
  struct card_def *card_def;
  int status;

  if (priv.log_parsed_rules)
  {
    log_info(
        "Create elem: id='%s' card='%s', ifname='%s', name='%s' index=%u device=%u, subdev=%u",
        elemdef->id    ? elemdef->id    : "<null>",
        elemdef->card  ? elemdef->card  : "<null>",
        elemdef->iface ? elemdef->iface : "<null>",
        elemdef->name  ? elemdef->name  : "<null>",
        elemdef->index, elemdef->dev, elemdef->subdev);
  }

  if ((card_def = cardtbl_get_card_def(priv.cardtbl, NULL, elemdef->card)))
  {
    status = elemtbl_add_elem(
        priv.elemtbl, elemdef->id, card_def, elemdef->iface, elemdef->name,
        elemdef->index, elemdef->dev, elemdef->subdev);
  }
  else
    status = -1;

  return status;
}

/**
 * Add parsed set value rule to daemon
 *
 * @param section  section type
 * @param rule     rule definition parsed from config file
 * @param lineno   config file reference
 *
 * @return         0 if success, -1 if error
 */
static int
create_rule_alsa_setting(enum section_type section,
                         struct ruldef *rule,
                         int lineno)
{
  enum rule_type rule_type;
  const char *rule_type_str;
  struct entry_def *entry_def;
  struct card_def *card_def;
  struct elem_def *elem_def;
  int status;

  switch (section)
  {
    case section_sink:
      rule_type = rule_sink;
      rule_type_str = "sink-route";
      break;
    case section_source:
      rule_type = rule_source;
      rule_type_str = "source-route";
      break;
    case section_context:
      rule_type = rule_context;
      rule_type_str = "context";
      break;
    default:
      rule_type = rule_unknown;
      rule_type_str = "<unknown>";
      break;
  }

  if (priv.log_parsed_rules)
  {
    log_info(
        "Create rule: type=%s entry='%s' action=alsa_setting, elemid='%s' value='%s'",
        rule_type_str,
        rule->entry  ? rule->entry  : "<null>",
        rule->elemid ? rule->elemid : "<null>",
        rule->value  ? rule->value  : "<null>");
  }

  if (rule_type == rule_unknown ||
      !(entry_def = entrytbl_get_entry(priv.entrytbl, rule->entry)) ||
      !(elem_def = elemtbl_find_by_id(priv.elemtbl, rule->elemid, &card_def)) ||
      !control_define_rule_alsa_setting(rule_type, entry_def, card_def,
                                        elem_def, rule->value, lineno))
  {
    return -1;
  }

  return 0;
}

/**
 * Add outband execution rule to daemon
 *
 * @param section  section type
 * @param rule     rule definition parsed from config
 * @param lineno   config file reference
 *
 * @return         -1 if error, 0 if success
 */
static int
create_rule_outband(enum section_type section,
                    struct ruldef *rule,
                    int lineno)
{
  enum rule_type rule_type;
  const char *rule_type_str;
  struct entry_def *entry_def;
  int status;

  switch (section)
  {
    case section_sink:
      rule_type = rule_sink;
      rule_type_str = "sink-route";
      break;
    case section_source:
      rule_type = rule_source;
      rule_type_str = "source-route";
      break;
    case section_context:
      rule_type = rule_context;
      rule_type_str = "context";
      break;
    default:
      rule_type = rule_unknown;
      rule_type_str = "<unknown>";
      break;
  }

  if (rule->action == action_outband_execution ||
      rule->action == action_outband_cancellation)
  {
    if (priv.log_parsed_rules)
    {
      char *entry = rule->entry ? rule->entry : "<null>";

      if (rule->action == action_outband_execution)
      {
        log_info(
          "Create rule: type=%s entry='%s' action=outband_execution delay=%dmsec",
          rule_type_str, entry, rule->delay);
      }
      else
      {
        log_info(
          "Create rule: type=%s entry='%s' action=outband_cancellation",
          rule_type_str, entry);
      }
    }
  }
  else
    rule_type = rule_unknown;

  if (rule_type == rule_unknown ||
      !(entry_def = entrytbl_get_entry(priv.entrytbl, rule->entry)) ||
      !control_define_rule_outband(rule_type, entry_def, rule->delay, lineno))
  {
    return -1;
  }

  return 0;
}

/**
 * Add suspend execution rule to daemon
 *
 * @param section  section type
 * @param rule     rule definition parsed from config
 * @param lineno   config file reference
 *
 * @return         -1 if error, 0 if success
 */
static int
create_rule_suspend(enum section_type section,
                    struct ruldef *rule,
                    int lineno)
{
  struct entry_def *entry_def;
  enum rule_type rule_type;
  const char *rule_type_str;
  int status;

  switch (section)
  {
    case section_sink:
      rule_type = rule_sink;
      rule_type_str = "sink-route";
      break;
    case section_source:
      rule_type = rule_source;
      rule_type_str = "source-route";
      break;
    case section_context:
      rule_type = rule_context;
      rule_type_str = "context";
      break;
    default:
      rule_type = rule_unknown;
      rule_type_str = "<unknown>";
      break;
  }

  if (priv.log_parsed_rules)
  {
    log_info(
      "Create rule: type=%s entry='%s' action=suspend_execution sleep=%dmsec",
      rule_type_str,
      rule->entry ? rule->entry : "<null>",
      rule->delay);
  }

  if (rule_type == rule_unknown ||
      !(entry_def = entrytbl_get_entry(priv.entrytbl, rule->entry)) ||
      !control_define_rule_suspend(rule_type, entry_def, rule->delay, lineno))
  {
    return -1;
  }

  return 0;
}

/**
 * Add default settings parsed from config file to daemon
 * @param rule    rule definition parsed from config file
 * @param lineno  config file reference
 * @return        -1 if error, 0 if success
 */
static int
create_deflt(struct ruldef *rule, int lineno)
{
  struct card_def *card_def;
  struct elem_def *elem_def;

  if (priv.log_parsed_rules)
  {
    log_info("Create deflt: elemid='%s' value='%s'",
        rule->elemid ? rule->elemid : "<null>",
        rule->value  ? rule->value  : "<null>");
  }

  elem_def = elemtbl_find_by_id(priv.elemtbl, rule->elemid, &card_def);

  if (!elem_def)
    return -1;

  if (control_define_deflt(card_def, elem_def, rule->value, lineno))
    return 0;

  return -1;
}

/**
 * Allocate, initialize and return struct elemdef.
 * Abort program if memory allocation fails.
 *
 * @return  pointer to allocated elemdef
 */
static struct elemdef *
elemdef_create()
{
  struct elemdef *elemdef;

  if (!(elemdef = malloc(sizeof(*elemdef))))
  {
    log_error("%s(): memory allocation error", "elemdef_create");
    exit(errno);
  }

  memset(elemdef, 0, sizeof(*elemdef));

  elemdef->index = -1;
  elemdef->dev = -1;
  elemdef->subdev = -1;

  return elemdef;
}

/**
 * Free allocated elemdef structure
 * @param elemdef  elemdef structure
 * @return  NULL
 */
static struct elemdef *
elemdef_free(struct elemdef *elemdef)
{
  if (elemdef)
  {
    free(elemdef->id);
    free(elemdef->card);
    free(elemdef->iface);
    free(elemdef->name);
    free(elemdef);
  }

  return NULL;
}

/**
 * Parse config file section [control]
 *
 * @param lineno   config file reference
 * @param line     string to parse
 * @param elemdef  pointer to store the result
 *
 * @return         -1 on error, 0 if success
 */
static int
elemdef_parse(int lineno, char *line, struct elemdef *elemdef)
{
  int status = 0;
  char *end;

  if (!elemdef)
    return -1;

  if (!strncmp(line, "id=", 3))
  {
    elemdef->id = strdup(line + 3);
    if (!elemdef->id)
      status = -1;
  }
  else if (!strncmp(line, "card=", 5))
  {
    elemdef->card = strdup(line + 5);
    if (!elemdef->card)
      status = -1;
  }
  else if (!strncmp(line, "interface=", 10))
  {
    elemdef->iface = strdup(line + 10);
    if (!elemdef->iface)
      status = -1;
  }
  else if (!strncmp(line, "name=", 5))
  {
    elemdef->name = strdup(line + 5);
    if (!elemdef->name)
      status = -1;
  }
  else if (!strncmp(line, "index=", 6))
    status = num_parse(lineno, line + 6, &elemdef->index) < 0;
  else if (!strncmp(line, "device=", 7))
    status = num_parse(lineno, line + 7, &elemdef->dev) < 0;
  else if (!strncmp(line, "sub-device=", 11))
    status = num_parse(lineno, line + 11, &elemdef->subdev) < 0;
  else
  {
    status = -1;

    if (!(end = strchr(line, '=')))
      log_error("Invalid definition '%s' in line %d", line, lineno);
    else
    {
      *end = '\0';
      log_error("Invalid key value '%s' in line %d", line, lineno);
    }
  }

  return status;
}

/**
 * Get number from string
 *
 * @param lineno  config file reference
 * @param line    string to parse
 * @param num     pointer to store the result
 *
 * @return        0 if success, -1 if number is invalid
 */
static int
num_parse(int lineno, const char *line, unsigned int *num)
{
  char *end;

  *num = strtoul(line, &end, 10);
  if (end != line && end[0] == '\0')
    return 0;

  log_error("Invalid number '%s' in line %d", line, lineno);
  return -1;
}

/**
 * Allocate, initialize with zeros and return struct ruldef.
 * Abort program if memory allocation fails.
 *
 * @return  pointer to allocated ruldef
 */
static struct ruldef *
ruldef_create()
{
  struct ruldef *rule;

  if (!(rule = malloc(sizeof(*rule))))
  {
    log_error("%s(): memory allocation error", "ruldef_create");
    exit(errno);
  }

  memset(rule, 0, sizeof(*rule));

  return rule;
}

/**
 * Free allocated ruldef struct
 * @param ruldef  allocated struct pointer
 * @return  NULL
 */
static struct ruldef *
ruldef_free(struct ruldef *rule)
{
  free(rule);
  return NULL;
}

/**
 * Parse ALSA setting rule definition
 *
 * @param lineno  reference to config file
 * @param line    string to parse
 * @param rule    pointer to store the result
 *
 * @return        -1 if error, 0 if OK, 1 if skipped
 */
static int
ruldef_parse_alsa_setting(int lineno, char *line, struct ruldef *rule)
{
  char *equal;
  char *colon;

  /*
   * Line example:
   * entry=line-pga-bypass-volume:0%
   */

  if (!rule)
    return -1;

  if (!(equal = strchr(line, '=')) ||
      !(colon = strchr(equal + 1, ':')))
  {
    return 1;
  }

  *equal = '\0';
  *colon = '\0';

  rule->action = action_alsa_setting;
  rule->entry  = line;
  rule->elemid = equal + 1;
  rule->value  = colon + 1;

  return valid_entry(lineno, rule->entry) ? 0 : -1;
}

/**
 * Parse outband execution rule definition
 *
 * @param lineno  config file reference
 * @param line    string to parse
 * @param rule    pointer to store the result
 *
 * @return        -1 if error, 0 if parsed, 1 if skipped
 */
static int
ruldef_parse_outband(int lineno, char *line, struct ruldef *rule)
{
  char *equal, *colon;
  int status;

  /*
   * Parsed line examples:
   * entry=@outband_execution@delay:100
   * entry=@outband_cancellation@
   */

  if (!rule)
    return -1;

  if (!(equal = strchr(line, '=')))
    return 1;

  if ((colon = strchr(equal + 1, ':')))
  {
    if (strncmp(equal + 1, "@outband_execution@delay", colon - equal - 1))
      return 1;

    *equal = '\0';
    *colon = '\0';

    status = valid_entry(lineno, line) ? 0 : -1;

    if (num_parse(lineno, colon + 1, &rule->delay) < 0)
      status = -1;

    rule->action = action_outband_execution;
    rule->entry = line;
  }
  else if (!strcmp(equal + 1, "@outband_cancellation@"))
  {
    *equal = '\0';

    status = valid_entry(lineno, line) ? 0 : -1;

    rule->action = action_outband_cancellation;
    rule->entry = line;
    rule->delay = -1;
  }
  else
    status = 1;

  return status;
}

/**
 * Parse suspend execution rule definition
 *
 * @param lineno  config file reference
 * @param line    string to parse
 * @param rule    pointer to store the result
 *
 * @return        -1 if error, 0 if parsed, 1 if skipped
 */
static int
ruldef_parse_suspend(int lineno, char *line, struct ruldef *rule)
{
  char *equal;
  char *colon;
  int status;

  /*
   * Line example:
   * entry=@suspend_execution@sleep:1000
   */

  if (!rule)
    return -1;

  if (!(equal = strchr(line, '=')) ||
      !(colon = strchr(equal + 1, ':')) ||
      strncmp(equal + 1, "@suspend_execution@sleep", colon - equal - 1))
  {
    return 1;
  }

  *equal = '\0';
  *colon = '\0';
  status = 0;

  if (!valid_entry(lineno, line) ||
      num_parse(lineno, colon + 1, &rule->delay) < 0)
  {
    status = -1;
  }

  rule->action = action_suspend_execution;
  rule->entry  = line;

  return status;
}

/**
 * Parse config file section [default]
 *
 * @param lineno  config file reference
 * @param line    string to parse
 * @param rule    pointer to store the result
 *
 * @return        0 if success, -1 if error
 */
static int
ruldef_parse_deflt(int lineno, char *line, struct ruldef *rule)
{
  char *colon;
  int status;

  /*
   * Entry is omitted. Line example:
   * l-l2-bypass-hpcom-switch:Off
   */

  if (!rule)
    return -1;

  if (!(colon = strchr(line, ':')))
  {
    log_error("Invalid definition '%s' in line %d", line, lineno);
    return -1;
  }

  *colon = '\0';
  rule->action = action_alsa_setting;
  rule->entry  = "<default>";
  rule->elemid = line;
  rule->value  = colon + 1;
  return 0;
}

/**
 * Check if entry is valid
 *
 * @param lineno  config file line number
 * @param entry   entry string to check
 *
 * @return        1 if entry is valid, 0 otherwise
 */
static int
valid_entry(int lineno, char *entry)
{
  int c;

  if (!isalpha(*entry))
    goto invalid;

  while((c = *entry++) != '\0')
  {
    if (!isalpha(c) && !isdigit(c) && c != '-' && c != '_')
      goto invalid;
  }

  return 1;

invalid:
  log_error("Invalid entry id '%s' in line %d", entry, lineno);
  return 0;
}

/**
 * Allocate, initialize and return cardtbl
 * @return  pointer to allocated cardtbl struct or NULL
 */
static struct cardtbl *
cardtbl_create()
{
  struct cardtbl *tbl;

  tbl = malloc(sizeof(*tbl));

  if (tbl)
    memset(tbl, 0, sizeof(*tbl));

  return tbl;
}

/**
 * Free allocated cardtbl struct
 * @param tbl  allocated cardtbl struct pointer
 * @return  NULL
 */
static struct cardtbl *
cardtbl_free(struct cardtbl *tbl)
{
  struct cardtbl_elem *elem;
  int i;

  if (tbl)
  {
    for (i = 0;  i < tbl->len;  i++)
    {
      elem = tbl->array[i];
      free(elem->id);
      free(elem->name);
      free(elem);
    }
    free(tbl->array);
    free(tbl);
  }

  return NULL;
}

/**
 * Find sound card definition in card table.
 * Create a new one if not found.
 *
 * @param tbl   cardtbl struct pointer
 * @param id    sound card id
 * @param name  sound card name
 *
 * @return      pointer to appropriate card_def instance
 */
static struct card_def *
cardtbl_get_card_def(struct cardtbl *tbl, char *id, char *name)
{
  struct cardtbl_elem *elem;
  int i;

  if (!tbl || !name)
    return NULL;

  for (i = 0;  i < tbl->len;  i++)
  {
    elem = tbl->array[i];
    if ((!strcmp(elem->id, "*") || (id && !strcmp(id, elem->id))) &&
        (!strcmp(elem->name, "*") || (name && !strcmp(name, elem->name))))
    {
      return elem->card;
    }
  }

  tbl->array = realloc(tbl->array, sizeof(*tbl->array) * (tbl->len + 1));

  if (!tbl->array || !(elem = malloc(sizeof(*elem))))
  {
    log_error("%s(): memory (re)allocation failed", "cardtbl_get_card_def");
    exit(errno);
  }

  memset(elem, 0, sizeof(*elem));
  elem->id = strdup(id ? id : "*");
  elem->name = strdup(name);
  elem->card = control_define_card(id, name);

  if (!elem->id || !elem->name)
  {
    log_error("%s(): memory allocation failed", "cardtbl_get_card_def");
    goto fail;
  }

  if (!elem->card)
    goto fail;

  tbl->array[tbl->len++] = elem;
  return elem->card;

fail:
  free(elem->id);
  free(elem->name);
  free(elem);

  return NULL;
}

/**
 * Allocate, initialize and return elemtbl
 * @return  pointer to allocated elemtbl struct or NULL
 */
static struct elemtbl *
elemtbl_create()
{
  struct elemtbl *tbl;

  tbl = malloc(sizeof(*tbl));

  if (tbl)
    memset(tbl, 0, sizeof(*tbl));

  return tbl;
}

/**
 * Free allocated elemtbl struct
 * @param tbl  allocated elemtbl struct pointer
 * @return  NULL
 */
static struct elemtbl *
elemtbl_free(struct elemtbl *tbl)
{
  struct elemtbl_elem *elem;
  int i;

  if (tbl)
  {
    for (i = 0;  i < tbl->len;  i++)
    {
      elem = tbl->array[i];
      free(elem->id);
      free(elem);
    }

    free(tbl->array);
    free(tbl);
  }

  return NULL;
}

/**
 * Create an instance of control element definition and add it to elemtbl
 *
 * @param tbl       elemtbl pointer
 * @param elemid    control element id
 * @param card_def  definition of sound card the control element belongs to
 * @param ifname    control element interface
 * @param name      control element name
 * @param index     control element index
 * @param dev       control element device
 * @param subdev    control element subdevice
 *
 * @return  0 if success, -1 if error
 */
static int
elemtbl_add_elem(struct elemtbl *tbl,
                 char *elemid,
                 struct card_def *card_def,
                 char *ifname,
                 char *name,
                 int index,
                 int dev,
                 int subdev)
{
  struct elemtbl_elem *elem;

  if (!tbl || !elemid || !card_def)
    return -1;

  if (elemtbl_find_by_id(tbl, elemid, NULL))
  {
    log_error("Attempt to redefine control '%s'", elemid);
    return -1;
  }

  tbl->array = realloc(
      tbl->array, sizeof(*tbl->array) * (tbl->len + 1));

  if (!tbl->array || !(elem = malloc(sizeof(*elem))))
  {
    log_error("%s(): memory (re)allocation failed", "elemtbl_add_elem");
    exit(errno);
  }

  memset(elem, 0, sizeof(*elem));
  elem->id = strdup(elemid);
  elem->card = card_def;
  elem->elem = control_define_elem(card_def, ifname, name, index, dev, subdev);

  if (!elem->id)
  {
    log_error("%s(): memory allocation failed", "elemtbl_add_elem");
    goto fail;
  }

  if (!elem->elem)
    goto fail;

  tbl->array[tbl->len++] = elem;
  return 0;

fail:
  free(elem->id);
  free(elem);

  return -1;
}

/**
 * Find control element definition in elemtbl
 *
 * @param tbl       pointer to elemtbl instance
 * @param elemid    control element id
 * @param card_def  if not NULL, return control element sound card definition
 *
 * @return  found control element definition or NULL
 */
static struct elem_def *
elemtbl_find_by_id(struct elemtbl *tbl, char *elemid, struct card_def **card_def)
{
  struct elemtbl_elem *elem;
  int i;

  if (!tbl || !elemid)
    return NULL;

  for (i = 0;  i < tbl->len;  i++)
  {
    elem = tbl->array[i];

    if (!strcmp(elemid, elem->id))
    {
      if (card_def)
        *card_def = elem->card;

      return elem->elem;
    }
  }

  if (card_def)
    *card_def = NULL;

  return NULL;
}

/**
 * Allocate, initialize and return entrytbl
 * @return  pointer to allocated entrytbl struct or NULL
 */
static struct entrytbl *
entrytbl_create()
{
  struct entrytbl *tbl;

  tbl = malloc(sizeof(*tbl));

  if (tbl)
    memset(tbl, 0, sizeof(*tbl));

  return tbl;
}

/**
 * Free allocated entrytbl struct
 * @param tbl  allocated entrytbl struct pointer
 * @return  NULL
 */
static struct entrytbl *
entrytbl_free(struct entrytbl *tbl)
{
  struct entrytbl_elem *elem;
  int i;

  if (tbl)
  {
    for (i = 0;  i < tbl->len;  i++)
    {
      elem = tbl->array[i];
      free(elem->name);
      free(elem);
    }

    free(tbl->array);
    free(tbl);
  }

  return NULL;
}

/**
 * Find an entry in entrytbl by its name
 *
 * @param tbl    entrytbl instance
 * @param entry  name of the entry
 *
 * @return       entry_def struct instance or NULL
 */
static struct entry_def *
entrytbl_get_entry(struct entrytbl *tbl, char *entry_name)
{
  struct entrytbl_elem *elem;
  int i;

  if (!tbl || !entry_name)
    return NULL;

  for (i = 0;  i < tbl->len;  i++)
  {
    elem = tbl->array[i];
    if (!strcmp(entry_name, elem->name))
      return elem->entry;
  }

  tbl->array = realloc(tbl->array, sizeof(*tbl->array) * (tbl->len + 1));

  if (!tbl->array || !(elem = malloc(sizeof(*elem))))
  {
    log_error("%s(): memory (re)allocation failed", "entrytbl_get_entry");
    exit(errno);
  }

  memset(elem, 0, sizeof(*elem));
  elem->name  = strdup(entry_name);
  elem->entry = control_define_entry(entry_name);

  if (!elem->entry)
  {
    log_error("%s(): memory allocation failed", "entrytbl_get_entry");
    goto fail;
  }

  if (!elem->entry)
    goto fail;

  tbl->array[tbl->len++] = elem;
  return elem->entry;

fail:
  free(elem->entry);
  free(elem);

  return NULL;
}
