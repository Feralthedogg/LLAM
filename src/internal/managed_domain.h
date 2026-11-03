/*
 * Copyright 2026 Feralthedogg
 * SPDX-License-Identifier: Apache-2.0
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * See LICENSES/Apache-2.0.txt.
 */

#ifndef LLAM_MANAGED_DOMAIN_H
#define LLAM_MANAGED_DOMAIN_H

/* This header follows runtime_types.h. Only the owning native thread changes a
 * binding or its depth. A signal may nest balanced guards on that same thread.
 * Controllers may read depth while the M and its binding are retained. */
extern atomic_uint g_llam_native_domains;
void llam_native_domain_worker_init(llam_worker_t *worker);
int llam_native_domain_bind(llam_worker_t *worker);
void llam_native_domain_unbind(llam_worker_t *worker);
llam_worker_t *llam_native_domain_guarded_worker(void);
void llam_native_domain_guard_identities(uintptr_t out[2]);

static inline bool llam_native_domain_blocked(void) {
    return atomic_load_explicit(&g_llam_native_domains, memory_order_acquire) != 0U &&
           llam_native_domain_guarded_worker() != NULL;
}

#endif /* LLAM_MANAGED_DOMAIN_H */
