// Copyright 2026 Feralthedogg
// SPDX-License-Identifier: Apache-2.0

#ifndef LLAM_EXPERIMENTS_LCCF_PLATFORM_H
#define LLAM_EXPERIMENTS_LCCF_PLATFORM_H

#include "lccf_portable_errno.h"

#include <stddef.h>
#include <stdint.h>

typedef int (*lccf_platform_thread_fn)(void *context);
typedef struct lccf_platform_thread lccf_platform_thread_t;
typedef struct lccf_platform_event lccf_platform_event_t;

int lccf_platform_thread_start(lccf_platform_thread_t **out,
                               lccf_platform_thread_fn fn,
                               void *context);
int lccf_platform_thread_join(lccf_platform_thread_t *thread,
                              int *out_result);

int lccf_platform_event_create(lccf_platform_event_t **out);
void lccf_platform_event_destroy(lccf_platform_event_t *event);
int lccf_platform_event_signal(lccf_platform_event_t *event);
int lccf_platform_event_wait(lccf_platform_event_t *event,
                             uint64_t observed_epoch);
uint64_t lccf_platform_event_epoch(lccf_platform_event_t *event);

uint64_t lccf_platform_monotonic_ns(void);
uint64_t lccf_platform_process_cpu_ns(void);
int lccf_platform_pin_current_thread(unsigned cpu);
int lccf_platform_describe(char *buffer, size_t buffer_size);

#endif
