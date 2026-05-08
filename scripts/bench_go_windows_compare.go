//go:build windows

package main

import (
	"fmt"
	"net"
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
	return report{name: "spawn_join", opsPerRound: tasksPerRound, samples: samples}
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
	return report{name: "channel_pingpong", opsPerRound: messagesPerRound, samples: samples}
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
	return report{name: "select_recv_ready", opsPerRound: opsPerRound, samples: samples}
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
	return report{name: "select_park_wake", opsPerRound: opsPerRound, samples: samples}
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
	return report{name: "select_timeout", opsPerRound: opsPerRound, samples: samples}
}

func tcpPair() (*net.TCPConn, *net.TCPConn) {
	listener, err := net.ListenTCP("tcp4", &net.TCPAddr{IP: net.IPv4(127, 0, 0, 1), Port: 0})
	if err != nil {
		panic(err)
	}
	defer listener.Close()

	serverCh := make(chan *net.TCPConn, 1)
	errCh := make(chan error, 1)
	go func() {
		server, err := listener.AcceptTCP()
		if err != nil {
			errCh <- err
			return
		}
		serverCh <- server
	}()

	client, err := net.DialTCP("tcp4", nil, listener.Addr().(*net.TCPAddr))
	if err != nil {
		panic(err)
	}

	var server *net.TCPConn
	select {
	case server = <-serverCh:
	case err = <-errCh:
		client.Close()
		panic(err)
	}

	_ = client.SetNoDelay(true)
	_ = server.SetNoDelay(true)
	return client, server
}

func writeFull(conn *net.TCPConn, data []byte) {
	for len(data) > 0 {
		n, err := conn.Write(data)
		if err != nil {
			panic(err)
		}
		data = data[n:]
	}
}

func readFull(conn *net.TCPConn, data []byte) {
	for len(data) > 0 {
		n, err := conn.Read(data)
		if err != nil {
			panic(err)
		}
		data = data[n:]
	}
}

func runIoEcho(rounds, messagesPerRound uint) report {
	samples := make([]time.Duration, rounds)
	for round := uint(0); round < rounds; round++ {
		client, server := tcpPair()
		done := make(chan struct{})
		go func() {
			defer close(done)
			defer server.Close()
			var one [1]byte
			for i := uint(0); i < messagesPerRound; i++ {
				readFull(server, one[:])
				writeFull(server, one[:])
			}
		}()

		start := time.Now()
		var one [1]byte
		for i := uint(0); i < messagesPerRound; i++ {
			one[0] = byte(i & 0x7f)
			writeFull(client, one[:])
			readFull(client, one[:])
		}
		samples[round] = time.Since(start)
		client.Close()
		<-done
	}
	return report{name: "io_echo", opsPerRound: messagesPerRound, samples: samples}
}

func runPollWakeApprox(rounds, eventsPerRound uint) report {
	samples := make([]time.Duration, rounds)
	for round := uint(0); round < rounds; round++ {
		reader, writer := tcpPair()
		done := make(chan struct{})
		go func() {
			defer close(done)
			defer writer.Close()
			for i := uint(0); i < eventsPerRound; i++ {
				runtime.Gosched()
				writeFull(writer, []byte{'p'})
			}
		}()

		start := time.Now()
		var one [1]byte
		for i := uint(0); i < eventsPerRound; i++ {
			readFull(reader, one[:])
		}
		samples[round] = time.Since(start)
		reader.Close()
		<-done
	}
	return report{name: "poll_wake_approx", opsPerRound: eventsPerRound, samples: samples}
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
	return report{name: "sleep_fanout", opsPerRound: tasksPerRound, samples: samples}
}

var ntDelayExecution = syscall.NewLazyDLL("ntdll.dll").NewProc("NtDelayExecution")

func blockingSyscallSleep(d time.Duration) {
	if d <= 0 {
		return
	}
	interval := -int64(d / (100 * time.Nanosecond))
	status, _, _ := ntDelayExecution.Call(0, uintptr(unsafe.Pointer(&interval)))
	if status != 0 {
		panic(syscall.Errno(status))
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
	return report{name: "opaque_syscall_sleep_approx", opsPerRound: scopesPerRound, samples: samples}
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
