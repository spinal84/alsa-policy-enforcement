/**
 * @file dbusif.c
 * @copyright GNU GPLv2 or later
 *
 * ALSA Policy Enforcement D-Bus interface functions.
 * Control alsaped with D-Bus messages. This is the main source of events
 * for the daemon, which reacts according to rules read from config file.
 *
 * @{ */

#include <glib.h>
#include <stdio.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <dbus/dbus-glib-lowlevel.h>

#include "control.h"
#include "options.h"
#include "logging.h"

#include "dbusif.h"

#define ADMIN_DBUS_MANAGER          "org.freedesktop.DBus"
#define ADMIN_DBUS_PATH             "/org/freedesktop/DBus"
#define ADMIN_DBUS_INTERFACE        "org.freedesktop.DBus"

#define ADMIN_NAME_OWNER_CHANGED    "NameOwnerChanged"

#define POLICY_DBUS_INTERFACE       "com.nokia.policy"
#define POLICY_DBUS_MYPATH          "/com/nokia/policy/enforce/alsa"
#define POLICY_DBUS_MYNAME          "com.nokia.policy.alsa"
#define POLICY_DBUS_PDPATH          "/com/nokia/policy"
#define POLICY_DBUS_PDNAME          "org.freedesktop.ohm"

#define POLICY_DECISION             "decision"
#define POLICY_ACTIONS              "audio_actions"
#define POLICY_STATUS               "status"


/* Action descriptor */
struct actdsc {
  const char   *name;
  int         (*parser)(DBusMessageIter *);
};

/** Argument descriptor for actions */
struct argdsc {
  const char   *name;
  int           offs;
  int           type;
};

/** audio_route arguments */
struct argrt {
  char         *type;
  char         *device;
};

/** context arguments */
struct argctx {
  char         *variable;
  char         *value;
};


/** D-Bus interface data */
static struct {
  /** audio_actions signal callback */
  action_handler cb;
  /** D-Bus connection */
  void *conn;
  /* Signal interface */
  char *ifname;
  /* My signal path */
  char *mypath;
  /* Policy daemon's signal path */
  char *pdpath;
  /* Policy daemon's D-Bus name */
  char *pdname;
  /* Match rule to catch name changes */
  char *admrule;
  /* Match rule to catch action signals */
  char *actrule;
  /** Whether or not registered to policy daemon */
  int   regist;
  /** Log D-Bus message related information */
  int   log;
} priv;


static int register_to_pdp(void *user_data);
static void registration_cb(DBusPendingCall *pend, void *data);
static DBusHandlerResult filter(DBusConnection *, DBusMessage *, void *);
static void handle_admin_message(DBusMessage *msg);
static void handle_action_message(DBusMessage *msg);
static int audio_route_parser(DBusMessageIter *actit);
static int context_parser(DBusMessageIter *actit);
static int signal_status(uint32_t txid, uint32_t status);
void dbusif_free();


/**
 * Initialize D-Bus interface options
 * @param options  options parsed from command line
 * @return  -1 if error, 0 if success
 */
int
dbusif_init(struct options *options)
{
  struct dbusif_options *opts;
  char admrule[512];
  char actrule[512];
  char *ifname;
  char *mypath;
  char *pdpath;
  char *pdname;

  opts = &options->dbusif;

  ifname = opts->ifname ? opts->ifname : POLICY_DBUS_INTERFACE;
  mypath = opts->mypath ? opts->mypath : POLICY_DBUS_MYPATH;
  pdpath = opts->pdpath ? opts->pdpath : POLICY_DBUS_PDPATH;
  pdname = opts->pdname ? opts->pdname : POLICY_DBUS_PDNAME;

  snprintf(admrule, sizeof(admrule),
    "type='signal',sender='%s',path='%s',interface='%s',member='%s',arg0='%s'",
    ADMIN_DBUS_MANAGER, ADMIN_DBUS_PATH, ADMIN_DBUS_INTERFACE,
    ADMIN_NAME_OWNER_CHANGED, pdname);

  snprintf(actrule, sizeof(actrule),
    "type='signal',interface='%s',member='%s',path='%s/%s'",
    ifname, POLICY_ACTIONS, pdpath, POLICY_DECISION);

  if (!(priv.ifname = strdup(ifname)) ||
      !(priv.mypath = strdup(mypath)) ||
      !(priv.pdpath = strdup(pdpath)) ||
      !(priv.pdname = strdup(pdname)) ||
      !(priv.admrule = strdup(admrule)) ||
      !(priv.actrule = strdup(actrule)))
  {
    return -1;
  }

  priv.log = opts->log;

  return 0;
}

/**
 * Set D-Bus interface callback function
 * @param cb  callback function
 */
void
dbusif_set_cb(action_handler cb)
{
  priv.cb = cb;
}

/**
 * Set filters for NameOwnerChanged and audio_actions/decision signals.
 * Register to Policy Decision Point over D-Bus.
 *
 * @return  0 if success, -1 if error
 */
int dbusif_create()
{
  DBusError error;

  dbus_error_init(&error);
  priv.conn = dbus_bus_get(DBUS_BUS_SYSTEM, &error);

  if (!priv.conn)
  {
    if (dbus_error_is_set(&error))
      log_error("Can't get D-Bus connection: %s", error.message);
    else
      log_error("Can't get D-Bus connection");

    goto fail;
  }

  dbus_connection_setup_with_g_main(priv.conn, NULL);

  if (!dbus_connection_add_filter(priv.conn, filter, NULL, NULL))
  {
    log_error("Can't add D-Bus filter function");
    goto fail;
  }

  dbus_bus_add_match(priv.conn, priv.admrule, &error);

  if (dbus_error_is_set(&error))
  {
    log_error("unable to subscribe name change signals: %s: %s",
               error.name, error.message);
    goto fail;
  }

  dbus_bus_add_match(priv.conn, priv.actrule, &error);

  if (dbus_error_is_set(&error))
  {
    log_error("unable to subscribe policy %s signal on %s: %s: %s",
               POLICY_ACTIONS, priv.ifname, error.name, error.message);
    goto fail;
  }

  register_to_pdp(NULL);

  return 0;

fail:
    dbusif_free();
    dbus_error_free(&error);
    errno = EIO;  /* I/O error */

    return -1;
}

/**
 * Free D-Bus interface allocated memory
 */
void dbusif_free()
{
  if (priv.conn)
  {
    dbus_connection_remove_filter(priv.conn, filter, NULL);
    dbus_bus_remove_match(priv.conn, priv.admrule, NULL);
    dbus_bus_remove_match(priv.conn, priv.actrule, NULL);
    dbus_connection_unref(priv.conn);
  }
  free(priv.ifname);
  free(priv.mypath);
  free(priv.pdpath);
  free(priv.pdname);
  free(priv.admrule);
  free(priv.actrule);
  priv.regist = FALSE;
}

/**
 * Register to policy decision point
 * @param user_data  user_data passed to registration callback
 * @return  0 if succes, -1 if error
 */
static int
register_to_pdp(void *user_data)
{
  static const char *name = "alsaped";

  DBusMessage     *msg;
  DBusPendingCall *pend;
  char            *signals[4];
  char           **v_ARRAY;
  int              i;
  int              success;

  if (priv.log)
  {
    log_info("registering to policy daemon: name='%s' path='%s' if='%s'",
              priv.pdname, priv.pdpath, priv.ifname);
  }

  msg = dbus_message_new_method_call(priv.pdname, priv.pdpath,
                                     priv.ifname, "register");

  if (!msg)
  {
    log_error("Failed to create D-Bus message to register");
    success = FALSE;
    goto out;
  }

  signals[i=0] = POLICY_ACTIONS;
  v_ARRAY = signals;

  success = dbus_message_append_args(msg,
                                     DBUS_TYPE_STRING, &name,
                                     DBUS_TYPE_ARRAY,
                                     DBUS_TYPE_STRING, &v_ARRAY, i+1,
                                     DBUS_TYPE_INVALID);

  if (!success)
  {
    log_error("Failed to build D-Bus message to register");
    goto out;
  }

  success = dbus_connection_send_with_reply(priv.conn, msg, &pend, 1000);

  if (!success)
  {
    log_error("Failed to register to Policy Decision Point over D-Bus");
    goto out;
  }

  success = dbus_pending_call_set_notify(
                              pend, registration_cb, user_data, NULL);

  if (!success)
    log_error("Can't set notification for D-Bus registration");

out:
  dbus_message_unref(msg);
  return success ? 0 : -1;
}

/**
 * D-Bus interface registration callback
 * @param pend  D-Bus pending call
 * @param data  user_data
 */
static void
registration_cb(DBusPendingCall *pend, void *data)
{
  intptr_t name_owner_changed = (intptr_t)data;
  DBusMessage *reply;
  const char  *error_descr;
  int          success;

  if (!(reply = dbus_pending_call_steal_reply(pend)))
  {
    log_error("registration to policy daemon failed: invalid argument in reply");
    return;
  }

  if (dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_ERROR)
  {
    if (name_owner_changed)
    {
      success = dbus_message_get_args(reply, NULL,
                                      DBUS_TYPE_STRING, &error_descr,
                                      DBUS_TYPE_INVALID);
      if (!success)
        error_descr = dbus_message_get_error_name(reply);

      log_error("registration to policy daemon failed: %s", error_descr);
    }
  }
  else
  {
    log_notice("registration to policy daemon succeeded");
    priv.regist = 1;
  }

  dbus_message_unref(reply);
}

/**
 * Filter incoming D-Bus messages
 */
static DBusHandlerResult
filter(DBusConnection *conn, DBusMessage *msg, void *user_data)
{
  if (dbus_message_is_signal(msg, ADMIN_DBUS_INTERFACE,
                             ADMIN_NAME_OWNER_CHANGED))
  {
    handle_admin_message(msg);
    return DBUS_HANDLER_RESULT_HANDLED;
  }

  if (dbus_message_is_signal(msg, POLICY_DBUS_INTERFACE,
                             POLICY_ACTIONS))
  {
    handle_action_message(msg);
    return DBUS_HANDLER_RESULT_HANDLED;
  }

  return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

/**
 * Handle NameOwnerChanged D-Bus signal
 */
static void
handle_admin_message(DBusMessage *msg)
{
  char *name;
  char *before;
  char *after;
  int   success;

  success = dbus_message_get_args(msg, NULL,
                                  DBUS_TYPE_STRING, &name,
                                  DBUS_TYPE_STRING, &before,
                                  DBUS_TYPE_STRING, &after,
                                  DBUS_TYPE_INVALID);

  if (!success || !name)
  {
    log_error("Received malformed '%s' message", ADMIN_NAME_OWNER_CHANGED);
    return;
  }

  if (strcmp(name, priv.pdname))
    return;

  if (after && *after)
  {
    log_info("policy decision point is up");

    if (!priv.regist)
      register_to_pdp((void *)1);

    return;
  }

  if (name && before)
  {
    log_info("policy decision point is gone");
    priv.regist = 0;
  }
}

/**
 * Handle D-Bus audio_actions signal
 */
static void
handle_action_message(DBusMessage *msg)
{
  static struct actdsc actions[] = {
    { "com.nokia.policy.audio_route", audio_route_parser },
    { "com.nokia.policy.context"    , context_parser     },
    { NULL                          , NULL               }
  };

  struct actdsc   *act;
  DBusMessageIter  msgit;
  dbus_uint32_t    txid;
  char            *actname;
  DBusMessageIter  arrit;
  DBusMessageIter  entit;
  DBusMessageIter  actit;
  int              success = TRUE;

  dbus_message_iter_init(msg, &msgit);

  if (dbus_message_iter_get_arg_type(&msgit) != DBUS_TYPE_UINT32)
    return;

  dbus_message_iter_get_basic(&msgit, &txid);

  if (priv.log)
    log_info("got actions (txid:%d)", txid);

  if (!dbus_message_iter_next(&msgit) ||
      dbus_message_iter_get_arg_type(&msgit) != DBUS_TYPE_ARRAY)
  {
    success = FALSE;
    goto send_signal;
  }

  dbus_message_iter_recurse(&msgit, &arrit);

  do
  {
    /* DBUS_TYPE_DICT_ENTRY is like a hash
     * It's passed like array of elements: key => value */
    if (dbus_message_iter_get_arg_type(&arrit) != DBUS_TYPE_DICT_ENTRY)
    {
      success = FALSE;
      continue;
    }

    dbus_message_iter_recurse(&arrit, &entit);

    do
    {
      if (dbus_message_iter_get_arg_type(&entit) != DBUS_TYPE_STRING)
      {
        success = FALSE;
        continue;
      }

      dbus_message_iter_get_basic(&entit, &actname);

      if (!dbus_message_iter_next(&entit) ||
          dbus_message_iter_get_arg_type(&entit) != DBUS_TYPE_ARRAY)
      {
        success = FALSE;
        continue;
      }

      dbus_message_iter_recurse(&entit, &actit);

      if (dbus_message_iter_get_arg_type(&actit) != DBUS_TYPE_ARRAY)
      {
        success = FALSE;
        continue;
      }

      for (act = actions;  act->name != NULL;  act++)
      {
        if (!strcmp(actname, act->name))
          break;
      }

      if (act->parser != NULL)
        success &= act->parser(&actit);

    }
    while (dbus_message_iter_next(&entit));

  }
  while (dbus_message_iter_next(&arrit));

send_signal:
  if (priv.log)
  {
    if (success)
      log_info("actions %s", "succeeded");
    else
      log_info("actions %s", "failed");
  }

  signal_status(txid, success);
}

/**
 * Parse audio_actions D-Bus signal args
 *
 * @param actit  D-Bus message iterator
 * @param descs  audio actions descriptor
 * @param args   pointer to return audio actions arguments
 * @param len    length of args buffer
 *
 * @return       TRUE if success, FALSE otherwise
 */
static int
action_parser(DBusMessageIter *actit, struct argdsc *descs, void *args, int len)
{
  DBusMessageIter  argit;
  DBusMessageIter  cmdit;
  DBusMessageIter  valit;
  struct argdsc   *desc;
  char            *argname;
  void            *argval;

  dbus_message_iter_recurse(actit, &cmdit);

  memset(args, 0, len);

  do
  {
    if (dbus_message_iter_get_arg_type(&cmdit) != DBUS_TYPE_STRUCT)
      return FALSE;

    dbus_message_iter_recurse(&cmdit, &argit);

    if (dbus_message_iter_get_arg_type(&argit) != DBUS_TYPE_STRING)
      return FALSE;

    dbus_message_iter_get_basic(&argit, &argname);

    if (!dbus_message_iter_next(&argit))
      return FALSE;

    /* DBUS_TYPE_VARIANT contains only single complete type */
    if (dbus_message_iter_get_arg_type(&argit) != DBUS_TYPE_VARIANT)
      return FALSE;

    dbus_message_iter_recurse(&argit, &valit);

    for (desc = descs;  desc->name != NULL;  desc++)
    {
      if (!strcmp(argname, desc->name))
      {
        if (desc->offs + sizeof(void *) > len)
        {
          log_error("D-Bus %s() desc offset %d is out of range %d",
                    __FUNCTION__, desc->offs, len);

          return FALSE;
        }

        if (dbus_message_iter_get_arg_type(&valit) != desc->type)
          return FALSE;

        argval = (char *)args + desc->offs;

        dbus_message_iter_get_basic(&valit, argval);

        break;
      }
    }
  }
  while (dbus_message_iter_next(&cmdit));

  return TRUE;
}

/**
 * Parse route audio action
 * @param actit  D-Bus message iterator
 * @return       FALSE if error, TRUE if success
 */
static int
audio_route_parser(DBusMessageIter *actit)
{
  static struct argdsc descs[] = {
    { "type",   G_STRUCT_OFFSET(struct argrt, type),   DBUS_TYPE_STRING },
    { "device", G_STRUCT_OFFSET(struct argrt, device), DBUS_TYPE_STRING },
    { NULL,                  0,                        DBUS_TYPE_INVALID}
  };

  struct action_data data;
  struct argrt args;
  int result = TRUE;

  if (priv.log)
    log_info("parsing audio routes");

  do
  {
    if (!action_parser(actit, descs, &args, sizeof(args)))
    {
      log_error("Action parsing failed");
      return FALSE;
    }

    if (!args.type || !args.device)
    {
      log_error("Some of the required action arguments are missing");
      return FALSE;
    }

    if (priv.log)
      log_info("Got routing request: '%s' -> '%s'", args.type, args.device);

    if (!strcmp(args.type, "sink"))
      data.rule_type = rule_sink;
    else if (!strcmp(args.type, "source"))
      data.rule_type = rule_source;
    else
    {
      log_error("Invalid audio route type '%s'", args.type);
      return FALSE;
    }

    if (priv.cb)
    {
      data.route_dev = args.device;
      if (priv.cb(&data) < 0)
        result = FALSE;
    }
  }
  while (dbus_message_iter_next(actit));

  return result;
}

/**
 * Parse context audio action
 * @param actit  D-Bus message iterator
 * @return       FALSE if error, TRUE if success
 */
static int
context_parser(DBusMessageIter *actit)
{
  static struct argdsc descs[] = {
    { "variable", G_STRUCT_OFFSET(struct argctx, variable), DBUS_TYPE_STRING },
    { "value",    G_STRUCT_OFFSET(struct argctx, value)   , DBUS_TYPE_STRING },
    { NULL,                    0                          , DBUS_TYPE_INVALID}
  };

  struct action_data data;
  struct argctx args;
  int result = TRUE;

  if (priv.log)
    log_info("parsing context");

  do
  {
    if (!action_parser(actit, descs, &args, sizeof(args)))
    {
      log_error("Action parsing failed");
      return FALSE;
    }

    if (!args.variable || !args.value)
    {
      log_error("Some of the required action arguments are missing");
      return FALSE;
    }

    if (priv.log)
      log_info("Got context request: '%s' '%s'", args.variable, args.value);

    if (priv.cb)
    {
      data.rule_type = rule_context;
      data.variable  = args.variable;
      data.value     = args.value;

      if (priv.cb(&data) < 0)
        result = FALSE;
    }
  }
  while (dbus_message_iter_next(actit));

  return result;
}

/**
 * Respond to audio_actions signal
 *
 * @param txid    transaction id
 * @param status  handling status
 *
 * @return  0 if success, -1 if error
 */
static int
signal_status(uint32_t txid, uint32_t status)
{
  DBusMessage *msg;
  char         path[256];
  int          ret;

  if (txid == 0)
  {
    /* When transaction ID is 0, the policy manager does not expect
     * a response. */
    log_info("Not sending status message since transaction ID is 0");
    return 0;
  }

  snprintf(path, sizeof(path), "%s/%s", priv.pdpath, POLICY_DECISION);

  if (priv.log)
  {
    log_info(
      "sending D-Bus signal to: path='%s', if='%s' member='%s' content: txid=%d status=%d",
      path, priv.ifname, POLICY_STATUS, txid, status);
  }

  msg = dbus_message_new_signal(path, priv.ifname, POLICY_STATUS);

  if (!msg)
  {
    log_error("failed to make new D-Bus status message");
    goto fail;
  }

  ret = dbus_message_append_args(msg,
          DBUS_TYPE_UINT32, &txid,
          DBUS_TYPE_UINT32, &status,
          DBUS_TYPE_INVALID);

  if (!ret)
  {
    log_error("Can't build D-Bus status message");
    goto fail;
  }

  ret = dbus_connection_send(priv.conn, msg, NULL);

  if (!ret)
  {
    log_error("Can't send status message: out of memory");
    goto fail;
  }

  dbus_message_unref(msg);

  return 0;

fail:
  dbus_message_unref(msg);

  return -1;
}

/** @} */
