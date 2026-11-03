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
	"fmt"
	"net"
	"os"
	"runtime"
	"sort"
	"strconv"
	"sync"
	"sync/atomic"
	"syscall"
	"time"
)

const (
	defaultRounds   = 7
	maxRounds       = 100
	defaultSpikeMS  = 200
	maxSpikeMS      = 10000
	defaultSettleMS = 100
	maxSettleMS     = 5000
	hogChunk        = 65536
)

var hogSink atomic.Uint64

type roundResult struct {
	timer   time.Duration
	io      time.Duration
	control time.Duration
}

func envPositive(name string, defaultValue, maximum int) (int, error) {
	raw := os.Getenv(name)
	if raw == "" {
		return defaultValue, nil
	}
	value, err := strconv.Atoi(raw)
	if err != nil || value <= 0 || value > maximum || strconv.Itoa(value) != raw {
		return 0, fmt.Errorf("invalid %s=%q (expected 1..%d)", name, raw, maximum)
	}
	return value, nil
}

func socketPair() (net.Conn, net.Conn, error) {
	fds, err := syscall.Socketpair(syscall.AF_UNIX, syscall.SOCK_STREAM, 0)
	if err != nil {
		return nil, nil, err
	}
	leftFile := os.NewFile(uintptr(fds[0]), "cpu-spike-left")
	rightFile := os.NewFile(uintptr(fds[1]), "cpu-spike-right")
	left, leftErr := net.FileConn(leftFile)
	right, rightErr := net.FileConn(rightFile)
	leftFile.Close()
	rightFile.Close()
	if leftErr != nil || rightErr != nil {
		if left != nil {
			left.Close()
		}
		if right != nil {
			right.Close()
		}
		if leftErr != nil {
			return nil, nil, leftErr
		}
		return nil, nil, rightErr
	}
	return left, right, nil
}

func cpuHog(deadline time.Time, seed uint64, started chan<- struct{}, done *sync.WaitGroup) {
	defer done.Done()
	value := seed | 1
	started <- struct{}{}
	for time.Now().Before(deadline) {
		for i := 0; i < hogChunk; i++ {
			value = value*2862933555777941757 + 3037000493
		}
	}
	hogSink.Add(value)
}

func positiveDelay(now, expected time.Time) time.Duration {
	if now.Before(expected) {
		return 0
	}
	return now.Sub(expected)
}

func runRound(workers, spikeMS, settleMS int) (roundResult, error) {
	reader, writer, err := socketPair()
	if err != nil {
		return roundResult{}, err
	}
	defer reader.Close()
	defer writer.Close()

	ioReady := make(chan struct{}, 1)
	timerResult := make(chan time.Duration, 1)
	ioResult := make(chan time.Duration, 1)
	controlResult := make(chan time.Duration, 1)
	probeErrors := make(chan error, 1)
	var ioWritten atomic.Value

	go func() {
		ioReady <- struct{}{}
		var byte [1]byte
		if _, readErr := reader.Read(byte[:]); readErr != nil {
			probeErrors <- readErr
			return
		}
		if byte[0] != 's' {
			probeErrors <- fmt.Errorf("unexpected I/O probe byte %q", byte[0])
			return
		}
		ioResult <- time.Since(ioWritten.Load().(time.Time))
	}()
	<-ioReady

	trigger := time.Now().Add(time.Duration(settleMS) * time.Millisecond)
	deadline := trigger.Add(time.Duration(spikeMS) * time.Millisecond)
	timer := time.NewTimer(time.Until(trigger))
	go func() {
		<-timer.C
		timerResult <- positiveDelay(time.Now(), trigger)
	}()
	// Match LLAM's short quiescence interval after the reader announces that it
	// is entering the blocking read, then prove full hog occupancy separately.
	time.Sleep(5 * time.Millisecond)

	var hogs sync.WaitGroup
	hogsStarted := make(chan struct{}, workers)
	hogs.Add(workers)
	for worker := 0; worker < workers; worker++ {
		go cpuHog(deadline, uint64(worker+1), hogsStarted, &hogs)
	}
	for worker := 0; worker < workers; worker++ {
		<-hogsStarted
	}
	if !time.Now().Before(trigger) {
		return roundResult{}, fmt.Errorf(
			"only reached full CPU-hog occupancy at/after trigger; increase LLAM_CPU_SPIKE_SETTLE_MS",
		)
	}

	time.Sleep(time.Until(trigger))
	ioWritten.Store(time.Now())
	if _, err = writer.Write([]byte{'s'}); err != nil {
		return roundResult{}, err
	}
	go func() {
		controlResult <- positiveDelay(time.Now(), trigger)
	}()

	var result roundResult
	for received := 0; received < 3; received++ {
		select {
		case result.timer = <-timerResult:
		case result.io = <-ioResult:
		case result.control = <-controlResult:
		case probeErr := <-probeErrors:
			return roundResult{}, probeErr
		case <-time.After(time.Duration(spikeMS+settleMS+5000) * time.Millisecond):
			return roundResult{}, fmt.Errorf("latency probe timed out")
		}
	}
	hogs.Wait()
	return result, nil
}

func percentile(samples []time.Duration, pct int) time.Duration {
	ordered := append([]time.Duration(nil), samples...)
	sort.Slice(ordered, func(i, j int) bool { return ordered[i] < ordered[j] })
	rank := (len(ordered)*pct + 99) / 100
	index := rank - 1
	return ordered[index]
}

func printReport(metric string, samples []time.Duration, workers, rounds, spikeMS, settleMS int) {
	maximum := percentile(samples, 100)
	fmt.Printf(
		"[cpu-spike] runtime=Go metric=%s workers=%d rounds=%d spike_ms=%d settle_ms=%d p50_us=%.2f p99_us=%.2f max_us=%.2f\n",
		metric,
		workers,
		rounds,
		spikeMS,
		settleMS,
		float64(percentile(samples, 50))/float64(time.Microsecond),
		float64(percentile(samples, 99))/float64(time.Microsecond),
		float64(maximum)/float64(time.Microsecond),
	)
}

func main() {
	workers, err := envPositive("LLAM_CPU_SPIKE_WORKERS", runtime.NumCPU(), 4096)
	if err != nil {
		fmt.Fprintln(os.Stderr, "[cpu-spike]", err)
		os.Exit(2)
	}
	rounds, err := envPositive("LLAM_CPU_SPIKE_ROUNDS", defaultRounds, maxRounds)
	if err != nil {
		fmt.Fprintln(os.Stderr, "[cpu-spike]", err)
		os.Exit(2)
	}
	spikeMS, err := envPositive("LLAM_CPU_SPIKE_MS", defaultSpikeMS, maxSpikeMS)
	if err != nil {
		fmt.Fprintln(os.Stderr, "[cpu-spike]", err)
		os.Exit(2)
	}
	settleMS, err := envPositive("LLAM_CPU_SPIKE_SETTLE_MS", defaultSettleMS, maxSettleMS)
	if err != nil {
		fmt.Fprintln(os.Stderr, "[cpu-spike]", err)
		os.Exit(2)
	}

	runtime.GOMAXPROCS(workers)
	timerSamples := make([]time.Duration, rounds)
	ioSamples := make([]time.Duration, rounds)
	controlSamples := make([]time.Duration, rounds)
	for round := 0; round < rounds; round++ {
		result, roundErr := runRound(workers, spikeMS, settleMS)
		if roundErr != nil {
			fmt.Fprintln(os.Stderr, "[cpu-spike]", roundErr)
			os.Exit(1)
		}
		timerSamples[round] = result.timer
		ioSamples[round] = result.io
		controlSamples[round] = result.control
	}

	fmt.Fprintf(
		os.Stderr,
		"[cpu-spike-config] runtime=Go workers=%d rounds=%d spike_ms=%d settle_ms=%d async_preempt=on polls=0\n",
		workers,
		rounds,
		spikeMS,
		settleMS,
	)
	printReport("timer", timerSamples, workers, rounds, spikeMS, settleMS)
	printReport("io", ioSamples, workers, rounds, spikeMS, settleMS)
	printReport("control", controlSamples, workers, rounds, spikeMS, settleMS)
}
