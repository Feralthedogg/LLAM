// Copyright 2026 Feralthedogg
// SPDX-License-Identifier: Apache-2.0

use std::env;
use std::io;
use std::net::SocketAddr;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex};

use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::{TcpListener, TcpStream};
use tokio::sync::mpsc;

const DEFAULT_PORT: u16 = 7777;
const OUTBOX_CAP: usize = 64;
const READ_BUF: usize = 2048;
const INPUT_CAP: usize = 4096;
const MAX_BROADCAST_TARGETS: usize = 1024;

type Message = Arc<Vec<u8>>;

#[derive(Clone)]
struct Client {
    id: u64,
    tx: mpsc::Sender<Message>,
}

struct Server {
    clients: Mutex<Vec<Client>>,
    next_client_id: AtomicU64,
    quiet: bool,
}

impl Server {
    fn new() -> Self {
        let quiet = env::var("LLAM_CHAT_QUIET").is_ok_and(|value| value != "0");
        Self {
            clients: Mutex::new(Vec::new()),
            next_client_id: AtomicU64::new(1),
            quiet,
        }
    }

    fn alloc_id(&self) -> u64 {
        self.next_client_id.fetch_add(1, Ordering::Relaxed)
    }

    fn add_client(&self, client: Client) {
        self.clients
            .lock()
            .expect("clients mutex poisoned")
            .push(client);
    }

    fn remove_client(&self, id: u64) {
        self.clients
            .lock()
            .expect("clients mutex poisoned")
            .retain(|client| client.id != id);
    }

    fn broadcast(&self, sender_id: u64, message: Message) {
        let targets = {
            let clients = self.clients.lock().expect("clients mutex poisoned");
            clients
                .iter()
                .filter(|client| client.id != sender_id)
                .take(MAX_BROADCAST_TARGETS)
                .cloned()
                .collect::<Vec<_>>()
        };

        for client in targets {
            let _ = client.tx.try_send(Arc::clone(&message));
        }
    }
}

#[tokio::main(flavor = "multi_thread")]
async fn main() -> io::Result<()> {
    let port = parse_port()?;
    let server = Arc::new(Server::new());
    let listener = TcpListener::bind(("127.0.0.1", port)).await?;

    if !server.quiet {
        println!("Tokio chat server listening on 127.0.0.1:{port}");
    }

    loop {
        let (stream, peer) = listener.accept().await?;
        let server = Arc::clone(&server);
        tokio::spawn(async move {
            if let Err(error) = handle_client(server, stream, peer).await {
                eprintln!("client error: {error}");
            }
        });
    }
}

async fn handle_client(server: Arc<Server>, stream: TcpStream, peer: SocketAddr) -> io::Result<()> {
    let id = server.alloc_id();
    let (tx, rx) = mpsc::channel::<Message>(OUTBOX_CAP);
    server.add_client(Client { id, tx });

    if !server.quiet {
        println!("client {id} connected from {peer}");
    }

    let (reader, writer) = stream.into_split();
    let writer_task = tokio::spawn(writer_task(writer, rx));
    let reader_result = reader_task(Arc::clone(&server), id, reader).await;

    server.remove_client(id);
    writer_task.abort();
    let _ = writer_task.await;

    if !server.quiet {
        println!("client {id} disconnected");
    }
    reader_result
}

async fn writer_task(mut writer: tokio::net::tcp::OwnedWriteHalf, mut rx: mpsc::Receiver<Message>) {
    while let Some(message) = rx.recv().await {
        if writer.write_all(&message).await.is_err() {
            break;
        }
    }
}

async fn reader_task(
    server: Arc<Server>,
    id: u64,
    mut reader: tokio::net::tcp::OwnedReadHalf,
) -> io::Result<()> {
    let mut read_buf = [0u8; READ_BUF];
    let mut input = Vec::with_capacity(INPUT_CAP);

    loop {
        let nread = reader.read(&mut read_buf).await?;
        if nread == 0 {
            break;
        }
        process_input(&server, id, &mut input, &read_buf[..nread]);
    }

    if !input.is_empty() {
        broadcast_line(&server, id, &input);
    }
    Ok(())
}

fn process_input(server: &Server, id: u64, input: &mut Vec<u8>, mut data: &[u8]) {
    while !data.is_empty() {
        if input.is_empty() {
            if let Some(pos) = data.iter().rposition(|&byte| byte == b'\n') {
                let span = pos + 1;
                broadcast_line(server, id, &data[..span]);
                data = &data[span..];
                continue;
            }
        }

        let available = INPUT_CAP.saturating_sub(input.len());
        let copy_len = available.min(data.len());
        input.extend_from_slice(&data[..copy_len]);
        data = &data[copy_len..];

        if let Some(pos) = input.iter().rposition(|&byte| byte == b'\n') {
            let span = pos + 1;
            broadcast_line(server, id, &input[..span]);
            input.drain(..span);
        } else if input.len() == INPUT_CAP {
            broadcast_line(server, id, input);
            input.clear();
        }
    }
}

fn broadcast_line(server: &Server, id: u64, line: &[u8]) {
    let mut message = Vec::with_capacity(32 + line.len());

    message.extend_from_slice(b"[client ");
    append_decimal(&mut message, id);
    message.extend_from_slice(b"] ");
    message.extend_from_slice(line);
    server.broadcast(id, Arc::new(message));
}

fn append_decimal(out: &mut Vec<u8>, mut value: u64) {
    let mut buf = [0u8; 20];
    let mut index = buf.len();

    if value == 0 {
        out.push(b'0');
        return;
    }
    while value != 0 {
        index -= 1;
        buf[index] = b'0' + (value % 10) as u8;
        value /= 10;
    }
    out.extend_from_slice(&buf[index..]);
}

fn parse_port() -> io::Result<u16> {
    match env::args().nth(1) {
        Some(value) if value == "--help" || value == "-h" => {
            eprintln!("usage: tokio_chat_server [PORT]");
            std::process::exit(0);
        }
        Some(value) => value.parse::<u16>().map_err(|_| {
            io::Error::new(
                io::ErrorKind::InvalidInput,
                format!("invalid port: {value}"),
            )
        }),
        None => Ok(DEFAULT_PORT),
    }
}
