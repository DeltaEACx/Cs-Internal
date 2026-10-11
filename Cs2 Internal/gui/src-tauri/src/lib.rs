#[cfg(feature = "standalone")]
mod bridge;
#[cfg(all(windows, feature = "standalone"))]
mod hotkey;
#[cfg(all(windows, feature = "embedded"))]
mod embedded;

#[cfg(feature = "standalone")]
use bridge::{Bridge, BridgeError};
#[cfg(feature = "standalone")]
use serde_json::{json, Value};
#[cfg(feature = "standalone")]
use std::sync::{
    atomic::{AtomicBool, Ordering},
    Arc,
};
#[cfg(feature = "standalone")]
use std::time::Duration;
#[cfg(feature = "standalone")]
use tauri::{Emitter, Manager};

#[cfg(feature = "standalone")]
struct Companion {
    bridge: Arc<Bridge>,
    stopping: Arc<AtomicBool>,
}

#[cfg(feature = "standalone")]
#[tauri::command]
async fn bridge_request(
    request: Value,
    state: tauri::State<'_, Companion>,
) -> Result<Value, BridgeError> {
    state.bridge.request(request).await
}

#[cfg(feature = "standalone")]
#[tauri::command]
fn bridge_status(state: tauri::State<'_, Companion>) -> Value {
    let pid = state.bridge.connected_pid.load(Ordering::Relaxed);
    json!({"connected": pid != 0, "pid": if pid == 0 { None } else { Some(pid) }})
}

#[cfg(feature = "standalone")]
fn start_session_heartbeat(app: tauri::AppHandle) {
    tauri::async_runtime::spawn(async move {
        let state = app.state::<Companion>();
        while !state.stopping.load(Ordering::Relaxed) {
            let visible = app
                .get_webview_window("main")
                .map(|window| {
                    window.is_visible().unwrap_or(false) && !window.is_minimized().unwrap_or(false)
                })
                .unwrap_or(false);
            // The native three-second lease restores its own menu if the companion exits.
            let _ = state
                .bridge
                .request(json!({"op":"session","active":true,"visible":visible}))
                .await;
            tokio::time::sleep(Duration::from_secs(1)).await;
        }
    });
}

#[cfg(feature = "standalone")]
pub(crate) fn toggle_window(app: &tauri::AppHandle) {
    let Some(window) = app.get_webview_window("main") else {
        return;
    };
    let visible = window.is_visible().unwrap_or(false) && !window.is_minimized().unwrap_or(false);
    let updated = if visible {
        window.minimize().is_ok().then_some(false)
    } else {
        let _ = window.unminimize();
        if window.show().is_ok() {
            let _ = window.set_focus();
            Some(true)
        } else {
            None
        }
    };
    if let Some(visible) = updated {
        let _ = app.emit("velora-visibility", json!({"visible":visible}));
        let bridge = app.state::<Companion>().bridge.clone();
        tauri::async_runtime::spawn(async move {
            let _ = bridge
                .request(json!({"op":"session","active":true,"visible":visible}))
                .await;
        });
    }
}

#[cfg(feature = "standalone")]
pub fn run() {
    let bridge = Arc::new(Bridge::new());
    let stopping = Arc::new(AtomicBool::new(false));
    tauri::Builder::default()
        .plugin(tauri_plugin_single_instance::init(|app, _args, _cwd| {
            if let Some(window) = app.get_webview_window("main") {
                let _ = window.unminimize();
                let _ = window.show();
                let _ = window.set_focus();
            }
        }))
        .manage(Companion {
            bridge: bridge.clone(),
            stopping: stopping.clone(),
        })
        .invoke_handler(tauri::generate_handler![bridge_request, bridge_status])
        .on_window_event(|window, event| {
            if let tauri::WindowEvent::CloseRequested { api, .. } = event {
                api.prevent_close();
                let _ = window.minimize();
            }
        })
        .setup(|app| {
            start_session_heartbeat(app.handle().clone());
            #[cfg(windows)]
            hotkey::start(app.handle().clone());
            Ok(())
        })
        .build(tauri::generate_context!())
        .expect("The DeltaEAC window could not be initialized")
        .run(move |_app, event| {
            if matches!(event, tauri::RunEvent::ExitRequested { .. }) {
                stopping.store(true, Ordering::Relaxed);
                // A graceful close immediately releases input ownership; abnormal exits
                // are covered by the native lease. Never block shutdown indefinitely.
                let _ = tauri::async_runtime::block_on(async {
                    tokio::time::timeout(
                        Duration::from_millis(500),
                        bridge.request(json!({"op":"session","active":false,"visible":false})),
                    )
                    .await
                });
            }
        });
}
