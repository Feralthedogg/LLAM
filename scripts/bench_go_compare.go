package main

import (
	"fmt"
	"os"
	"runtime"
	"sort"
	"strconv"
	"sync"
	"syscall"
	"time"
	"unsafe"
)

type report struct {
	name        string
	rounds      uint
	opsPerRound uint
	samples     []time.Duration
}

func envU32(name string, def uint, max uint) uint {
	value := os.Getenv(name)
	if value == "" {
		return def
	}
	parsed, err := strconv.ParseUint(value, 10, 32)
	if err != nil || parsed == 0 {
		return def
	}
	if uint(parsed) > max {
		return max
	}
	return uint(parsed)
}

func envU32AllowZero(name string, def uint, max uint) uint {
	value := os.Getenv(name)
	if value == "" {
		return def
	}
	parsed, err := strconv.ParseUint(value, 10, 32)
	if err != nil {
		return def
	}
	if uint(parsed) > max {
		return max
	}
	return uint(parsed)
}

func envI32(name string, def int, min int, max int) int {
	value := os.Getenv(name)
	if value == "" {
		return def
	}
	parsed, err := strconv.Atoi(value)
	if err != nil {
		return def
	}
	if parsed < min {
		return min
	}
	if parsed > max {
		return max
	}
	return parsed
}

func envFlag(name string, def bool) bool {
	value := os.Getenv(name)
	if value == "" {
		return def
	}
	return value != "0"
}

func percentile(samples []time.Duration, pct uint) time.Duration {
	if len(samples) == 0 {
		return 0
	}
	cp := make([]time.Duration, len(samples))
	copy(cp, samples)
	sort.Slice(cp, func(i, j int) bool { return cp[i] < cp[j] })
	index := ((len(cp)-1)*int(pct) + 99) / 100
	if index >= len(cp) {
		index = len(cp) - 1
	}
	return cp[index]
}

func printReport(r report, warmup uint) {
	samples := r.samples

	if warmup > 0 && warmup < uint(len(samples)) {
		samples = samples[warmup:]
	} else {
		warmup = 0
	}
	var total time.Duration
	for _, sample := range samples {
		total += sample
	}
	opsPerSec := 0.0
	if total > 0 {
		opsPerSec = float64(r.opsPerRound) * float64(len(samples)) * float64(time.Second) / float64(total)
	}
	p50 := percentile(samples, 50)
	p99 := percentile(samples, 99)
	fmt.Printf("[go-bench] name=%s rounds=%d warmup=%d ops=%d ops_per_sec=%.2f p50_us=%.2f p99_us=%.2f\n",
		r.name,
		len(samples),
		warmup,
		r.opsPerRound,
		opsPerSec,
		float64(p50)/1000.0,
		float64(p99)/1000.0,
	)
	if envFlag("LLAM_BENCH_CSV", false) {
		fmt.Println("[go-bench-csv] name,rounds,warmup,ops,ops_per_sec,p50_us,p99_us")
		fmt.Printf("[go-bench-csv] %s,%d,%d,%d,%.2f,%.2f,%.2f\n",
			r.name,
			len(samples),
			warmup,
			r.opsPerRound,
			opsPerSec,
			float64(p50)/1000.0,
			float64(p99)/1000.0,
		)
	}
}

func benchCaseSelected(name string) bool {
	only := os.Getenv("LLAM_BENCH_ONLY")

	return only == "" || only == name
}

func runSpawnJoin(rounds, tasksPerRound, yieldsPerTask uint) report {
	samples := make([]time.Duration, rounds)
	for round := uint(0); round < rounds; round++ {
		var wg sync.WaitGroup
		start := time.Now()
		wg.Add(int(tasksPerRound))
		for i := uint(0); i < tasksPerRound; i++ {
			go func() {
				for j := uint(0); j < yieldsPerTask; j++ {
					runtime.Gosched()
				}
				wg.Done()
			}()
		}
		wg.Wait()
		samples[round] = time.Since(start)
	}
	return report{name: "spawn_join", rounds: rounds, opsPerRound: tasksPerRound, samples: samples}
}

func runChannelPingPong(rounds, messagesPerRound uint) report {
	samples := make([]time.Duration, rounds)
	for round := uint(0); round < rounds; round++ {
		request := make(chan uintptr, 1)
		response := make(chan uintptr, 1)
		done := make(chan struct{})
		go func() {
			for i := uint(0); i < messagesPerRound; i++ {
				value := <-request
				response <- value
			}
			close(done)
		}()
		start := time.Now()
		for i := uint(0); i < messagesPerRound; i++ {
			token := uintptr(i + 1)
			request <- token
			if <-response != token {
				panic("channel echo mismatch")
			}
		}
		<-done
		samples[round] = time.Since(start)
	}
	return report{name: "channel_pingpong", rounds: rounds, opsPerRound: messagesPerRound, samples: samples}
}

func runSelectRecvReady(rounds, opsPerRound uint) report {
	samples := make([]time.Duration, rounds)
	for round := uint(0); round < rounds; round++ {
		primary := make(chan uintptr, 1)
		secondary := make(chan uintptr, 1)
		start := time.Now()
		for i := uint(0); i < opsPerRound; i++ {
			token := uintptr(i + 1)
			primary <- token
			select {
			case value := <-primary:
				if value != token {
					panic("select ready mismatch")
				}
			case <-secondary:
				panic("select ready wrong channel")
			}
		}
		samples[round] = time.Since(start)
	}
	return report{name: "select_recv_ready", rounds: rounds, opsPerRound: opsPerRound, samples: samples}
}

func runSelectParkWake(rounds, opsPerRound uint) report {
	samples := make([]time.Duration, rounds)
	for round := uint(0); round < rounds; round++ {
		primary := make(chan uintptr, 1)
		secondary := make(chan uintptr, 1)
		done := make(chan struct{})
		go func() {
			defer close(done)
			for i := uint(0); i < opsPerRound; i++ {
				runtime.Gosched()
				primary <- uintptr(i + 1)
			}
		}()
		start := time.Now()
		for i := uint(0); i < opsPerRound; i++ {
			token := uintptr(i + 1)
			select {
			case value := <-primary:
				if value != token {
					panic("select park-wake mismatch")
				}
			case <-secondary:
				panic("select park-wake wrong channel")
			}
		}
		<-done
		samples[round] = time.Since(start)
	}
	return report{name: "select_park_wake", rounds: rounds, opsPerRound: opsPerRound, samples: samples}
}

func runSelectTimeout(rounds, opsPerRound uint) report {
	samples := make([]time.Duration, rounds)
	for round := uint(0); round < rounds; round++ {
		primary := make(chan uintptr, 1)
		secondary := make(chan uintptr, 1)
		start := time.Now()
		for i := uint(0); i < opsPerRound; i++ {
			select {
			case <-primary:
				panic("select timeout primary ready")
			case <-secondary:
				panic("select timeout secondary ready")
			default:
			}
		}
		samples[round] = time.Since(start)
	}
	return report{name: "select_timeout", rounds: rounds, opsPerRound: opsPerRound, samples: samples}
}

func runIoEcho(rounds, messagesPerRound uint) report {
	samples := make([]time.Duration, rounds)
	for round := uint(0); round < rounds; round++ {
		fds, err := syscall.Socketpair(syscall.AF_UNIX, syscall.SOCK_STREAM, 0)
		if err != nil {
			panic(err)
		}
		done := make(chan struct{})
		go func(fd int) {
			defer close(done)
			defer syscall.Close(fd)
			var one [1]byte
			for i := uint(0); i < messagesPerRound; i++ {
				if _, err := syscall.Read(fd, one[:]); err != nil {
					return
				}
				if _, err := syscall.Write(fd, one[:]); err != nil {
					return
				}
			}
		}(fds[1])
		start := time.Now()
		var one [1]byte
		for i := uint(0); i < messagesPerRound; i++ {
			one[0] = byte(i & 0x7f)
			if _, err := syscall.Write(fds[0], one[:]); err != nil {
				panic(err)
			}
			if _, err := syscall.Read(fds[0], one[:]); err != nil {
				panic(err)
			}
		}
		samples[round] = time.Since(start)
		syscall.Close(fds[0])
		<-done
	}
	return report{name: "io_echo", rounds: rounds, opsPerRound: messagesPerRound, samples: samples}
}

type rawFdSet struct {
	bits [16]uint64
}

func fdSet(fd int, set *rawFdSet) {
	index := fd / 64
	shift := uint(fd % 64)

	if index < 0 || index >= len(set.bits) {
		panic("fd out of fd_set range")
	}
	set.bits[index] |= uint64(1) << shift
}

func rawSelect(n int, rfds *rawFdSet, timeout *syscall.Timeval) error {
	var rfdsPtr uintptr
	var timeoutPtr uintptr
	syscallNumber := uintptr(0)

	if rfds != nil {
		rfdsPtr = uintptr(unsafe.Pointer(rfds))
	}
	if runtime.GOOS == "linux" && runtime.GOARCH == "arm64" {
		var ts syscall.Timespec

		if timeout != nil {
			ts.Sec = timeout.Sec
			ts.Nsec = int64(timeout.Usec) * 1000
			timeoutPtr = uintptr(unsafe.Pointer(&ts))
		}
		_, _, errno := syscall.Syscall6(72, uintptr(n), rfdsPtr, 0, 0, timeoutPtr, 0)
		if errno != 0 {
			return errno
		}
		return nil
	}

	switch runtime.GOOS {
	case "darwin":
		syscallNumber = 93
	case "linux":
		if runtime.GOARCH == "amd64" {
			syscallNumber = 23
		} else if runtime.GOARCH == "386" || runtime.GOARCH == "arm" {
			syscallNumber = 82
		}
	}
	if syscallNumber == 0 {
		return syscall.ENOSYS
	}
	if timeout != nil {
		timeoutPtr = uintptr(unsafe.Pointer(timeout))
	}
	_, _, errno := syscall.Syscall6(syscallNumber, uintptr(n), rfdsPtr, 0, 0, timeoutPtr, 0)
	if errno != 0 {
		return errno
	}
	return nil
}

func runPollWakeApprox(rounds, eventsPerRound uint) report {
	samples := make([]time.Duration, rounds)
	for round := uint(0); round < rounds; round++ {
		fds, err := syscall.Socketpair(syscall.AF_UNIX, syscall.SOCK_STREAM, 0)
		if err != nil {
			panic(err)
		}
		done := make(chan struct{})
		go func(fd int) {
			defer close(done)
			defer syscall.Close(fd)
			var one [1]byte
			for i := uint(0); i < eventsPerRound; i++ {
				runtime.Gosched()
				if _, err := syscall.Write(fd, []byte{'p'}); err != nil {
					return
				}
				one[0] = 0
			}
		}(fds[1])
		start := time.Now()
		var one [1]byte
		for i := uint(0); i < eventsPerRound; i++ {
			var rfds rawFdSet

			fdSet(fds[0], &rfds)
			for {
				err := rawSelect(fds[0]+1, &rfds, nil)
				if err == syscall.EINTR {
					continue
				}
				if err != nil {
					panic(err)
				}
				break
			}
			if _, err := syscall.Read(fds[0], one[:]); err != nil {
				panic(err)
			}
		}
		samples[round] = time.Since(start)
		syscall.Close(fds[0])
		<-done
	}
	return report{name: "poll_wake_approx", rounds: rounds, opsPerRound: eventsPerRound, samples: samples}
}

func runSleepFanout(rounds, tasksPerRound, preSleepYields uint, sleepDur time.Duration) report {
	samples := make([]time.Duration, rounds)
	for round := uint(0); round < rounds; round++ {
		var wg sync.WaitGroup
		start := time.Now()
		wg.Add(int(tasksPerRound))
		for i := uint(0); i < tasksPerRound; i++ {
			go func() {
				for j := uint(0); j < preSleepYields; j++ {
					runtime.Gosched()
				}
				time.Sleep(sleepDur)
				wg.Done()
			}()
		}
		wg.Wait()
		samples[round] = time.Since(start)
	}
	return report{name: "sleep_fanout", rounds: rounds, opsPerRound: tasksPerRound, samples: samples}
}

func blockingSyscallSleep(d time.Duration) {
	for {
		tv := syscall.NsecToTimeval(d.Nanoseconds())
		err := rawSelect(0, nil, &tv)
		if err == syscall.EINTR {
			continue
		}
		if err != nil {
			panic(err)
		}
		return
	}
}

func runBlockingSleepApprox(rounds, scopesPerRound, companionYields uint, sleepDur time.Duration) report {
	samples := make([]time.Duration, rounds)
	for round := uint(0); round < rounds; round++ {
		start := time.Now()
		for i := uint(0); i < scopesPerRound; i++ {
			var wg sync.WaitGroup
			wg.Add(1)
			go func() {
				for j := uint(0); j < companionYields; j++ {
					runtime.Gosched()
				}
				wg.Done()
			}()
			blockingSyscallSleep(sleepDur)
			wg.Wait()
		}
		samples[round] = time.Since(start)
	}
	return report{name: "opaque_syscall_sleep_approx", rounds: rounds, opsPerRound: scopesPerRound, samples: samples}
}

func main() {
	rounds := envU32("LLAM_BENCH_ROUNDS", 21, 512)
	warmupRounds := envU32AllowZero("LLAM_BENCH_WARMUP_ROUNDS", 0, 128)
	totalRounds := rounds + warmupRounds
	spawnTasks := envU32("LLAM_BENCH_SPAWN_TASKS", 128, 4096)
	channelMessages := envU32("LLAM_BENCH_CHANNEL_MESSAGES", 1024, 16384)
	selectOps := envU32("LLAM_BENCH_SELECT_OPS", 512, 16384)
	ioMessages := envU32("LLAM_BENCH_IO_MESSAGES", 256, 8192)
	pollEvents := envU32("LLAM_BENCH_POLL_EVENTS", 256, 8192)
	sleepTasks := envU32("LLAM_BENCH_SLEEP_TASKS", maxU32(spawnTasks, 512), 8192)
	sleepYields := envU32("LLAM_BENCH_SLEEP_YIELDS", 4, 64)
	sleepUS := envU32("LLAM_BENCH_SLEEP_US", 30000, 1000000)
	opaqueScopes := envU32("LLAM_BENCH_OPAQUE_SCOPES", 16, 1024)
	goMaxProcs := envI32("GO_BENCH_GOMAXPROCS", runtime.NumCPU(), 1, 4096)

	runtime.GOMAXPROCS(goMaxProcs)
	fmt.Printf("[go-bench] config rounds=%d warmup=%d gomaxprocs=%d spawn_tasks=%d channel_messages=%d select_ops=%d io_messages=%d poll_events=%d sleep_tasks=%d sleep_yields=%d sleep_us=%d opaque_scopes=%d\n",
		rounds,
		warmupRounds,
		goMaxProcs,
		spawnTasks,
		channelMessages,
		selectOps,
		ioMessages,
		pollEvents,
		sleepTasks,
		sleepYields,
		sleepUS,
		opaqueScopes,
	)

	if benchCaseSelected("spawn_join") {
		printReport(runSpawnJoin(totalRounds, spawnTasks, 2), warmupRounds)
		runtime.GC()
	}
	if benchCaseSelected("channel_pingpong") {
		printReport(runChannelPingPong(totalRounds, channelMessages), warmupRounds)
		runtime.GC()
	}
	if benchCaseSelected("select_recv_ready") {
		printReport(runSelectRecvReady(totalRounds, selectOps), warmupRounds)
		runtime.GC()
	}
	if benchCaseSelected("select_park_wake") {
		printReport(runSelectParkWake(totalRounds, selectOps), warmupRounds)
		runtime.GC()
	}
	if benchCaseSelected("select_timeout") {
		printReport(runSelectTimeout(totalRounds, selectOps), warmupRounds)
		runtime.GC()
	}
	if benchCaseSelected("io_echo") {
		printReport(runIoEcho(totalRounds, ioMessages), warmupRounds)
		runtime.GC()
	}
	if benchCaseSelected("poll_wake") || benchCaseSelected("poll_wake_approx") {
		printReport(runPollWakeApprox(totalRounds, pollEvents), warmupRounds)
		runtime.GC()
	}
	if benchCaseSelected("sleep_fanout") {
		printReport(runSleepFanout(totalRounds, sleepTasks, sleepYields, time.Duration(sleepUS)*time.Microsecond), warmupRounds)
		runtime.GC()
	}
	if benchCaseSelected("opaque_block") || benchCaseSelected("opaque_sleep_approx") {
		printReport(runBlockingSleepApprox(totalRounds, opaqueScopes, 4, 200*time.Microsecond), warmupRounds)
	}
}

func maxU32(a, b uint) uint {
	if a > b {
		return a
	}
	return b
}
