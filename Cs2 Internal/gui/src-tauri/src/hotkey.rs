use crate::{toggle_window, Companion};
use std::sync::atomic::Ordering;
use std::time::Duration;
use tauri::Manager;
use windows_sys::Win32::UI::Input::KeyboardAndMouse::{
    RegisterHotKey, UnregisterHotKey, MOD_NOREPEAT, VK_INSERT,
};
use windows_sys::Win32::UI::WindowsAndMessaging::{
    GetForegroundWindow, GetWindowThreadProcessId, PeekMessageW, MSG, PM_REMOVE, WM_HOTKEY,
};

const HOTKEY_ID: i32 = 0x564C;

pub fn start(app: tauri::AppHandle) {
    std::thread::Builder::new()
        .name("velora-insert".into())
        .spawn(move || {
            let state = app.state::<Companion>();
            let our_pid = std::process::id();
            let mut registered = false;
            // Register only while our own window or the connected game has foreground
            // focus. Unrelated applications retain their normal Insert shortcut.
            while !state.stopping.load(Ordering::Relaxed) {
                unsafe {
                    let mut foreground_pid = 0;
                    GetWindowThreadProcessId(GetForegroundWindow(), &mut foreground_pid);
                    let game_pid = state.bridge.connected_pid.load(Ordering::Relaxed);
                    let eligible =
                        foreground_pid == our_pid || (game_pid != 0 && foreground_pid == game_pid);
                    if eligible && !registered {
                        registered = RegisterHotKey(
                            std::ptr::null_mut(),
                            HOTKEY_ID,
                            MOD_NOREPEAT,
                            VK_INSERT as u32,
                        ) != 0;
                    } else if !eligible && registered {
                        UnregisterHotKey(std::ptr::null_mut(), HOTKEY_ID);
                        registered = false;
                    }
                    let mut message: MSG = std::mem::zeroed();
                    while PeekMessageW(
                        &mut message,
                        std::ptr::null_mut(),
                        WM_HOTKEY,
                        WM_HOTKEY,
                        PM_REMOVE,
                    ) != 0
                    {
                        if message.wParam == HOTKEY_ID as usize && eligible {
                            toggle_window(&app);
                        }
                    }
                }
                std::thread::sleep(Duration::from_millis(40));
            }
            if registered {
                unsafe {
                    UnregisterHotKey(std::ptr::null_mut(), HOTKEY_ID);
                }
            }
        })
        .expect("The menu shortcut could not be initialized");
}
