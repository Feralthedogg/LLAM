use std::env;
use std::io::ErrorKind;
use std::thread;
use std::time::{Duration, Instant};

use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::UnixStream;
use tokio::runtime::Builder;
use tokio::sync::mpsc;
use tokio::task;
use tokio::time;

struct Report {
    name: &'static str,
    ops_per_round: u32,
    samples: Vec<Duration>,
}

fn env_u32(name: &str, default_value: u32, max_value: u32) -> u32 {
    match env::var(name)
        .ok()
        .and_then(|value| value.parse::<u32>().ok())
    {
        Some(value) if value > 0 => value.min(max_value),
        _ => default_value,
    }
}

fn env_u32_allow_zero(name: &str, default_value: u32, max_value: u32) -> u32 {
    match env::var(name)
        .ok()
        .and_then(|value| value.parse::<u32>().ok())
    {
        Some(value) => value.min(max_value),
        _ => default_value,
    }
}

fn env_usize(name: &str, default_value: usize, min_value: usize, max_value: usize) -> usize {
    match env::var(name)
        .ok()
        .and_then(|value| value.parse::<usize>().ok())
    {
        Some(value) => value.clamp(min_value, max_value),
        _ => default_value,
    }
}

fn bench_case_selected(name: &str) -> bool {
    match env::var("LLAM_BENCH_ONLY") {
        Ok(only) if !only.is_empty() => only == name,
        _ => true,
    }
}

fn percentile(samples: &[Duration], pct: usize) -> Duration {
    if samples.is_empty() {
        return Duration::ZERO;
    }
    let mut sorted = samples.to_vec();
    sorted.sort();
    let index = ((sorted.len() - 1) * pct + 99) / 100;
    sorted[index.min(sorted.len() - 1)]
}

fn print_report(report: Report, warmup_rounds: u32) {
    let mut warmup = warmup_rounds as usize;
    if warmup >= report.samples.len() {
        warmup = 0;
    }
    let samples = &report.samples[warmup..];
    let total = samples
        .iter()
        .copied()
        .fold(Duration::ZERO, |acc, value| acc + value);
    let ops_per_sec = if total.is_zero() {
        0.0
    } else {
        report.ops_per_round as f64 * samples.len() as f64 / total.as_secs_f64()
    };
    let p50 = percentile(samples, 50);
    let p99 = percentile(samples, 99);

    println!(
        "[tokio-bench] name={} rounds={} warmup={} ops={} ops_per_sec={:.2} p50_us={:.2} p99_us={:.2}",
        report.name,
        samples.len(),
        warmup,
        report.ops_per_round,
        ops_per_sec,
        p50.as_secs_f64() * 1_000_000.0,
        p99.as_secs_f64() * 1_000_000.0,
    );
}

async fn run_spawn_join(rounds: u32, tasks_per_round: u32, yields_per_task: u32) -> Report {
    let mut samples = Vec::with_capacity(rounds as usize);
    for _ in 0..rounds {
        let mut handles = Vec::with_capacity(tasks_per_round as usize);
        let start = Instant::now();
        for _ in 0..tasks_per_round {
            handles.push(task::spawn(async move {
                for _ in 0..yields_per_task {
                    task::yield_now().await;
                }
            }));
        }
        for handle in handles {
            handle.await.expect("spawn_join task panicked");
        }
        samples.push(start.elapsed());
    }
    Report {
        name: "spawn_join",
        ops_per_round: tasks_per_round,
        samples,
    }
}

async fn run_channel_pingpong(rounds: u32, messages_per_round: u32) -> Report {
    let mut samples = Vec::with_capacity(rounds as usize);
    for _ in 0..rounds {
        let (request_tx, mut request_rx) = mpsc::channel::<usize>(1);
        let (response_tx, mut response_rx) = mpsc::channel::<usize>(1);
        let handle = task::spawn(async move {
            for _ in 0..messages_per_round {
                let value = request_rx.recv().await.expect("request channel closed");
                response_tx
                    .send(value)
                    .await
                    .expect("response channel closed");
            }
        });

        let start = Instant::now();
        for i in 0..messages_per_round {
            let token = i as usize + 1;
            request_tx.send(token).await.expect("request send failed");
            let response = response_rx.recv().await.expect("response recv failed");
            assert_eq!(response, token, "channel echo mismatch");
        }
        handle.await.expect("channel task panicked");
        samples.push(start.elapsed());
    }
    Report {
        name: "channel_pingpong",
        ops_per_round: messages_per_round,
        samples,
    }
}

async fn run_io_echo(rounds: u32, messages_per_round: u32) -> Report {
    let mut samples = Vec::with_capacity(rounds as usize);
    for _ in 0..rounds {
        let (mut client, mut server) = UnixStream::pair().expect("UnixStream::pair failed");
        let handle = task::spawn(async move {
            let mut one = [0_u8; 1];
            for _ in 0..messages_per_round {
                server
                    .read_exact(&mut one)
                    .await
                    .expect("server read failed");
                server.write_all(&one).await.expect("server write failed");
            }
        });

        let start = Instant::now();
        let mut one = [0_u8; 1];
        for i in 0..messages_per_round {
            one[0] = (i & 0x7f) as u8;
            client.write_all(&one).await.expect("client write failed");
            client
                .read_exact(&mut one)
                .await
                .expect("client read failed");
        }
        samples.push(start.elapsed());
        drop(client);
        handle.await.expect("io_echo task panicked");
    }
    Report {
        name: "io_echo",
        ops_per_round: messages_per_round,
        samples,
    }
}

async fn run_poll_wake_approx(rounds: u32, events_per_round: u32) -> Report {
    let mut samples = Vec::with_capacity(rounds as usize);
    for _ in 0..rounds {
        let (reader, mut writer) = UnixStream::pair().expect("UnixStream::pair failed");
        let handle = task::spawn(async move {
            for _ in 0..events_per_round {
                task::yield_now().await;
                writer.write_all(&[b'p']).await.expect("poll writer failed");
            }
        });

        let start = Instant::now();
        let mut one = [0_u8; 1];
        for _ in 0..events_per_round {
            loop {
                reader.readable().await.expect("reader readiness failed");
                match reader.try_read(&mut one) {
                    Ok(0) => panic!("poll reader eof"),
                    Ok(_) => break,
                    Err(err) if err.kind() == ErrorKind::WouldBlock => continue,
                    Err(err) => panic!("poll reader failed: {err}"),
                }
            }
        }
        samples.push(start.elapsed());
        drop(reader);
        handle.await.expect("poll writer task panicked");
    }
    Report {
        name: "poll_wake_approx",
        ops_per_round: events_per_round,
        samples,
    }
}

async fn run_sleep_fanout(
    rounds: u32,
    tasks_per_round: u32,
    pre_sleep_yields: u32,
    sleep_dur: Duration,
) -> Report {
    let mut samples = Vec::with_capacity(rounds as usize);
    for _ in 0..rounds {
        let mut handles = Vec::with_capacity(tasks_per_round as usize);
        let start = Instant::now();
        for _ in 0..tasks_per_round {
            handles.push(task::spawn(async move {
                for _ in 0..pre_sleep_yields {
                    task::yield_now().await;
                }
                time::sleep(sleep_dur).await;
            }));
        }
        for handle in handles {
            handle.await.expect("sleep_fanout task panicked");
        }
        samples.push(start.elapsed());
    }
    Report {
        name: "sleep_fanout",
        ops_per_round: tasks_per_round,
        samples,
    }
}

async fn run_opaque_block_approx(
    rounds: u32,
    scopes_per_round: u32,
    companion_yields: u32,
    sleep_dur: Duration,
) -> Report {
    let mut samples = Vec::with_capacity(rounds as usize);
    for _ in 0..rounds {
        let start = Instant::now();
        for _ in 0..scopes_per_round {
            let companion = task::spawn(async move {
                for _ in 0..companion_yields {
                    task::yield_now().await;
                }
            });
            task::block_in_place(|| thread::sleep(sleep_dur));
            companion.await.expect("opaque companion task panicked");
        }
        samples.push(start.elapsed());
    }
    Report {
        name: "opaque_block_in_place_approx",
        ops_per_round: scopes_per_round,
        samples,
    }
}

async fn async_main(worker_threads: usize) {
    let rounds = env_u32("LLAM_BENCH_ROUNDS", 21, 512);
    let warmup_rounds = env_u32_allow_zero("LLAM_BENCH_WARMUP_ROUNDS", 0, 128);
    let total_rounds = rounds + warmup_rounds;
    let spawn_tasks = env_u32("LLAM_BENCH_SPAWN_TASKS", 128, 4096);
    let channel_messages = env_u32("LLAM_BENCH_CHANNEL_MESSAGES", 1024, 16384);
    let io_messages = env_u32("LLAM_BENCH_IO_MESSAGES", 256, 8192);
    let poll_events = env_u32("LLAM_BENCH_POLL_EVENTS", 256, 8192);
    let sleep_tasks = env_u32("LLAM_BENCH_SLEEP_TASKS", spawn_tasks.max(512), 8192);
    let sleep_yields = env_u32("LLAM_BENCH_SLEEP_YIELDS", 4, 64);
    let sleep_us = env_u32("LLAM_BENCH_SLEEP_US", 30000, 1_000_000);
    let opaque_scopes = env_u32("LLAM_BENCH_OPAQUE_SCOPES", 16, 1024);

    println!(
        "[tokio-bench] config rounds={} warmup={} worker_threads={} spawn_tasks={} channel_messages={} io_messages={} poll_events={} sleep_tasks={} sleep_yields={} sleep_us={} opaque_scopes={}",
        rounds,
        warmup_rounds,
        worker_threads,
        spawn_tasks,
        channel_messages,
        io_messages,
        poll_events,
        sleep_tasks,
        sleep_yields,
        sleep_us,
        opaque_scopes,
    );

    if bench_case_selected("spawn_join") {
        print_report(
            run_spawn_join(total_rounds, spawn_tasks, 2).await,
            warmup_rounds,
        );
    }
    if bench_case_selected("channel_pingpong") {
        print_report(
            run_channel_pingpong(total_rounds, channel_messages).await,
            warmup_rounds,
        );
    }
    if bench_case_selected("io_echo") {
        print_report(run_io_echo(total_rounds, io_messages).await, warmup_rounds);
    }
    if bench_case_selected("poll_wake") || bench_case_selected("poll_wake_approx") {
        print_report(
            run_poll_wake_approx(total_rounds, poll_events).await,
            warmup_rounds,
        );
    }
    if bench_case_selected("sleep_fanout") {
        print_report(
            run_sleep_fanout(
                total_rounds,
                sleep_tasks,
                sleep_yields,
                Duration::from_micros(sleep_us as u64),
            )
            .await,
            warmup_rounds,
        );
    }
    if bench_case_selected("opaque_block") || bench_case_selected("opaque_block_in_place_approx") {
        print_report(
            run_opaque_block_approx(total_rounds, opaque_scopes, 4, Duration::from_micros(200))
                .await,
            warmup_rounds,
        );
    }
}

fn main() {
    let default_threads = thread::available_parallelism()
        .map(|value| value.get())
        .unwrap_or(1);
    let worker_threads = env_usize("TOKIO_BENCH_WORKER_THREADS", default_threads, 1, 4096);
    let runtime = Builder::new_multi_thread()
        .worker_threads(worker_threads)
        .enable_all()
        .build()
        .expect("failed to build Tokio runtime");

    runtime.block_on(async_main(worker_threads));
}
