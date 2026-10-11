//! Candidate in-process host. This module is opt-in and is not part of the
//! standalone companion. The containing module is pinned before UI startup:
//! upstream Wry/Tao retain process-scoped window classes and helper callbacks.
use serde::Serialize;
use serde_json::{json, Value};
use std::ffi::c_void;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::sync::{atomic::{AtomicBool, AtomicIsize, Ordering}, mpsc, Arc, Mutex, OnceLock};
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant};
use tauri::Emitter;
use tokio::sync::oneshot;
use windows_sys::Win32::System::LibraryLoader::{GetModuleHandleExW, GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, GET_MODULE_HANDLE_EX_FLAG_PIN};
use windows_sys::Win32::UI::Input::{GetRegisteredRawInputDevices, RegisterRawInputDevices, RAWINPUTDEVICE, RIDEV_REMOVE};
use windows_sys::Win32::UI::WindowsAndMessaging::{IsIconic, IsWindowVisible};

const MAX_MESSAGE: usize = 8 * 1024 * 1024;
const REQUEST_LIMIT: Duration = Duration::from_secs(3);
pub type RequestFn = unsafe extern "C" fn(*mut c_void, *const u8, usize, *mut u8, usize, *mut usize) -> i32;
static ACTIVE: AtomicBool = AtomicBool::new(false);
static STOPPING: AtomicBool = AtomicBool::new(false);
static LIFECYCLE: Mutex<()> = Mutex::new(());
static CONTROL: Mutex<Option<Control>> = Mutex::new(None);
static LAST_ERROR: Mutex<String> = Mutex::new(String::new());
static STOP_INPUT: Mutex<Option<Vec<RawRegistration>>> = Mutex::new(None);
static EXECUTOR: OnceLock<tokio::runtime::Runtime> = OnceLock::new();

fn error_text(text: impl Into<String>) {
    if let Ok(mut error) = LAST_ERROR.lock() { *error = text.into(); }
}

#[derive(Debug, Serialize)]
struct BridgeError { code: &'static str, message: String }
impl BridgeError {
    fn new(code: &'static str, message: impl Into<String>) -> Self { Self { code, message: message.into() } }
}
type Reply = Result<Value, BridgeError>;
struct Job { request: Value, reply: oneshot::Sender<Reply>, deadline: Instant }
struct EmbeddedBridge {
    sender: mpsc::SyncSender<Job>,
    stopping: Arc<AtomicBool>,
    connected: Arc<AtomicBool>,
    ready: Arc<AtomicBool>,
    window: Arc<AtomicIsize>,
}
struct Worker { bridge: Arc<EmbeddedBridge>, thread: Option<JoinHandle<()>> }
struct Control { app: tauri::AppHandle, window: tauri::WebviewWindow, bridge: Arc<EmbeddedBridge> }

unsafe fn call_native(callback: RequestFn, context: usize, request: &Value, output: &mut [u8]) -> Reply {
    let input = serde_json::to_vec(request).map_err(|_| BridgeError::new("invalid_request", "The request could not be encoded."))?;
    if input.len() >= MAX_MESSAGE { return Err(BridgeError::new("message_too_large", "The request exceeds the local bridge limit.")); }
    let mut length = 0usize;
    // The owner guarantees the context survives until velora_gui_run returns,
    // and the callback is noexcept with a two-second maximum response time.
    let status = callback(context as *mut c_void, input.as_ptr(), input.len(), output.as_mut_ptr(), output.len(), &mut length);
    if status != 0 { return Err(BridgeError::new("native_error", "The native settings bridge could not complete the request.")); }
    if length == 0 || length > output.len() { return Err(BridgeError::new("invalid_response", "The native bridge returned an invalid response size.")); }
    serde_json::from_slice(&output[..length]).map_err(|_| BridgeError::new("invalid_response", "The native bridge returned unreadable data."))
}

impl Worker {
    fn start(callback: RequestFn, context: usize) -> Result<Self, String> {
        let (sender, receiver) = mpsc::sync_channel::<Job>(16);
        let bridge = Arc::new(EmbeddedBridge { sender, stopping: Arc::new(AtomicBool::new(false)), connected: Arc::new(AtomicBool::new(false)), ready: Arc::new(AtomicBool::new(false)), window: Arc::new(AtomicIsize::new(0)) });
        let state = bridge.clone();
        let thread = thread::Builder::new().name("velora-native-bridge".into()).spawn(move || {
            let result = catch_unwind(AssertUnwindSafe(|| {
                let mut output = vec![0u8; MAX_MESSAGE];
                let mut heartbeat = Instant::now() - Duration::from_secs(2);
                let mut last_visibility = None;
                let mut session_started = false;
                while !state.stopping.load(Ordering::Acquire) {
                    // No native ownership or callbacks until the window exists.
                    // A failed/canceled startup can then unwind independently.
                    if !state.ready.load(Ordering::Acquire) { thread::sleep(Duration::from_millis(25)); continue; }
                    let hwnd = state.window.load(Ordering::Relaxed) as *mut c_void;
                    let visible = !hwnd.is_null() && unsafe { IsWindowVisible(hwnd) != 0 && IsIconic(hwnd) == 0 };
                    if last_visibility != Some(visible) || heartbeat.elapsed() >= Duration::from_millis(700) {
                        session_started = true;
                        let reply = unsafe { call_native(callback, context, &json!({"op":"session","active":true,"visible":visible}), &mut output) };
                        state.connected.store(reply.is_ok(), Ordering::Release);
                        heartbeat = Instant::now();
                        last_visibility = Some(visible);
                    }
                    match receiver.recv_timeout(Duration::from_millis(25)) {
                        Ok(job) => {
                            if state.stopping.load(Ordering::Acquire) || job.reply.is_closed() || Instant::now() >= job.deadline {
                                let _ = job.reply.send(Err(BridgeError::new("stopping", "The settings window is closing.")));
                                continue;
                            }
                            let reply = unsafe { call_native(callback, context, &job.request, &mut output) };
                            state.connected.store(reply.is_ok(), Ordering::Release);
                            let _ = job.reply.send(reply);
                        }
                        Err(mpsc::RecvTimeoutError::Timeout) => {}
                        Err(mpsc::RecvTimeoutError::Disconnected) => break,
                    }
                }
                while let Ok(job) = receiver.try_recv() {
                    let _ = job.reply.send(Err(BridgeError::new("stopping", "The settings window is closing.")));
                }
                if session_started { let _ = unsafe { call_native(callback, context, &json!({"op":"session","active":false,"visible":false}), &mut output) }; }
            }));
            state.connected.store(false, Ordering::Release);
            if result.is_err() { error_text("The native bridge worker stopped unexpectedly."); }
        }).map_err(|error| error.to_string())?;
        Ok(Self { bridge, thread: Some(thread) })
    }
}
impl Drop for Worker {
    fn drop(&mut self) {
        self.bridge.stopping.store(true, Ordering::Release);
        self.bridge.window.store(0, Ordering::Release);
        if let Some(thread) = self.thread.take() {
            if thread.join().is_err() { error_text("The native bridge worker could not finish cleanly."); }
        }
    }
}

#[tauri::command]
async fn bridge_request(request: Value, state: tauri::State<'_, Arc<EmbeddedBridge>>) -> Reply {
    if !request.is_object() { return Err(BridgeError::new("invalid_request", "A request must be an object.")); }
    if state.stopping.load(Ordering::Acquire) { return Err(BridgeError::new("stopping", "The settings window is closing.")); }
    let (reply, receive) = oneshot::channel();
    state.sender.try_send(Job { request, reply, deadline: Instant::now() + REQUEST_LIMIT })
        .map_err(|_| BridgeError::new("busy", "The native bridge is applying another change."))?;
    match tokio::time::timeout(REQUEST_LIMIT, receive).await {
        Ok(Ok(reply)) => reply,
        Ok(Err(_)) => Err(BridgeError::new("stopping", "The settings window is closing.")),
        Err(_) => Err(BridgeError::new("timeout", "The game did not respond to the change.")),
    }
}
#[tauri::command]
fn bridge_status(state: tauri::State<'_, Arc<EmbeddedBridge>>) -> Value {
    json!({"connected":state.connected.load(Ordering::Acquire),"embedded":true})
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct RawRegistration { page: u16, usage: u16, flags: u32, window: usize }
fn raw_input_snapshot() -> Result<Vec<RawRegistration>, String> {
    let mut count = 0;
    let size = std::mem::size_of::<RAWINPUTDEVICE>() as u32;
    if unsafe { GetRegisteredRawInputDevices(std::ptr::null_mut(), &mut count, size) } == u32::MAX {
        return Err("Could not preserve the host raw-input registration.".into());
    }
    if count > 1024 { return Err("Unexpected host raw-input registration count.".into()); }
    if count == 0 { return Ok(Vec::new()); }
    let mut records = vec![RAWINPUTDEVICE::default(); count as usize];
    let result = unsafe { GetRegisteredRawInputDevices(records.as_mut_ptr(), &mut count, size) };
    if result == u32::MAX { return Err("Could not read the host raw-input registration.".into()); }
    records.truncate(result as usize);
    Ok(records.into_iter().map(|r| RawRegistration { page:r.usUsagePage, usage:r.usUsage, flags:r.dwFlags, window:r.hwndTarget as usize }).collect())
}
fn restore_raw_input(snapshot: &[RawRegistration]) -> Result<(), String> {
    let current = raw_input_snapshot()?;
    let mut changes: Vec<RAWINPUTDEVICE> = current.iter().filter(|r| !snapshot.iter().any(|saved| saved.page == r.page && saved.usage == r.usage))
        .map(|r| RAWINPUTDEVICE { usUsagePage:r.page, usUsage:r.usage, dwFlags:RIDEV_REMOVE, hwndTarget:std::ptr::null_mut() }).collect();
    changes.extend(snapshot.iter().map(|r| RAWINPUTDEVICE { usUsagePage:r.page, usUsage:r.usage, dwFlags:r.flags, hwndTarget:r.window as *mut c_void }));
    if !changes.is_empty() && unsafe { RegisterRawInputDevices(changes.as_ptr(), changes.len() as u32, std::mem::size_of::<RAWINPUTDEVICE>() as u32) } == 0 {
        return Err("Could not restore the host raw-input registration.".into());
    }
    Ok(())
}
struct InputGuard { initial: Vec<RawRegistration>, restored: bool, entered_loop: bool }
impl InputGuard {
    fn restore(&mut self) -> Result<(), String> { restore_raw_input(&self.initial)?; self.restored = true; Ok(()) }
    fn restore_stop(&mut self) -> Result<(), String> {
        let mut snapshot = STOP_INPUT.lock().map_err(|_| "Could not restore the host raw-input registration.".to_string())?;
        if let Some(saved) = snapshot.as_ref() { restore_raw_input(saved)?; }
        *snapshot = None;
        Ok(())
    }
}
impl Drop for InputGuard {
    fn drop(&mut self) {
        // On startup failure App/runtime are dropped before this guard. Restore
        // once more after that cleanup, even if the earlier build restore passed.
        if !self.restored || !self.entered_loop { if let Err(error) = restore_raw_input(&self.initial) { error_text(error); } }
        if let Err(error) = self.restore_stop() { error_text(error); }
    }
}

struct RunningGuard;
impl Drop for RunningGuard {
    fn drop(&mut self) {
        if let Ok(mut control) = CONTROL.lock() { *control = None; }
        ACTIVE.store(false, Ordering::Release);
    }
}
fn executor() -> Result<(), String> {
    if EXECUTOR.get().is_none() {
        let runtime = tokio::runtime::Builder::new_multi_thread().worker_threads(2).max_blocking_threads(2)
            .thread_name("velora-async").enable_all().build().map_err(|error| error.to_string())?;
        tauri::async_runtime::set(runtime.handle().clone());
        let _ = EXECUTOR.set(runtime);
    }
    Ok(())
}

fn run_inner(callback: RequestFn, context: usize) -> Result<(), String> {
    let mut module = std::ptr::null_mut();
    if unsafe { GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        velora_gui_run as *const () as *const u16, &mut module) } == 0 {
        return Err("The embedded UI module could not be retained safely.".into());
    }
    // WebView2 can present a blocking native error dialog for an invalid profile
    // override. Reject that known bad input without changing the host environment.
    if let Some(profile) = std::env::var_os("WEBVIEW2_USER_DATA_FOLDER").filter(|value| !value.is_empty()) {
        match std::fs::metadata(profile) {
            Ok(metadata) if !metadata.is_dir() => return Err("WEBVIEW2_USER_DATA_FOLDER must be a directory.".into()),
            Ok(_) => {}
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => {}
            Err(error) => return Err(format!("The WebView profile directory is unavailable: {error}")),
        }
    }
    executor()?;
    let mut input = InputGuard { initial: raw_input_snapshot()?, restored: false, entered_loop: false };
    let worker = Worker::start(callback, context)?;
    let bridge = worker.bridge.clone();
    let mut context = tauri::generate_context!();
    context.config_mut().identifier = "app.velora.embedded".into();
    #[cfg(feature = "bridge-test")]
    {
        context.config_mut().identifier = "app.velora.embedded.test".into();
        for window in &mut context.config_mut().app.windows { window.visible = false; window.always_on_top = false; window.devtools = Some(true); }
    }
    let window_config = context.config().app.windows.iter().find(|window| window.label == "main")
        .cloned().ok_or_else(|| "The embedded window configuration is missing.".to_string())?;
    // Tauri's automatic Ready/setup path panics if WebView creation fails.
    // Construct the window explicitly below, where errors can return to C++.
    context.config_mut().app.windows.clear();
    let app = tauri::Builder::default().any_thread().device_event_filter(tauri::DeviceEventFilter::Always)
        .manage(bridge.clone())
        .invoke_handler(tauri::generate_handler![bridge_request, bridge_status])
        .on_window_event(|window, event| {
            if let tauri::WindowEvent::CloseRequested { api, .. } = event {
                // Native stop uses destroy(), which bypasses CloseRequested.
                // A concurrent user close must never overtake stop's input snapshot.
                api.prevent_close();
                if !STOPPING.load(Ordering::Acquire) { let _ = window.minimize(); }
            }
        }).build(context).map_err(|error| error.to_string())?;
    // Both upstream raw-input registration calls occur in Builder::build.
    // Restore before creating/showing webviews or entering the UI event loop.
    input.restore()?;
    if STOPPING.load(Ordering::Acquire) { return Ok(()); }
    let window = tauri::WebviewWindowBuilder::from_config(&app, &window_config)
        .map_err(|error| error.to_string())?.build().map_err(|error| error.to_string())?;
    let hwnd = window.hwnd().map_err(|error| error.to_string())?;
    bridge.window.store(hwnd.0 as isize, Ordering::Release);
    if let Ok(mut control) = CONTROL.lock() { *control = Some(Control { app:app.handle().clone(), window:window.clone(), bridge:bridge.clone() }); }
    if STOPPING.load(Ordering::Acquire) { bridge.stopping.store(true, Ordering::Release); let _ = window.destroy(); }
    else { bridge.ready.store(true, Ordering::Release); }
    input.entered_loop = true;
    app.run_return(|_app, _event| {});
    bridge.stopping.store(true, Ordering::Release);
    bridge.window.store(0, Ordering::Release);
    if let Ok(mut control) = CONTROL.lock() { *control = None; }
    drop(worker); // Callback/context ownership is released only after this join.
    input.restore_stop()?;
    Ok(())
}

/// Blocking entry point for a dedicated, non-loader-lock UI thread.
/// The callback/context remain valid until this function returns. The containing
/// DLL is intentionally pinned; Stop closes the UI, it does not permit unmapping.
#[no_mangle]
pub extern "C" fn velora_gui_run(callback: Option<RequestFn>, context: *mut c_void) -> i32 {
    let Some(callback) = callback else { return -1; };
    {
        let Ok(_gate) = LIFECYCLE.lock() else { return -3; };
        if ACTIVE.load(Ordering::Acquire) { return -2; }
        STOPPING.store(false, Ordering::Release);
        if let Ok(mut snapshot) = STOP_INPUT.lock() { *snapshot = None; }
        // Publish running only once early-stop state is ready.
        ACTIVE.store(true, Ordering::Release);
    }
    let _running = RunningGuard;
    error_text("");
    match catch_unwind(AssertUnwindSafe(|| run_inner(callback, context as usize))) {
        Ok(Ok(())) => 0,
        Ok(Err(error)) => { error_text(error); -3 }
        Err(_) => { error_text("The embedded UI could not complete its lifecycle."); -4 }
    }
}

#[no_mangle]
pub extern "C" fn velora_gui_stop() {
    let _ = catch_unwind(AssertUnwindSafe(|| {
        {
            let Ok(_gate) = LIFECYCLE.lock() else { return; };
            if !ACTIVE.load(Ordering::Acquire) { return; }
            if STOPPING.swap(true, Ordering::AcqRel) { return; }
        }
        let window = if let Ok(control) = CONTROL.lock() {
            control.as_ref().map(|control| { control.bridge.stopping.store(true, Ordering::Release); control.window.clone() })
        } else { None };
        if let Some(window) = window {
            // CONTROL is published after the startup input guard has
            // restored the host. An earlier stop must not capture Tao's temporary
            // registration changes in Builder::build.
            if let Ok(snapshot) = raw_input_snapshot() { if let Ok(mut stop_input) = STOP_INPUT.lock() { *stop_input = Some(snapshot); } }
            let _ = window.destroy();
        }
    }));
}
#[no_mangle]
pub extern "C" fn velora_gui_toggle() {
    let _ = catch_unwind(AssertUnwindSafe(|| {
        if STOPPING.load(Ordering::Acquire) { return; }
        let control = CONTROL.lock().ok().and_then(|control| control.as_ref().map(|control| (control.app.clone(), control.window.clone(), control.bridge.window.load(Ordering::Acquire))));
        if let Some((app, window, hwnd)) = control {
                // Read HWND state directly instead of waiting on the UI thread.
                let hwnd = hwnd as *mut c_void;
                let visible = !hwnd.is_null() && unsafe { IsWindowVisible(hwnd) != 0 && IsIconic(hwnd) == 0 };
                if visible { let _ = window.minimize(); }
                else { let _ = window.unminimize(); let _ = window.show(); let _ = window.set_focus(); }
                let _ = app.emit("velora-visibility", json!({"visible":!visible}));
        }
    }));
}
#[no_mangle]
pub extern "C" fn velora_gui_running() -> i32 { i32::from(ACTIVE.load(Ordering::Acquire)) }
/// Returns the full error byte length; writes a truncated NUL-terminated copy.
#[no_mangle]
pub unsafe extern "C" fn velora_gui_last_error(output: *mut u8, capacity: usize) -> usize {
    catch_unwind(AssertUnwindSafe(|| {
        let Ok(error) = LAST_ERROR.lock() else { return 0; };
        if !output.is_null() && capacity != 0 {
            let count = error.len().min(capacity - 1);
            std::ptr::copy_nonoverlapping(error.as_ptr(), output, count);
            *output.add(count) = 0;
        }
        error.len()
    })).unwrap_or(0)
}
