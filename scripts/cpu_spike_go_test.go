// Copyright 2026 Feralthedogg
// SPDX-License-Identifier: Apache-2.0
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
// See LICENSE.

package main

import (
	"testing"
	"time"
)

func TestPercentileUsesNearestRank(t *testing.T) {
	samples := make([]time.Duration, 100)
	for index := range samples {
		samples[index] = time.Duration(index+1) * time.Nanosecond
	}

	if got := percentile(samples, 99); got != 99*time.Nanosecond {
		t.Fatalf("p99 = %s, want 99ns", got)
	}
	if got := percentile(samples, 100); got != 100*time.Nanosecond {
		t.Fatalf("max percentile = %s, want 100ns", got)
	}
}
