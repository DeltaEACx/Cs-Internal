use serde::Serialize;
use serde_json::Value;
use std::sync::atomic::{AtomicU32, Ordering};
use std::time::Duration;
use tokio::sync::Mutex;

const MAX_MESSAGE_BYTES: usize = 8 * 1024 * 1024;
const REQUEST_TIMEOUT: Duration = Duration::from_secs(3);

#[derive(Default)]
pub struct Bridge {
    gate: Mutex<()>,
    pub connected_pid: AtomicU32,
    fixture_pid: Option<u32>,
}

#[derive(Debug, Serialize)]
pub struct BridgeError {
    pub code: &'static str,
    pub message: String,
}

impl BridgeError {
    fn new(code: &'static str, message: impl Into<String>) -> Self {
        Self {
            code,
            message: message.into(),
        }
    }
}

fn encode_request(request: &Value) -> Result<Vec<u8>, BridgeError> {
    if !request.is_object() {
        return Err(BridgeError::new(
            "invalid_request",
            "A request must be a JSON object.",
        ));
    }
    let mut data = serde_json::to_vec(request)
        .map_err(|_| BridgeError::new("invalid_request", "The request could not be encoded."))?;
    if data.len() >= MAX_MESSAGE_BYTES {
        return Err(BridgeError::new(
            "message_too_large",
            "The request exceeds the local bridge limit.",
        ));
    }
    data.push(b'\n');
    Ok(data)
}

impl Bridge {
    pub fn new() -> Self {
        #[cfg(any(debug_assertions, feature = "bridge-test"))]
        let fixture_pid = {
            let args: Vec<_> = std::env::args().collect();
            args.windows(2)
                .find(|pair| pair[0] == "--bridge-pid")
                .and_then(|pair| pair[1].parse::<u32>().ok())
                .filter(|pid| *pid > 0)
        };
        #[cfg(not(any(debug_assertions, feature = "bridge-test")))]
        let fixture_pid = None;
        Self {
            gate: Mutex::new(()),
            connected_pid: AtomicU32::new(0),
            fixture_pid,
        }
    }

    pub async fn request(&self, request: Value) -> Result<Value, BridgeError> {
        let data = encode_request(&request)?;
        // Serialize requests so a single-instance native pipe cannot reorder slider edits.
        let result = tokio::time::timeout(REQUEST_TIMEOUT, async {
            let _guard = self.gate.lock().await;
            self.discover_and_exchange(&data).await
        })
        .await;
        match result {
            Ok(Ok((pid, value))) => {
                self.connected_pid.store(pid, Ordering::Relaxed);
                Ok(value)
            }
            Ok(Err(error)) => {
                self.connected_pid.store(0, Ordering::Relaxed);
                Err(error)
            }
            Err(_) => {
                self.connected_pid.store(0, Ordering::Relaxed);
                Err(BridgeError::new(
                    "timeout",
                    "The game did not respond. Reconnect after loading the updated DLL.",
                ))
            }
        }
    }

    #[cfg(windows)]
    async fn discover_and_exchange(&self, data: &[u8]) -> Result<(u32, Value), BridgeError> {
        let mut pids = if let Some(pid) = self.fixture_pid {
            vec![pid]
        } else {
            game_processes()?
        };
        let preferred = self.connected_pid.load(Ordering::Relaxed);
        pids.sort_unstable_by_key(|pid| (*pid != preferred, std::cmp::Reverse(*pid)));
        if pids.is_empty() {
            return Err(BridgeError::new(
                "game_not_running",
                "Open Counter-Strike 2 to connect.",
            ));
        }
        for pid in pids {
            let name = format!(r"\\.\pipe\velora_gui_v1_{pid}");
            match exchange(&name, pid, data).await {
                Ok(value) => return Ok((pid, value)),
                Err(error) if error.code == "not_found" => continue,
                // A timed-out mutation is never replayed: the game may have applied it.
                Err(error) => return Err(error),
            }
        }
        Err(BridgeError::new(
            "not_connected",
            "Load DeltaEAC.dll to connect your settings.",
        ))
    }

    #[cfg(not(windows))]
    async fn discover_and_exchange(&self, _data: &[u8]) -> Result<(u32, Value), BridgeError> {
        Err(BridgeError::new(
            "unsupported_platform",
            "The game connection is available on Windows.",
        ))
    }
}

#[cfg(windows)]
fn game_processes() -> Result<Vec<u32>, BridgeError> {
    use windows_sys::Win32::Foundation::{CloseHandle, INVALID_HANDLE_VALUE};
    use windows_sys::Win32::System::Diagnostics::ToolHelp::{
        CreateToolhelp32Snapshot, Process32FirstW, Process32NextW, PROCESSENTRY32W,
        TH32CS_SNAPPROCESS,
    };
    // Snapshot enumeration reads process names and IDs; it never opens game memory.
    unsafe {
        let snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if snapshot == INVALID_HANDLE_VALUE {
            return Err(BridgeError::new(
                "discovery_failed",
                "The running game could not be located.",
            ));
        }
        let mut entry: PROCESSENTRY32W = std::mem::zeroed();
        entry.dwSize = std::mem::size_of::<PROCESSENTRY32W>() as u32;
        let mut pids = Vec::new();
        let mut found = Process32FirstW(snapshot, &mut entry);
        while found != 0 {
            let end = entry
                .szExeFile
                .iter()
                .position(|c| *c == 0)
                .unwrap_or(entry.szExeFile.len());
            if String::from_utf16_lossy(&entry.szExeFile[..end]).eq_ignore_ascii_case("cs2.exe") {
                pids.push(entry.th32ProcessID);
            }
            found = Process32NextW(snapshot, &mut entry);
        }
        CloseHandle(snapshot);
        Ok(pids)
    }
}

#[cfg(windows)]
async fn exchange(name: &str, expected_pid: u32, data: &[u8]) -> Result<Value, BridgeError> {
    use std::os::windows::io::AsRawHandle;
    use tokio::io::{AsyncBufReadExt, AsyncReadExt, AsyncWriteExt, BufReader};
    use tokio::net::windows::named_pipe::ClientOptions;
    use windows_sys::Win32::Foundation::ERROR_PIPE_BUSY;
    use windows_sys::Win32::System::Pipes::GetNamedPipeServerProcessId;

    let mut client = loop {
        match ClientOptions::new().open(name) {
            Ok(client) => break client,
            Err(error) if error.raw_os_error() == Some(ERROR_PIPE_BUSY as i32) => {
                tokio::time::sleep(Duration::from_millis(20)).await;
            }
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => {
                return Err(BridgeError::new(
                    "not_found",
                    "The game bridge is not available yet.",
                ));
            }
            Err(error) => {
                return Err(BridgeError::new(
                    "connection_failed",
                    format!("The local game connection could not be opened: {error}"),
                ))
            }
        }
    };
    let mut server_pid = 0;
    if unsafe { GetNamedPipeServerProcessId(client.as_raw_handle(), &mut server_pid) } == 0
        || server_pid != expected_pid
    {
        return Err(BridgeError::new(
            "wrong_server",
            "The local connection does not belong to the running game.",
        ));
    }
    client.write_all(data).await.map_err(|_| {
        BridgeError::new(
            "disconnected",
            "The game disconnected while applying the change.",
        )
    })?;
    let mut response = Vec::with_capacity(4096);
    // `take` bounds allocation even if a broken server never sends its newline.
    let mut reader = BufReader::new(client.take((MAX_MESSAGE_BYTES + 1) as u64));
    reader.read_until(b'\n', &mut response).await.map_err(|_| {
        BridgeError::new(
            "disconnected",
            "The game disconnected before returning its settings.",
        )
    })?;
    decode_response(&response)
}

fn decode_response(response: &[u8]) -> Result<Value, BridgeError> {
    if response.len() > MAX_MESSAGE_BYTES {
        return Err(BridgeError::new(
            "message_too_large",
            "The game returned more data than the bridge allows.",
        ));
    }
    if response.last() != Some(&b'\n') {
        return Err(BridgeError::new(
            "incomplete_response",
            "The game returned an incomplete response.",
        ));
    }
    serde_json::from_slice(response).map_err(|_| {
        BridgeError::new(
            "invalid_response",
            "The game returned an unreadable response.",
        )
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn framing_preserves_unicode_and_escapes_newlines() {
        let value = json!({"op":"set", "value":"桜\n雪"});
        let encoded = encode_request(&value).unwrap();
        assert_eq!(encoded.iter().filter(|c| **c == b'\n').count(), 1);
        assert_eq!(decode_response(&encoded).unwrap(), value);
    }

    #[test]
    fn rejects_unbounded_or_incomplete_frames() {
        assert_eq!(
            encode_request(&json!("not an object")).unwrap_err().code,
            "invalid_request"
        );
        assert_eq!(
            decode_response(b"{\"ok\":true}").unwrap_err().code,
            "incomplete_response"
        );
        assert_eq!(
            decode_response(b"invalid\n").unwrap_err().code,
            "invalid_response"
        );
        let oversized = vec![b'x'; MAX_MESSAGE_BYTES + 1];
        assert_eq!(
            decode_response(&oversized).unwrap_err().code,
            "message_too_large"
        );
    }

    #[cfg(windows)]
    #[tokio::test]
    async fn named_pipe_roundtrip_checks_server_and_handles_fragmented_json() {
        use tokio::io::{AsyncBufReadExt, AsyncWriteExt, BufReader};
        use tokio::net::windows::named_pipe::ServerOptions;
        let pid = std::process::id();
        let name = format!(r"\\.\pipe\velora_gui_test_{pid}_roundtrip");
        let server = ServerOptions::new()
            .first_pipe_instance(true)
            .create(&name)
            .unwrap();
        let worker = tokio::spawn(async move {
            server.connect().await.unwrap();
            let mut reader = BufReader::new(server);
            let mut request = String::new();
            reader.read_line(&mut request).await.unwrap();
            assert_eq!(
                serde_json::from_str::<Value>(&request).unwrap(),
                json!({"op":"snapshot"})
            );
            reader.get_mut().write_all(b"{\"ok\":true,").await.unwrap();
            tokio::time::sleep(Duration::from_millis(5)).await;
            reader
                .get_mut()
                .write_all(b"\"value\":42}\n")
                .await
                .unwrap();
        });
        let reply = tokio::time::timeout(
            REQUEST_TIMEOUT,
            exchange(&name, pid, b"{\"op\":\"snapshot\"}\n"),
        )
        .await
        .unwrap()
        .unwrap();
        assert_eq!(reply, json!({"ok":true,"value":42}));
        worker.await.unwrap();
    }

    #[cfg(windows)]
    #[tokio::test]
    async fn rejects_pipe_owned_by_an_unexpected_process() {
        use tokio::net::windows::named_pipe::ServerOptions;
        let pid = std::process::id();
        let name = format!(r"\\.\pipe\velora_gui_test_{pid}_wrong_pid");
        let _server = ServerOptions::new()
            .first_pipe_instance(true)
            .create(&name)
            .unwrap();
        let error = exchange(&name, pid.wrapping_add(1), b"{}\n")
            .await
            .unwrap_err();
        assert_eq!(error.code, "wrong_server");
    }

    #[cfg(windows)]
    #[tokio::test]
    async fn stalled_server_is_cancelled_by_deadline() {
        use tokio::net::windows::named_pipe::ServerOptions;
        let pid = std::process::id();
        let name = format!(r"\\.\pipe\velora_gui_test_{pid}_timeout");
        let server = ServerOptions::new()
            .first_pipe_instance(true)
            .create(&name)
            .unwrap();
        let worker = tokio::spawn(async move {
            server.connect().await.unwrap();
            tokio::time::sleep(Duration::from_secs(1)).await;
        });
        let result =
            tokio::time::timeout(Duration::from_millis(40), exchange(&name, pid, b"{}\n")).await;
        assert!(result.is_err());
        worker.abort();
    }
}
