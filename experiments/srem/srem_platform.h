// Copyright 2026 Feralthedogg
// SPDX-License-Identifier: Apache-2.0

#ifndef LLAM_EXPERIMENTS_SREM_PLATFORM_H
#define LLAM_EXPERIMENTS_SREM_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

typedef int (*srem_platform_thread_fn)(void *context);
typedef struct srem_platform_thread srem_platform_thread_t;
typedef struct srem_platform_event srem_platform_event_t;

int srem_platform_thread_start(srem_platform_thread_t **out,
                               srem_platform_thread_fn fn,
                               void *context);
int srem_platform_thread_join(srem_platform_thread_t *thread,
                              int *out_result);

int srem_platform_event_create(srem_platform_event_t **out);
void srem_platform_event_destroy(srem_platform_event_t *event);
int srem_platform_event_signal(srem_platform_event_t *event);
int srem_platform_event_wait(srem_platform_event_t *event,
                             uint64_t observed_epoch);
uint64_t srem_platform_event_epoch(srem_platform_event_t *event);

uint64_t srem_platform_monotonic_ns(void);
uint64_t srem_platform_process_cpu_ns(void);
int srem_platform_pin_current_thread(unsigned cpu);
int srem_platform_describe(char *buffer, size_t buffer_size);

#endif
