// SPDX-License-Identifier: GPL-3.0-or-later
//! Linux (and Steam Deck) side of the launcher. The game stays a Windows program that runs under Proton or Wine; what the
//! Windows build does with bundled Windows tools, this does with Linux builds of the same tools, shipped in the AppImage's
//! `linux-tools` resource folder:
//!   python/bin/python3 (with lz4 and capstone), zig/zig (0.17.0, cross-compiles pkp.exe), spirv_harness/spirv_harness (the
//!   shader translator built from source with the current fixes, self-contained), scripts/ (pkp_linux.py, fetch_release.py,
//!   steam_shortcut.py), runtime/pkp_rt.dll (this fork's runtime, built by linux/winbuild/build.sh).
//! The release's own files (upscalers, the lifter, pkp_build.py, ...) are installed into the game folder from the official
//! release zip by fetch_release.py, so the launcher never sits next to pkp.exe as it does on Windows; the release's pkp_rt.dll is
//! then replaced by the bundled one (sync_runtime), which also runs on AMD GPUs (the Steam Deck).

use serde_json::Value;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use tauri::{AppHandle, Manager};

/// `linux-tools` in the AppImage's resources; PKP_LINUX_TOOLS overrides it (development runs).
pub fn tools(app: &AppHandle) -> PathBuf {
    if let Some(p) = std::env::var_os("PKP_LINUX_TOOLS") {
        return PathBuf::from(p);
    }
    app.path().resource_dir().map(|d| d.join("linux-tools")).unwrap_or_else(|_| PathBuf::from("linux-tools"))
}

pub fn python(t: &Path) -> PathBuf {
    t.join("python").join("bin").join("python3")
}
pub fn zig(t: &Path) -> PathBuf {
    t.join("zig").join("zig")
}
pub fn harness(t: &Path) -> PathBuf {
    t.join("spirv_harness").join("spirv_harness")
}
pub fn script(t: &Path, name: &str) -> PathBuf {
    t.join("scripts").join(name)
}

/// ~/Games/PokopiaRecomp: where the game folder goes unless the player picks another.
pub fn default_game_dir() -> PathBuf {
    std::env::var_os("HOME").map(PathBuf::from).unwrap_or_else(|| PathBuf::from(".")).join("Games").join("PokopiaRecomp")
}

/// This fork's runtime in linux-tools (the GPU memory fallback for AMD and the Steam Deck, the shader pack, the Deck defaults).
pub fn bundled_runtime(t: &Path) -> PathBuf {
    t.join("runtime").join("pkp_rt.dll")
}

/// Puts the bundled runtime into the game folder when the folder's copy differs: after the release is installed and before every
/// start, so a newer AppImage updates the game too. The copy it replaces the first time is kept as pkp_rt.dll.release.
/// False when nothing changed (or this AppImage has no runtime of its own).
pub fn sync_runtime(t: &Path, game_dir: &Path) -> Result<bool, String> {
    let Ok(new) = std::fs::read(bundled_runtime(t)) else { return Ok(false) };
    let dst = game_dir.join("pkp_rt.dll");
    if std::fs::read(&dst).map(|old| old == new).unwrap_or(false) {
        return Ok(false);
    }
    let backup = game_dir.join("pkp_rt.dll.release");
    if dst.is_file() && !backup.exists() {
        std::fs::copy(&dst, &backup).map_err(|e| format!("could not keep {}: {}", backup.display(), e))?;
    }
    let part = game_dir.join("pkp_rt.dll.part");
    std::fs::write(&part, &new).map_err(|e| format!("could not write {}: {}", part.display(), e))?;
    std::fs::rename(&part, &dst).map_err(|e| format!("could not replace {}: {}", dst.display(), e))?;
    Ok(true)
}

/// The release's files are in the game folder (fetch_release.py ran).
pub fn release_installed(game_dir: &Path) -> bool {
    game_dir.join("pkp_rt.dll").is_file() && game_dir.join("tools").join("build").join("pkp_build.py").is_file()
}

/// Runs one of the bundled Python scripts and parses the single JSON object it prints.
fn run_json(t: &Path, name: &str, args: &[&str]) -> Result<Value, String> {
    let out = Command::new(python(t))
        .arg("-I")
        .arg(script(t, name))
        .args(args)
        .stdin(Stdio::null())
        .output()
        .map_err(|e| format!("could not run {}: {}", name, e))?;
    let text = String::from_utf8_lossy(&out.stdout);
    let v: Value = serde_json::from_str(text.trim().lines().last().unwrap_or("")).map_err(|_| {
        format!("{} failed: {}", name, String::from_utf8_lossy(&out.stderr).lines().last().unwrap_or("no output"))
    })?;
    if v.get("ok") == Some(&Value::Bool(false)) {
        return Err(v.get("error").and_then(|e| e.as_str()).unwrap_or("failed").to_string());
    }
    Ok(v)
}

pub fn steam_status(t: &Path, game_dir: &Path) -> Value {
    let exe = game_dir.join("pkp.exe");
    run_json(t, "steam_shortcut.py", &["status", "--exe", &exe.to_string_lossy()]).unwrap_or(Value::Null)
}

pub fn steam_add(t: &Path, game_dir: &Path) -> Result<Value, String> {
    let exe = game_dir.join("pkp.exe");
    run_json(t, "steam_shortcut.py", &["add", "--exe", &exe.to_string_lossy(), "--close-steam"])
}

/// Starts the game through its Steam shortcut (Proton); without one, through Wine when it is installed.
pub fn play(t: &Path, game_dir: &Path) -> Result<String, String> {
    sync_runtime(t, game_dir)?;
    let exe = game_dir.join("pkp.exe");
    if run_json(t, "steam_shortcut.py", &["launch", "--exe", &exe.to_string_lossy()]).is_ok() {
        return Ok("Started through Steam (Proton).".into());
    }
    if which("wine").is_some() {
        let prefix = std::env::var_os("HOME").map(PathBuf::from).unwrap_or_default().join(".local/share/pokopia-recomp/wine");
        let _ = std::fs::create_dir_all(&prefix);
        Command::new("wine")
            .arg(&exe)
            .args(["--log", "pkp.log"])
            .current_dir(game_dir)
            .env("WINEPREFIX", &prefix)
            .env("WINEDEBUG", "-all")
            .env("WINEDLLOVERRIDES", "mshtml=")
            .stdin(Stdio::null())
            .spawn()
            .map_err(|e| format!("could not start Wine: {}", e))?;
        return Ok(format!("Started with Wine (prefix {}).", prefix.display()));
    }
    Err("Add the game to Steam first (Play page, Add to Steam), or install Wine.".into())
}

/// How many shaders the game recorded as missing while it ran ([nvn] dump_missing).
pub fn missing_shaders(game_dir: &Path) -> usize {
    let m = game_dir.join("shader_cache").join("missing").join("manifest.tsv");
    std::fs::read_to_string(m)
        .map(|t| {
            let mut h: Vec<&str> = t.lines().skip(1).filter_map(|l| l.split('\t').nth(10)).collect();
            h.sort_unstable();
            h.dedup();
            h.len()
        })
        .unwrap_or(0)
}

/// Translates the recorded shaders and adds them to the cache (pkp_linux.py refresh, with the bundled translator).
pub fn refresh(t: &Path, game_dir: &Path) -> Result<String, String> {
    let jobs = std::thread::available_parallelism().map(|n| n.get().saturating_sub(1)).unwrap_or(2).max(1).to_string();
    let out = Command::new(python(t))
        .arg("-I")
        .arg(script(t, "pkp_linux.py"))
        .args(["refresh", "--game"])
        .arg(game_dir)
        .arg("--harness")
        .arg(harness(t))
        .args(["--jobs", &jobs])
        .stdin(Stdio::null())
        .output()
        .map_err(|e| format!("could not run the shader refresh: {}", e))?;
    let text = String::from_utf8_lossy(&out.stdout).to_string();
    let last = text.lines().filter(|l| !l.trim().is_empty()).last().unwrap_or("").trim().to_string();
    if out.status.success() {
        Ok(last)
    } else {
        Err(format!("{} {}", last, String::from_utf8_lossy(&out.stderr).lines().last().unwrap_or("")).trim().to_string())
    }
}

/// fetch_release.py with its @progress lines handed to `report` as (done, total, text).
pub fn install_release(t: &Path, game_dir: &Path, zip: Option<&str>, mut report: impl FnMut(u64, u64, String)) -> Result<(), String> {
    use std::io::{BufRead, BufReader};
    let mut cmd = Command::new(python(t));
    cmd.arg("-I").arg(script(t, "fetch_release.py")).arg("--dir").arg(game_dir);
    if let Some(z) = zip {
        cmd.arg("--zip").arg(z);
    }
    let mut child = cmd.stdout(Stdio::piped()).stderr(Stdio::null()).stdin(Stdio::null()).spawn().map_err(|e| e.to_string())?;
    let mut error = None;
    for l in BufReader::new(child.stdout.take().unwrap()).lines().map_while(Result::ok) {
        if let Some(rest) = l.strip_prefix("@progress ") {
            let mut it = rest.splitn(4, ' ');
            let _phase = it.next();
            let done = it.next().and_then(|x| x.parse().ok()).unwrap_or(0);
            let total = it.next().and_then(|x| x.parse().ok()).unwrap_or(0);
            report(done, total, it.next().unwrap_or("").to_string());
        } else if let Some(rest) = l.strip_prefix("@error ") {
            error = Some(rest.to_string());
        }
    }
    let st = child.wait().map_err(|e| e.to_string())?;
    if st.success() {
        sync_runtime(t, game_dir)?;
        Ok(())
    } else {
        Err(error.unwrap_or_else(|| format!("installing the release failed ({})", st)))
    }
}

pub fn which(exe: &str) -> Option<PathBuf> {
    std::env::var_os("PATH").and_then(|p| std::env::split_paths(&p).map(|d| d.join(exe)).find(|f| f.is_file()))
}

pub fn open(path: &Path) -> Result<(), String> {
    Command::new("xdg-open").arg(path).spawn().map(|_| ()).map_err(|e| format!("Could not open {}: {}", path.display(), e))
}
