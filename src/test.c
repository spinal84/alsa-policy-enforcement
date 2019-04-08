/*
 * Test modules functionality
 */

#include <stdio.h>
#include <unistd.h>
#include <glib.h>

#include "alsaif.h"
#include "logging.h"

int main() {
  GMainLoop *main_loop;
  struct options opts;

  /* Setup logging to console */
  opts.daemon = 0;
  opts.list_and_exit = 0;
  opts.log_mask = LOG_MASK_ALL;
  opts.alsaif.log_info = 1;
  opts.alsaif.log_ctl  = 1;
  opts.alsaif.log_val  = 1;
  log_init(&opts);

  /* Test logging facility */
  log_error("error test");
  log_warning("warning test");
  log_notice("notice test");
  log_info("info test");

  /* Test alsaif */
  alsaif_init(&opts);
  alsaif_create();

  /* Do infinite loop */
  main_loop = g_main_loop_new(NULL, FALSE);
  g_main_loop_run(main_loop);

  log_notice("Exiting...");
}
