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
#include <sys/inotify.h>
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
  sigaddset(&signal_set, SIGTERM);

  for (;;) {
    sigwait(&signal_set, &sig);
    if (sigismember(&signal_set, SIGHUP)) {
      if (sig == SIGHUP) {
        break;
      }
      hs_log(NULL, g_module, 6, "forced stop signal received");
      exit(EXIT_FAILURE);
    }
    if (sig == SIGINT || sig == SIGTERM) {
      hs_log(NULL, g_module, 6, "stop signal received");
      sem_post(&g_shutdown);
#ifdef HINDSIGHT_CLI
      sigaddset(&signal_set, SIGINT);
      sigaddset(&signal_set, SIGTERM);
      sigaddset(&signal_set, SIGHUP);
      continue;
#endif
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

  int watch[3] = {0};
  int load = 0;
  if (cfg.load_path[0] != 0) {
    load = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (load < 0) {
      hs_free_config(&cfg);
      perror("inotify_init failed");
      return EXIT_FAILURE;
    }
    watch[0] = inotify_add_watch(load, cfg.load_path_input,
                                 IN_CLOSE_WRITE | IN_MOVED_TO);
    watch[1] = inotify_add_watch(load, cfg.load_path_analysis,
                                 IN_CLOSE_WRITE | IN_MOVED_TO);
    watch[2] = inotify_add_watch(load, cfg.load_path_output,
                                 IN_CLOSE_WRITE | IN_MOVED_TO);
  }

  hs_checkpoint_reader cpr;
  hs_init_checkpoint_reader(&cpr, cfg.output_path);
  hs_cleanup_checkpoints(&cpr, cfg.run_path, cfg.analysis_threads);

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
  hs_init_input_plugins(&ips, &cfg, &cpr);
  hs_load_input_startup(&ips);

  hs_analysis_plugins aps;
  hs_init_analysis_plugins(&aps, &cfg, &cpr);
  hs_load_analysis_startup(&aps);
  hs_start_analysis_threads(&aps);

  hs_output_plugins ops;
  hs_init_output_plugins(&ops, &cfg, &cpr);
  hs_load_output_startup(&ops);

  hs_checkpoint_writer cpw;
  hs_init_checkpoint_writer(&cpw, &ips, &aps, &ops, cfg.output_path);

  struct timespec ts;
  const struct inotify_event *event;
  char inotify_buf[sizeof(struct inotify_event) + FILENAME_MAX + 1]
      __attribute__((aligned(__alignof__(struct inotify_event))));

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
    hs_write_checkpoints(&cpw, &cpr);

    if (load) {
      for (;;){
        ssize_t len = read(load, inotify_buf, sizeof(inotify_buf));
        if (len == -1 && errno != EAGAIN) {
          hs_log(NULL, g_module, 1, "inotify read failure");
          sem_post(&g_shutdown);
        }
        if (len <= 0) break;

        for (char *ptr = inotify_buf; ptr < inotify_buf + len;
             ptr += sizeof(struct inotify_event) + event->len) {
          event = (const struct inotify_event *)ptr;
          if (event->len) {
            if (watch[1] == event->wd) {
              hs_load_analysis_dynamic(&aps, event->name);
            } else if (watch[0] == event->wd) {
              hs_load_input_dynamic(&ips, event->name);
            } else if (watch[2] == event->wd) {
              hs_load_output_dynamic(&ops, event->name);
            }
          }
        }
      }
    }

#ifdef HINDSIGHT_CLI
    if (ips.list_cnt == 0) {
      hs_log(NULL, g_module, 6, "input plugins have exited; "
             "cascading shutdown initiated");
      pthread_kill(sig_thread, SIGINT); // when all the inputs are done, exit
    }
#endif
  }

  if (load) {
    for (int i = 0; i < 3; ++i) {
      inotify_rm_watch(load, watch[i]);
    }
    close(load);
  }
  int rv = EXIT_SUCCESS;

#ifdef HINDSIGHT_CLI
  hs_stop_input_plugins(&ips);
  hs_wait_input_plugins(&ips);
  hs_write_checkpoints(&cpw, &cpr);
  if (ips.terminated) {
    rv = 2;
  }
  hs_free_input_plugins(&ips);

  hs_stop_analysis_plugins(&aps);
  hs_wait_analysis_plugins(&aps);
  if (aps.terminated) {
    rv |= 4;
  }
  hs_write_checkpoints(&cpw, &cpr);
  hs_free_analysis_plugins(&aps);

  hs_stop_output_plugins(&ops);
  hs_wait_output_plugins(&ops);
  hs_write_checkpoints(&cpw, &cpr);
  if (ops.terminated) {
    rv |= 8;
  }
  hs_free_output_plugins(&ops);
  pthread_kill(sig_thread, SIGHUP);
#else
  // non CLI mode should shut everything down immediately
  hs_stop_input_plugins(&ips);
  hs_stop_analysis_plugins(&aps);
  hs_stop_output_plugins(&ops);

  hs_wait_input_plugins(&ips);
  hs_wait_analysis_plugins(&aps);
  hs_wait_output_plugins(&ops);

  hs_write_checkpoints(&cpw, &cpr);

  hs_free_input_plugins(&ips);
  hs_free_analysis_plugins(&aps);
  hs_free_output_plugins(&ops);
#endif

  hs_free_checkpoint_writer(&cpw);
  hs_free_checkpoint_reader(&cpr);
  hs_free_config(&cfg);

  pthread_join(sig_thread, NULL);
  hs_log(NULL, g_module, 6, "exiting");
  hs_free_log();
  sem_destroy(&g_shutdown);
  return rv;
}
