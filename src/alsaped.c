#include <unistd.h>
#include <libgen.h>
#include <stdio.h>
#include <sched.h>
#include <glib.h>
#include <pwd.h>

#include "logging.h"
#include "control.h"
#include "alsaif.h"
#include "dbusif.h"
#include "config.h"


static struct {
  GMainLoop  *main_loop;
  GIOChannel *gio_stdin;
  guint       gio_src_id;
} priv;


static void parse_options (int, char **, struct options *);
static void sig_handler   (int);
static int  daemonize     (uid_t, const char *);
static void set_rt_prio   (int prio);


static gboolean
stdin_handler(GIOChannel *source, GIOCondition condition, gpointer data)
{
  char buf;
  ssize_t bytes_read;
  static long value = 0;

  while (TRUE)
  {
    bytes_read = read(STDIN_FILENO, &buf, 1);

    if (bytes_read == 1)
      break;

    if (errno != EINTR)  /* Interrupted system call */
      return FALSE;
  }

  switch (buf)
  {
    case '+':
      value += 5;
      if (value > 50)
        value = 50;
      alsaif_set_value(0, 1, &value);
      break;

    case '-':
      value -= 5;
      if (value < 0)
        value = 0;
      alsaif_set_value(0, 1, &value);
      break;

    case 'E':
      control_run_rules_for_entry(rule_sink, "earpiece");
      break;

    case 'H':
      control_run_rules_for_entry(rule_sink, "headset");
      break;

    case 'I':
      control_run_rules_for_entry(rule_sink, "ihf");
      break;

    case 'V':
      alsaif_get_value(0, 1, &value);
      printf("value = %ld\n", value);
      break;

    default:
      break;
  }

  return TRUE;
}

static void
setup_interact()
{
  priv.gio_stdin = g_io_channel_unix_new(STDIN_FILENO);

  if (priv.gio_stdin)
  {
    priv.gio_src_id = g_io_add_watch(priv.gio_stdin,
				     G_IO_IN | G_IO_ERR | G_IO_HUP,
				     stdin_handler, NULL);
  }
  else
  {
    log_error("Can't setup interact");
  }
}

int main(int argc, char **argv)
{
  int retval;
  struct sigaction sig_action;
  struct options options;

  memset(&options, 0, sizeof(options));
  options.uid = 0;
  options.config_path = "/etc/alsaped.conf";
  options.log_mask = LOG_FLAG_ERROR;
  parse_options(argc, argv, &options);
  memset(&sig_action, 0, sizeof(sig_action));
  sig_action.sa_handler = sig_handler;

  if (sigaction(SIGHUP, &sig_action, NULL) < 0 ||
      sigaction(SIGTERM, &sig_action, NULL) < 0 ||
      sigaction(SIGINT, &sig_action, NULL) < 0)
  {
    retval = errno;
    fputs("Failed to install signal handlers\n", stderr);
    return retval;
  }

  if (log_init(&options) < 0 ||
      config_init(&options) < 0 ||
      control_init(&options) < 0 ||
      alsaif_init(&options) < 0 ||
      dbusif_init(&options) < 0)
  {
    fputs("Error during initialization\n", stderr);
    return EINVAL;
  }

  if (options.daemon && !options.list_and_exit &&
      daemonize(options.uid, options.work_dir) < 0)
  {
    retval = errno;
    perror("Can't run as a daemon");
    return retval;
  }

  priv.main_loop = g_main_loop_new(NULL, FALSE);
  if (!priv.main_loop)
  {
    log_error("Can't create main loop");
    return EIO;
  }

  if (!options.list_and_exit)
  {
    if (control_set_cb() < 0)  /* FIXME: always return 0 */
    {
      log_error("control creation failed");
      return EIO;
    }

    if (config_parse() < 0)
    {
      log_error("Configuration file error");
      return errno;
    }
  }

  if (alsaif_create() < 0)  /* FIXME: always return 0 */
  {
    retval = errno;
    log_error("ALSA interface creation failed");
    return retval;
  }

  if (options.list_and_exit)
    return 0;

  if (dbusif_create() < 0)
  {
    log_error("D-Bus interface creation failed");
    return errno;
  }

  if (options.interact)
    setup_interact();

  if (options.rt_prio)
    set_rt_prio(options.rt_prio);

  log_info("Started");
  g_main_loop_run(priv.main_loop);
  if (priv.main_loop)
    g_main_loop_unref(priv.main_loop);

  log_info("Exiting now ...");
  return 0;
}

static void
help_exit(int argc, char **argv, int status)
{
  printf(
    "Usage: %s [-h] [-d] [-u user] [-p priority] [-f config_file] [-l] [-v] [-r] [-b] [-e] [-m error,info,warning]\n",
    basename(argv[0]));
  puts("\th\t\tprint this help message and exit");
  puts("\td\t\trun as a daemon");
  puts("\ti\t\taccept commands from stdin");
  puts("\tu user\t\tif daemonized run as user");
  puts("\tp priority\trun on run-time priority");
  puts("\tf config_file\tconfig file path. If not specified the");
  puts("\t\t\tdefault config file is /etc/alsaped.conf");
  puts("\tl\t\tprint the list of the detected ALSA controls and exit");
  puts("\tv\t\tlog the value changes of the ALSA controls");
  puts("\tr\t\tlog the parsed rules");
  puts("\tb\t\tlog D-Bus message related information");
  puts("\te\t\tlog rule execution related information");
  puts("\tm\t\twhat to log. the -vrbe option turns on all levels");

  exit(status);
}

static void
parse_options(int argc, char **argv, struct options *options)
{
  struct passwd *passwd;
  char *endptr;
  char *args;
  int c;

  while ((c = getopt(argc, argv, "diu:f:hp:lvrbem:")) != -1)
  {
    switch (c)
    {
      case 'b':
        /* Log D-Bus message related information */
        options->log_mask = LOG_MASK_ALL;
        options->dbusif.log = TRUE;
        break;

      case 'd':
        /* Run as a daemon */
        options->daemon = TRUE;
        break;

      case 'e':
        /* Log rule execution related information */
        options->log_mask = LOG_MASK_ALL;
        options->log_rule_execution = TRUE;
        break;

      case 'f':
        /* Config file path. If not specified
         * the default config file is /etc/alsaped.conf */
        if (!optarg && !*optarg)
          help_exit(argc, argv, EINVAL);
        options->config_path = optarg;
        break;

      case 'h':
        /* Print help message and exit */
        help_exit(argc, argv, 0);
        return;

      case 'i':
        /* Accept commands from stdin */
        options->interact = TRUE;
        break;

      case 'l':
        /* Print the list of detected ALSA controls and exit */
        options->list_and_exit   = TRUE;
        options->log_mask        = LOG_MASK_ALL;
        options->alsaif.log_info = TRUE;
        options->alsaif.log_ctl  = TRUE;
        break;

      case 'm':
        /* What to log.
         * These options turn on all log levels:
         * -v, -r, -b, -e */
        if (!optarg)
          help_exit(argc, argv, EINVAL);

        args = optarg;
        options->log_mask = LOG_MASK_NONE;

        do
        {
          if (!strncmp(args, "error", 5))
          {
            options->log_mask |= LOG_LEVEL_ERROR;
            args += 5;
          }
          else if (!strncmp(args, "warning", 7))
          {
            options->log_mask |= LOG_LEVEL_WARNING;
            args += 7;
          }
          else if (!strncmp(args, "info", 4))
          {
            options->log_mask |= LOG_LEVEL_INFO;
            args += 4;
          }
          else
          {
            help_exit(argc, argv, EINVAL);
          }

          if (*args == ',')
            ++args;
          else if (*args)
            help_exit(argc, argv, EINVAL);
        }
        while (*args);

        break;

      case 'p':
        /* Run on run-time priority */
        if (!optarg)
          help_exit(argc, argv, EINVAL);

        options->rt_prio = strtol(optarg, &endptr, 10);
        if (endptr == optarg || *endptr)
          help_exit(argc, argv, EINVAL);

        break;

      case 'r':
        /* Log parsed rules */
        options->log_mask = LOG_MASK_ALL;
        options->log_parsed_rules = TRUE;
        break;

      case 'u':
        /* If daemonized, run as user */
        if (!optarg && !*optarg)
          help_exit(argc, argv, EINVAL);
        options->uid = -1;

        while ((passwd = getpwent()))
        {
          if (!strcmp(optarg, passwd->pw_name))
          {
            options->uid = passwd->pw_uid;
            break;
          }
        }

        if (options->uid == -1)
        {
          printf("Invalid username: %s\n", optarg);
          help_exit(argc, argv, EINVAL);
        }

        break;

      case 'v':
        /* Log ALSA ctls changes */
        options->log_mask = LOG_MASK_ALL;
        options->alsaif.log_val = TRUE;
        break;

      default:
        help_exit(argc, argv, EINVAL);
    }
  }

  if (!options->daemon && options->uid)
    puts("Warning: -d is not present; ignoring -u option");
}

static void
sig_handler(int signal)
{
  if (signal > SIGTERM || !priv.main_loop ||
      !((1 << signal) & (1 << SIGHUP | 1 << SIGINT | 1 << SIGTERM)))
  {
    /* Interrupted system call */
    exit(EINTR);
  }

  g_main_loop_quit(priv.main_loop);
}

static int
daemonize(uid_t uid, const char *cwd)
{
  int dev_null;
  pid_t pid = fork();

  if (pid < 0)
    return -1;

  /* Terminate parent */
  if (pid > 0)
    exit(0);

  if (setsid() < 0 ||
      (uid && setuid(uid) < 0) ||
      (cwd && chdir(cwd) < 0))
  {
    return -1;
  }

  dev_null = open("/dev/null", O_RDWR, 0);
  if (dev_null < 0)
    return -1;

  if (dup2(dev_null, fileno(stdin))  < 0 ||
      dup2(dev_null, fileno(stdout)) < 0 ||
      dup2(dev_null, fileno(stderr)) < 0)
  {
    return -1;
  }

  if (fileno(stdin)  != dev_null &&
      fileno(stdout) != dev_null &&
      fileno(stderr) != dev_null)
  {
    close(dev_null);
  }

  return 0;
}

static void
set_rt_prio(int prio)
{
  struct sched_param sched_param;

  if (prio < 0 || prio > 99)
  {
    log_info("Ignoring invalid realtime priority %d", prio);
    return;
  }

  sched_param.sched_priority = prio;

  if (sched_setscheduler(0, SCHED_RR, &sched_param) < 0)
    log_info("Failed to set realtime priority %d. Reason: %s", prio, strerror(errno));
}
