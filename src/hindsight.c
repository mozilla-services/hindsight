/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/** @brief Hindsight main executable @file */

#include <errno.h>
#include <luasandbox/lauxlib.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "hs_analysis_plugins.h"
#include "hs_checkpoint_writer.h"
#include "hs_config.h"
#include "hs_input.h"
#include "hs_input_plugins.h"
#include "hs_logger.h"
#include "hs_output_plugins.h"


static const char g_module[] = "hindsight";
static sem_t g_shutdown;


void* sig_handler(void *arg)
{
  (void)arg;
  int sig;
  sigset_t signal_set;
  sigemptyset(&signal_set);
  sigaddset(&signal_set, SIGINT);

  for (;;) {
    sigwait(&signal_set, &sig);
    if (sig == SIGINT) {
      hs_log(NULL, g_module, 6, "stop signal received");
      sem_post(&g_shutdown);
      break;
    } else {
      hs_log(NULL, g_module, 6, "unexpected signal received %d", sig);
    }
  }
  return (void *)0;
}


int main(int argc, char *argv[])
{
  if (argc < 2 || argc > 3) {
    fprintf(stderr, "usage: %s <cfg> [loglevel]\n", argv[0]);
    return EXIT_FAILURE;
  }

  if (sem_init(&g_shutdown, 0, 1)) {
    perror("g_shutdown sem_init failed");
    exit(EXIT_FAILURE);
  }

  if (sem_wait(&g_shutdown)) {
    perror("g_shutdown sem_wait failed");
    exit(EXIT_FAILURE);
  }

  int loglevel = 6;
  if (argc == 3) {
    loglevel = atoi(argv[2]);
    if (loglevel < 0 || loglevel > 7) {
      loglevel = 6;
    }
  }
  hs_init_log(loglevel);

  hs_config cfg;
  if (hs_load_config(argv[1], &cfg)) {
    return EXIT_FAILURE;
  }

  if (cfg.rm_checkpoint) {
    hs_cleanup_checkpoints(&cfg.cp_reader, cfg.run_path,
                           cfg.analysis_threads);
  }

  hs_log(NULL, g_module, 6, "starting");
  sigset_t signal_set;
  sigfillset(&signal_set);
  if (pthread_sigmask(SIG_SETMASK, &signal_set, NULL)) {
    perror("pthread_sigmask failed");
    exit(EXIT_FAILURE);
  }
  pthread_t sig_thread;
  if (pthread_create(&sig_thread, NULL, sig_handler, NULL)) {
    hs_log(NULL, g_module, 1, "signal handler could not be setup");
    return EXIT_FAILURE;
  }

  hs_input_plugins ips;
  hs_init_input_plugins(&ips, &cfg);
  hs_load_input_plugins(&ips, &cfg, false);

  hs_analysis_plugins aps;
  hs_init_analysis_plugins(&aps, &cfg);
  hs_load_analysis_plugins(&aps, &cfg, false);
  hs_start_analysis_threads(&aps);

  hs_output_plugins ops;
  hs_init_output_plugins(&ops, &cfg);
  hs_load_output_plugins(&ops, &cfg, false);

  hs_checkpoint_writer cpw;
  hs_init_checkpoint_writer(&cpw, &ips, &aps, &ops, cfg.output_path);

  struct timespec ts;
  int cnt = 0;
  while (true) {
    if (clock_gettime(CLOCK_REALTIME, &ts) == -1) {
      hs_log(NULL, g_module, 3, "clock_gettime failed");
      ts.tv_sec = time(NULL);
      ts.tv_nsec = 0;
    }
    ts.tv_sec += 1;
    if (!sem_timedwait(&g_shutdown, &ts)) {
      sem_post(&g_shutdown);
      break; // shutting down
    }
    hs_write_checkpoints(&cpw, &cfg.cp_reader);
    if (cfg.load_path[0] != 0 && ++cnt == 59) {
      // scan just before emitting the stats
      hs_log(NULL, g_module, 7, "scan load directories");
      hs_load_input_plugins(&ips, &cfg, true);
      hs_load_analysis_plugins(&aps, &cfg, true);
      hs_load_output_plugins(&ops, &cfg, true);
      cnt = 0;
    }
#ifdef HINDSIGHT_CLI
    if (ips.list_cnt == 0) {
      hs_log(NULL, g_module, 6, "input plugins have exited; "
             "cascading shutdown initiated");
      pthread_kill(sig_thread, SIGINT); // when all the inputs are done, exit
    }
#endif
  }

#ifdef HINDSIGHT_CLI
  hs_stop_input_plugins(&ips);
  hs_wait_input_plugins(&ips);
  hs_write_checkpoints(&cpw, &cfg.cp_reader);
  hs_free_input_plugins(&ips);

  aps.stop = true;
  hs_wait_analysis_plugins(&aps);
  hs_write_checkpoints(&cpw, &cfg.cp_reader);
  hs_free_analysis_plugins(&aps);

  hs_stop_output_plugins(&ops);
  hs_wait_output_plugins(&ops);
  hs_write_checkpoints(&cpw, &cfg.cp_reader);
  hs_free_output_plugins(&ops);
#else
  // non CLI mode should shut everything down immediately
  hs_stop_input_plugins(&ips);
  aps.stop = true;
  hs_stop_output_plugins(&ops);

  hs_wait_input_plugins(&ips);
  hs_wait_analysis_plugins(&aps);
  hs_wait_output_plugins(&ops);

  hs_write_checkpoints(&cpw, &cfg.cp_reader);

  hs_free_input_plugins(&ips);
  hs_free_analysis_plugins(&aps);
  hs_free_output_plugins(&ops);
#endif

  hs_free_checkpoint_writer(&cpw);
  hs_free_config(&cfg);
  hs_free_log();

  pthread_join(sig_thread, NULL);
  hs_log(NULL, g_module, 6, "exiting");
  sem_destroy(&g_shutdown);
  return 0;
}
