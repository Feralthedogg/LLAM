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

/* The exception is thrown and caught entirely inside this call chain. */
static void __attribute__((noinline)) win_probe_throw_once() {
    volatile unsigned char local[512];

    local[0] = 42U;
    if (local[0] == 42U) {
        throw 42;
    }
}

extern "C" int win_probe_local_exception(void) noexcept {
    try {
        win_probe_throw_once();
    } catch (int value) {
        return value;
    } catch (...) {
        return -2;
    }
    return -1;
}
