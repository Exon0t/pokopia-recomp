// SPDX-License-Identifier: GPL-3.0-or-later
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]
// Linux builds leave the Windows-only paths (Windows tool lookup, starting pkp.exe directly) unused.
#![cfg_attr(unix, allow(dead_code, unused_variables))]

mod dlss;
mod dump;
mod gamebuild;
mod ini;
mod nup;
#[cfg(unix)]
mod linux;
mod play;
mod settings;
mod shaders;

use serde::{Deserialize, Serialize};
use std::fs;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use tauri::Emitter;
use tauri::{AppHandle, Manager, State};
use tauri_plugin_dialog::DialogExt;

/// The launcher's own options, stored in the user's app config folder.
#[derive(Serialize, Deserialize, Clone, Default)]
#[serde(default)]
struct Config {
    /// Root of the decrypted dump (the folder with base/ and updates/).
    dump_dir: String,
    /// Folder with pkp.exe. Empty: the folder the launcher is in.
    game_dir: String,
    minimize_on_play: bool,
}

fn config_path(app: &AppHandle) -> Result<PathBuf, String> {
    let dir = app.path().app_config_dir().map_err(|e| e.to_string())?;
    fs::create_dir_all(&dir).map_err(|e| format!("cannot create {}: {}", dir.display(), e))?;
    Ok(dir.join("launcher.json"))
}

fn load_config(app: &AppHandle) -> Config {
    config_path(app)
        .ok()
        .and_then(|p| fs::read_to_string(p).ok())
        .and_then(|t| serde_json::from_str(&t).ok())
        .unwrap_or_default()
}

fn exe_dir() -> PathBuf {
    std::env::current_exe().ok().and_then(|p| p.parent().map(|d| d.to_path_buf())).unwrap_or_else(|| PathBuf::from("."))
}

fn game_dir_of(cfg: &Config) -> PathBuf {
    if cfg.game_dir.trim().is_empty() {
        // Windows: the launcher sits next to pkp.exe. Linux: the AppImage is read-only and mounted somewhere temporary, so
        // the game gets its own folder.
        #[cfg(unix)]
        return linux::default_game_dir();
        #[cfg(not(unix))]
        return exe_dir();
    } else {
        PathBuf::from(cfg.game_dir.trim())
    }
}

/// The release's tools/ folder (pkp_build.py, the lifter, the SDK, extract_corpus.py): next to the launcher on Windows, in
/// the game folder on Linux (installed there from the release zip).
fn release_tools(cfg: &Config) -> PathBuf {
    #[cfg(unix)]
    return game_dir_of(cfg).join("tools");
    #[cfg(not(unix))]
    return exe_dir().join("tools");
}

#[derive(Serialize)]
struct Overview {
    game_dir: String,
    default_game_dir: String,
    exe_exists: bool,
    /// "linux" or "windows"
    platform: String,
    /// Linux: the release's files are installed in the game folder (always true on Windows)
    release_installed: bool,
    /// Linux: shaders the game recorded as missing, waiting for a refresh
    missing_shaders: usize,
    /// Linux: steam_shortcut.py status (null when unknown)
    steam: serde_json::Value,
}

#[tauri::command]
fn get_config(app: AppHandle) -> Config {
    load_config(&app)
}

#[tauri::command]
fn save_config(app: AppHandle, config: Config) -> Result<(), String> {
    let text = serde_json::to_string_pretty(&config).map_err(|e| e.to_string())?;
    settings::write_atomic(&config_path(&app)?, &text).map_err(|e| format!("cannot save launcher settings: {}", e))
}

#[tauri::command]
fn get_overview(app: AppHandle) -> Overview {
    let cfg = load_config(&app);
    let dir = game_dir_of(&cfg);
    #[cfg(unix)]
    let (platform, release_installed, missing_shaders, steam, default_dir) = {
        let lt = linux::tools(&app);
        let steam = if dir.join("pkp.exe").is_file() { linux::steam_status(&lt, &dir) } else { serde_json::Value::Null };
        ("linux", linux::release_installed(&dir), linux::missing_shaders(&dir), steam, linux::default_game_dir())
    };
    #[cfg(not(unix))]
    let (platform, release_installed, missing_shaders, steam, default_dir) = ("windows", true, 0, serde_json::Value::Null, exe_dir());
    Overview {
        game_dir: dump::fwd(&dir),
        default_game_dir: dump::fwd(&default_dir),
        exe_exists: dir.join("pkp.exe").is_file(),
        platform: platform.into(),
        release_installed,
        missing_shaders,
        steam,
    }
}

#[tauri::command]
async fn pick_folder(app: AppHandle, title: String, start: Option<String>) -> Option<String> {
    let mut dlg = app.dialog().file().set_title(title);
    if let Some(s) = start.filter(|s| PathBuf::from(s).is_dir()) {
        dlg = dlg.set_directory(s);
    }
    dlg.blocking_pick_folder().and_then(|p| p.into_path().ok()).map(|p| dump::fwd(&p))
}

#[tauri::command]
async fn pick_nup(app: AppHandle, start: Option<String>) -> Option<String> {
    let mut dlg = app.dialog().file().set_title("Choose your Pokopia .nup package").add_filter("NUP package", &["nup"]);
    if let Some(s) = start.filter(|s| PathBuf::from(s).is_dir()) {
        dlg = dlg.set_directory(s);
    }
    dlg.blocking_pick_file().and_then(|p| p.into_path().ok()).map(|p| dump::fwd(&p))
}

#[derive(Serialize, Clone)]
struct NupProgress {
    done: u64,
    total: u64,
    file: String,
}

/// Unpacks a .nup into `<game folder>/nup_dump` (base/ and updates/<version>/, the same layout as a dump folder) and
/// returns that folder. Emits "nup-progress". A finished unpack of the same package is reused.
#[tauri::command]
async fn import_nup(app: AppHandle, state: State<'_, Arc<SetupState>>, path: String) -> Result<String, String> {
    if state.progress.lock().unwrap().state == "running" {
        return Err("Setup is running. Pause it first.".into());
    }
    let out = game_dir_of(&load_config(&app)).join("nup_dump");
    let src = PathBuf::from(path.trim());
    let emitter = app.clone();
    let res = tauri::async_runtime::spawn_blocking(move || {
        let mut last = 0u64;
        nup::extract(
            &src,
            &out,
            &mut |done, total, file| {
                // the UI only needs ~100 updates over the whole unpack
                if total == 0 || done == total || done - last >= total / 100 {
                    last = done;
                    let _ = emitter.emit("nup-progress", NupProgress { done, total, file: file.to_string() });
                }
            },
            &|| false,
        )
    })
    .await
    .map_err(|e| e.to_string())??;
    Ok(dump::fwd(&res))
}

#[tauri::command]
fn validate_dump(path: String) -> dump::DumpReport {
    dump::scan(&path)
}

/// Validates, stores the dump folder, and points `[fs] romfs` in pkp.ini at the chosen romfs.
#[tauri::command]
fn save_dump(app: AppHandle, path: String) -> Result<dump::DumpReport, String> {
    let report = dump::scan(&path);
    let chosen = report.chosen.clone().ok_or_else(|| report.errors.join(" "))?;
    let mut cfg = load_config(&app);
    cfg.dump_dir = report.path.clone();
    save_config(app.clone(), cfg.clone())?;
    let gd = game_dir_of(&cfg);
    settings::apply(
        &gd,
        &[
            settings::Edit { file: "pkp".into(), section: "fs".into(), key: "exefs".into(), value: settings::game_path_in(&gd, &chosen.exefs) },
            settings::Edit { file: "pkp".into(), section: "fs".into(), key: "romfs".into(), value: settings::game_path_in(&gd, &chosen.romfs) },
        ],
    )?;
    Ok(report)
}

#[tauri::command]
fn read_settings(app: AppHandle) -> settings::SettingsDump {
    settings::read(&game_dir_of(&load_config(&app)))
}

#[tauri::command]
fn write_settings(app: AppHandle, edits: Vec<settings::Edit>) -> Result<(), String> {
    settings::apply(&game_dir_of(&load_config(&app)), &edits)
}

#[tauri::command]
fn play(app: AppHandle) -> Result<(), String> {
    let cfg = load_config(&app);
    if cfg.dump_dir.trim().is_empty() {
        return Err("Choose your game files first (Setup).".into());
    }
    // a .nup package is played as it is: pkp reads exefs and romfs straight from it
    let (exefs, romfs) = if cfg.dump_dir.trim().ends_with(".nup") && Path::new(cfg.dump_dir.trim()).is_file() {
        (cfg.dump_dir.trim().to_string(), cfg.dump_dir.trim().to_string())
    } else {
        let report = dump::scan(&cfg.dump_dir);
        let chosen = report.chosen.ok_or_else(|| format!("The game files at {} are not usable: {}", cfg.dump_dir, report.errors.join(" ")))?;
        (chosen.exefs, chosen.romfs)
    };
    let dir = game_dir_of(&cfg);
    let tools_dir = release_tools(&cfg);
    if gamebuild::is_package_b(&tools_dir) {
        match gamebuild::built_state(&dir, &tools_dir) {
            gamebuild::Built::Current => {}
            gamebuild::Built::Stale => return Err(gamebuild::REBUILD_MESSAGE.into()),
            gamebuild::Built::No => return Err("Build the game first (Game files, Set up).".into()),
        }
    }
    if !shaders::is_complete(&dir) {
        return Err("Finish the shader setup first (Game files, Set up).".into());
    }
    settings::ensure_play_paths(&dir, &exefs, &romfs)?;
    #[cfg(unix)]
    return play_linux(&app, &dir);
    #[cfg(not(unix))]
    return play::start(&app, &dir, cfg.minimize_on_play);
}

/// Linux Play: translate any shaders the game recorded as missing, then start it through Steam (Proton) or Wine.
#[cfg(unix)]
fn play_linux(app: &AppHandle, dir: &std::path::Path) -> Result<(), String> {
    let lt = linux::tools(app);
    if linux::missing_shaders(dir) > 0 {
        linux::refresh(&lt, dir)?;
    }
    let message = linux::play(&lt, dir)?;
    play::set_status(app, play::Status { state: "idle".into(), message, ..Default::default() });
    Ok(())
}

/// Shader setup: the running job's progress and its cancel flag.
#[derive(Default)]
struct SetupState {
    progress: Mutex<shaders::Progress>,
    cancel: AtomicBool,
}

#[derive(Serialize)]
struct SetupStatus {
    progress: shaders::Progress,
    /// setup.json says finished and the cache is there
    complete: bool,
    /// an earlier setup started (Set up continues it)
    started: bool,
    /// pkp.exe is from an older release whose game code this one changes: Set up rebuilds it
    rebuild: bool,
}

#[tauri::command]
fn setup_status(app: AppHandle, state: State<Arc<SetupState>>) -> SetupStatus {
    let dir = game_dir_of(&load_config(&app));
    let mut progress = state.progress.lock().unwrap().clone();
    if progress.state.is_empty() {
        progress.state = "idle".into();
    }
    let tools_dir = release_tools(&load_config(&app));
    let state = if gamebuild::is_package_b(&tools_dir) { gamebuild::built_state(&dir, &tools_dir) } else { gamebuild::Built::Current };
    let started = !shaders::read_marker(&dir).romfs.is_empty() || dir.join("build").join("state.json").is_file();
    SetupStatus {
        progress,
        complete: state == gamebuild::Built::Current && shaders::is_complete(&dir),
        started,
        rebuild: state == gamebuild::Built::Stale,
    }
}

/// First-run setup: (a) the pkp.ini paths, (b) in a model (b) release, pkp.exe built from the player's dump
/// (gamebuild.rs), (c) the shader cache in shader_cache/spirv, on a worker thread with "setup-status" events.
#[tauri::command]
fn start_setup(app: AppHandle, state: State<Arc<SetupState>>) -> Result<(), String> {
    let cfg = load_config(&app);
    let report = dump::scan(&cfg.dump_dir);
    let chosen = report.chosen.ok_or_else(|| "Choose usable game files first.".to_string())?;
    let dir = game_dir_of(&cfg);
    let tools_dir = release_tools(&cfg);
    #[cfg(unix)]
    if !linux::release_installed(&dir) {
        return Err(format!("The Pokopia Recomp files are not in {} yet: press Install on the Game files page first.", dump::fwd(&dir)));
    }
    #[cfg(unix)]
    let lt = linux::tools(&app);
    let package_b = gamebuild::is_package_b(&tools_dir);
    if !package_b && !dir.join("pkp.exe").is_file() {
        return Err(format!("pkp.exe was not found in {}. Put the launcher next to pkp.exe or set the game folder in Settings.", dump::fwd(&dir)));
    }
    {
        let mut p = state.progress.lock().unwrap();
        if p.state == "running" {
            return Err("Setup is already running.".into());
        }
        *p = shaders::Progress { state: "running".into(), phase: "check".into(), message: "Checking the setup tools...".into(), ..Default::default() };
    }
    settings::ensure_play_paths(&dir, &chosen.exefs, &chosen.romfs)?;
    let st = state.inner().clone();
    st.cancel.store(false, Ordering::SeqCst);
    let jobs = std::thread::available_parallelism().map(|n| n.get().saturating_sub(1)).unwrap_or(2).max(1);
    let build_job = gamebuild::Job { game_dir: dir.clone(), exefs: PathBuf::from(&chosen.exefs), jobs: jobs + 1 };
    let job = shaders::Job { game_dir: dir, romfs: PathBuf::from(&chosen.romfs), main: PathBuf::from(&chosen.exefs).join("main"), jobs, limit: 0 };
    std::thread::spawn(move || {
        let emit = |p: shaders::Progress| {
            *st.progress.lock().unwrap() = p.clone();
            let _ = app.emit("setup-status", p);
        };
        #[cfg_attr(not(unix), allow(unused_mut))]
        let mut note: Option<String> = None;   // Linux: why the hidden first run did not finish
        let result = (|| {
            // no build yet, or one from an older release (stale tools stamp): build pkp.exe (again)
            if package_b && !gamebuild::is_built(&build_job.game_dir, &tools_dir) {
                #[cfg(unix)]
                let tools = gamebuild::find_tools_linux(&tools_dir, &lt)?;
                #[cfg(not(unix))]
                let tools = gamebuild::find_tools(&tools_dir)?;
                let built = gamebuild::run(&build_job, &tools, &st.cancel, |t| {
                    emit(shaders::Progress { state: "running".into(), phase: t.phase, done: t.done, total: t.total, message: t.message })
                })?;
                if !built {
                    return Ok(false);
                }
            }
            #[cfg(unix)]
            let tools = shaders::find_tools_linux(&tools_dir, &lt)?;
            #[cfg(not(unix))]
            let tools = shaders::find_tools(&tools_dir)?;
            // a finished cache of the same romfs has nothing left to translate (re-extracting would only find that out)
            let same = shaders::read_marker(&job.game_dir).romfs == job.romfs.to_string_lossy().replace('\\', "/");
            let done = same && shaders::is_complete(&job.game_dir);
            if !done && !shaders::run(&job, &tools, &st.cancel, |p| emit(p))? {
                return Ok(false);
            }
            // Linux: one hidden run records the shaders the game builds while it runs and translates them (linux::warm_up), so the
            // first start has them, also from Steam. Not fatal: without it the first start records them and Play translates them.
            #[cfg(unix)]
            {
                emit(shaders::Progress {
                    state: "running".into(),
                    phase: "warmup".into(),
                    message: "Running the game once in the background to prepare the shaders it builds while it runs (about a minute)...".into(),
                    ..Default::default()
                });
                if let Err(e) = linux::warm_up(&lt, &job.game_dir, &st.cancel) {
                    if st.cancel.load(Ordering::SeqCst) {
                        return Ok(false);
                    }
                    note = Some(format!(
                        " The hidden first run did not finish ({}): the first start records the shaders it builds while it runs, and Play translates them.",
                        if e.is_empty() { "no details" } else { e.as_str() }
                    ));
                }
                return Ok(true);
            }
            // the shaders the game builds while it runs: one hidden run collects those of the opening; later ones are
            // collected by every normal run, and every Play checks them all (play.rs)
            #[cfg(not(unix))]
            {
                emit(shaders::Progress {
                    state: "running".into(),
                    phase: "warmup".into(),
                    message: format!("Running the game once in the background to collect the shaders it builds while it runs (about {} s)...", shaders::WARMUP_SECONDS),
                    ..Default::default()
                });
                return Ok(shaders::prepare(&job.game_dir, &tools_dir, &st.cancel)?.is_some());
            }
        })();
        let end = match result {
            Ok(true) => shaders::Progress {
                state: "done".into(),
                phase: "finish".into(),
                message: format!("Setup finished.{}", note.unwrap_or_default()),
                ..Default::default()
            },
            Ok(false) => shaders::Progress { state: "cancelled".into(), message: "Setup paused. Set up again continues where it stopped.".into(), ..Default::default() },
            Err(e) => shaders::Progress { state: "error".into(), message: e, ..Default::default() },
        };
        emit(end);
    });
    Ok(())
}

#[tauri::command]
fn cancel_setup(state: State<Arc<SetupState>>) {
    state.cancel.store(true, Ordering::SeqCst);
}

#[tauri::command]
fn dlss_status(app: AppHandle) -> dlss::Status {
    dlss::status(&game_dir_of(&load_config(&app)), &exe_dir().join("tools"))
}

/// Downloads NVIDIA's nvngx_dlss.dll (see dlss.rs) on a worker thread, with "dlss-progress" events.
#[tauri::command]
async fn fetch_dlss(app: AppHandle) -> Result<(), String> {
    let dir = game_dir_of(&load_config(&app));
    let tools_dir = exe_dir().join("tools");
    tauri::async_runtime::spawn_blocking(move || {
        dlss::fetch(&dir, &tools_dir, |p| {
            let _ = app.emit("dlss-progress", p);
        })
    })
    .await
    .map_err(|e| e.to_string())?
}

#[tauri::command]
fn game_status(state: State<play::PlayState>) -> play::Status {
    state.0.lock().unwrap().clone()
}

#[tauri::command]
fn open_game_file(app: AppHandle, name: String) -> Result<(), String> {
    if !play::REPORT_FILES.contains(&name.as_str()) {
        return Err("That file cannot be opened from here.".into());
    }
    let path = game_dir_of(&load_config(&app)).join(&name);
    if !path.is_file() {
        return Err(format!("{} does not exist.", name));
    }
    play::open_with_shell(&path)
}

#[tauri::command]
fn open_game_dir(app: AppHandle) -> Result<(), String> {
    play::open_with_shell(&game_dir_of(&load_config(&app)))
}


/// Linux: installs the official release's files into the game folder (fetch_release.py), with "release-progress" events.
#[tauri::command]
async fn install_release(app: AppHandle, zip: Option<String>) -> Result<(), String> {
    #[cfg(unix)]
    {
        let dir = game_dir_of(&load_config(&app));
        let lt = linux::tools(&app);
        return tauri::async_runtime::spawn_blocking(move || {
            linux::install_release(&lt, &dir, zip.as_deref(), |done, total, message| {
                let _ = app.emit("release-progress", shaders::Progress { state: "running".into(), phase: "install".into(), done, total, message });
            })
        })
        .await
        .map_err(|e| e.to_string())?;
    }
    #[cfg(not(unix))]
    return { let _ = (app, zip); Err("Only needed on Linux.".into()) };
}

/// Linux: adds pkp.exe to Steam as a non-Steam game with Proton (closes Steam first if it is running).
#[tauri::command]
async fn steam_add(app: AppHandle) -> Result<serde_json::Value, String> {
    #[cfg(unix)]
    {
        let dir = game_dir_of(&load_config(&app));
        let lt = linux::tools(&app);
        return tauri::async_runtime::spawn_blocking(move || linux::steam_add(&lt, &dir)).await.map_err(|e| e.to_string())?;
    }
    #[cfg(not(unix))]
    return { let _ = app; Err("Only needed on Linux.".into()) };
}

/// Linux: translates the shaders the game recorded as missing and adds them to the cache.
#[tauri::command]
async fn refresh_shaders(app: AppHandle) -> Result<String, String> {
    #[cfg(unix)]
    {
        let dir = game_dir_of(&load_config(&app));
        let lt = linux::tools(&app);
        return tauri::async_runtime::spawn_blocking(move || linux::refresh(&lt, &dir)).await.map_err(|e| e.to_string())?;
    }
    #[cfg(not(unix))]
    return { let _ = app; Err("Only needed on Linux.".into()) };
}

/// Test runs only (PKP_LAUNCHER_EVAL_FILE set): the page writes a result next to the eval file, <file>.out.
#[tauri::command]
fn test_report(text: String) -> Result<(), String> {
    let path = std::env::var("PKP_LAUNCHER_EVAL_FILE").map_err(|_| "not a test run".to_string())?;
    std::fs::write(format!("{path}.out"), text).map_err(|e| e.to_string())
}

fn main() {
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .manage(play::PlayState::default())
        .manage(Arc::new(SetupState::default()))
        .setup(|app| {
            // Test runs drive the page without input: PKP_LAUNCHER_EVAL_FILE=<path>. A JS file written there is run in the page once and
            // deleted; it reports back through document.title (the window title). WebView2's DevTools port does not open for Tauri apps here.
            if let Ok(path) = std::env::var("PKP_LAUNCHER_EVAL_FILE") {
                if let Some(win) = app.get_webview_window("main") {
                    std::thread::spawn(move || loop {
                        std::thread::sleep(std::time::Duration::from_millis(300));
                        if let Ok(js) = std::fs::read_to_string(&path) {
                            let _ = std::fs::remove_file(&path);
                            let _ = win.eval(&js);
                        }
                    });
                }
            }
            Ok(())
        })
        .invoke_handler(tauri::generate_handler![
            test_report,
            get_config,
            save_config,
            get_overview,
            pick_folder,
            validate_dump,
            pick_nup,
            import_nup,
            save_dump,
            read_settings,
            write_settings,
            play,
            setup_status,
            start_setup,
            cancel_setup,
            dlss_status,
            fetch_dlss,
            game_status,
            open_game_file,
            open_game_dir,
            install_release,
            steam_add,
            refresh_shaders,
        ])
        .run(tauri::generate_context!())
        .expect("error while running the launcher");
}
