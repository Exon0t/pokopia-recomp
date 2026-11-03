// SPDX-License-Identifier: GPL-3.0-or-later
//! First-run game build (release model b): the release ships no game code, so pkp.exe is built here, on the player's PC,
//! from their own dump. The work is done by `tools/build/pkp_build.py` (bundled Python, lifter and Zig; see that file):
//! verify the ExeFS by hash, lift the four modules, compile them, link pkp.exe against pkp_rt.dll (the runtime).
//!
//! Resumable: the script keeps finished modules and objects in `<game>/build`; Pause writes `<game>/build/cancel`, which
//! stops it and its compilers. `<game>/build/done.json` plus pkp.exe means a build finished; it is this release's build
//! only if `pkp_build.py --check` says done.json matches the tools this release ships (lifter, main.sites, pkp_rt.lib, ...).
//! An update extracted over an install leaves the old pkp.exe and done.json behind: the check calls that stale, so Play
//! asks for Set up again and Set up rebuilds (the old pkp.exe stays until the new one is linked over it).
//! No crate-local imports, so `examples/first_run.rs` can build this file alone.

use std::fs;
use std::io::{BufRead, BufReader};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::Duration;

pub struct Tools {
    pub python: PathBuf,
    pub script: PathBuf,
    /// Linux: `_build --script <release pkp_build.py> --zig <Linux Zig>` for pkp_linux.py, which runs the release's script
    /// unchanged with the Linux build of the same Zig. Empty on Windows.
    pub pre_args: Vec<std::ffi::OsString>,
}

pub struct Job {
    pub game_dir: PathBuf,
    pub exefs: PathBuf,
    pub jobs: usize,
}

/// One progress line of the script: phase ("verify", "lift", "compile", "link", "finish"), done, total, text.
pub struct Tick {
    pub phase: String,
    pub done: u64,
    pub total: u64,
    pub message: String,
}

/// The package builds pkp.exe itself (model b) when it carries the build tools.
pub fn is_package_b(tools_dir: &Path) -> bool {
    tools_dir.join("build").join("pkp_build.py").is_file()
}

/// What Play says when pkp.exe is from an older release (Built::Stale). The time is the compile's, as in the README.
pub const REBUILD_MESSAGE: &str = "This update changes the game code: set up again to rebuild pkp.exe (Game files, Set up; about 7 minutes). Your shaders and saves are kept.";

/// How far the pkp.exe in a game folder is from this release.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Built {
    /// no finished build (no pkp.exe or no done.json)
    No,
    /// a finished build from other tools: an older release, or one whose tools could not be checked
    Stale,
    /// built with this release's tools
    Current,
}

/// The last check: (done.json path, its mtime and size, pkp.exe's mtime) -> result. The tools do not change while the
/// launcher runs, and a finished build rewrites done.json, so one python run per finished build is enough.
type CheckKey = (PathBuf, Option<std::time::SystemTime>, u64, Option<std::time::SystemTime>);
static CHECKED: std::sync::Mutex<Option<(CheckKey, Built)>> = std::sync::Mutex::new(None);

/// Whether pkp.exe was built by this release's tools (`pkp_build.py --check`, the stamp's only implementation).
pub fn built_state(game_dir: &Path, tools_dir: &Path) -> Built {
    let exe = game_dir.join("pkp.exe");
    let done = game_dir.join("build").join("done.json");
    let (Ok(em), Ok(dm)) = (fs::metadata(&exe), fs::metadata(&done)) else {
        return Built::No;
    };
    if !em.is_file() || !dm.is_file() {
        return Built::No;
    }
    let key: CheckKey = (done.clone(), dm.modified().ok(), dm.len(), em.modified().ok());
    if let Some((k, b)) = CHECKED.lock().unwrap().as_ref() {
        if *k == key {
            return *b;
        }
    }
    // missing tools: Stale, so Set up runs and names what is missing
    let Ok(tools) = find_tools(tools_dir) else {
        return Built::Stale;
    };
    let mut cmd = Command::new(&tools.python);
    cmd.arg("-I").arg(&tools.script).arg("--check").arg("--game").arg(game_dir);
    quiet(&mut cmd);
    let Ok(out) = cmd.stdin(Stdio::null()).stdout(Stdio::null()).stderr(Stdio::null()).status() else {
        return Built::Stale; // not cached: the next look tries again
    };
    let b = if out.success() { Built::Current } else { Built::Stale };
    *CHECKED.lock().unwrap() = Some((key, b));
    b
}

/// pkp.exe is in place and was built with this release's tools.
pub fn is_built(game_dir: &Path, tools_dir: &Path) -> bool {
    built_state(game_dir, tools_dir) == Built::Current
}

pub fn find_tools(tools_dir: &Path) -> Result<Tools, String> {
    let python = tools_dir.join("python").join("python.exe");
    let script = tools_dir.join("build").join("pkp_build.py");
    let mut missing = Vec::new();
    if !python.is_file() {
        missing.push(python.display().to_string());
    }
    if !script.is_file() {
        missing.push(script.display().to_string());
    }
    for p in ["build/zig/zig.exe", "build/sdk/pkp_rt.lib", "build/recomp/nxrecomp/__main__.py"] {
        if !tools_dir.join(p).is_file() {
            missing.push(tools_dir.join(p).display().to_string());
        }
    }
    if !missing.is_empty() {
        return Err(format!("The game build cannot run, missing: {}. Extract the release zip again so tools/ is complete.", missing.join("; ")));
    }
    Ok(Tools { python, script, pre_args: Vec::new() })
}

/// Linux: the release's build files in `tools_dir` (the game folder's tools/), Python and Zig from the AppImage (`lt`).
#[cfg(unix)]
pub fn find_tools_linux(tools_dir: &Path, lt: &Path) -> Result<Tools, String> {
    let python = crate::linux::python(lt);
    let script = crate::linux::script(lt, "pkp_linux.py");
    let zig = crate::linux::zig(lt);
    let release_script = tools_dir.join("build").join("pkp_build.py");
    let mut missing = Vec::new();
    for p in [&python, &script, &zig, &release_script, &tools_dir.join("build/sdk/pkp_rt.lib"), &tools_dir.join("build/recomp/nxrecomp/__main__.py")] {
        if !p.is_file() {
            missing.push(p.display().to_string());
        }
    }
    if !missing.is_empty() {
        return Err(format!("The game build cannot run, missing: {}. Install the game files again (Game files page).", missing.join("; ")));
    }
    let pre_args = vec!["_build".into(), "--script".into(), release_script.into_os_string(), "--zig".into(), zig.into_os_string()];
    Ok(Tools { python, script, pre_args })
}

fn quiet(cmd: &mut Command) {
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        // CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS (the compilers it starts inherit it)
        cmd.creation_flags(0x0800_0000 | 0x0000_4000);
    }
}

/// Runs the build. Ok(true) = pkp.exe built, Ok(false) = paused, Err = what went wrong, in the script's words.
pub fn run(job: &Job, tools: &Tools, cancel: &AtomicBool, mut report: impl FnMut(Tick)) -> Result<bool, String> {
    let build = job.game_dir.join("build");
    fs::create_dir_all(&build).map_err(|e| format!("cannot create {}: {}", build.display(), e))?;
    let cancel_file = build.join("cancel");
    let _ = fs::remove_file(&cancel_file);
    let mut cmd = Command::new(&tools.python);
    cmd.arg("-I").arg(&tools.script).args(&tools.pre_args).arg("--exefs").arg(&job.exefs).arg("--game").arg(&job.game_dir);
    cmd.args(["--jobs", &job.jobs.max(1).to_string()]);
    quiet(&mut cmd);
    let mut child = cmd.stdout(Stdio::piped()).stderr(Stdio::piped()).spawn().map_err(|e| format!("could not start the game build ({}): {}", tools.python.display(), e))?;
    let out = child.stdout.take().unwrap();
    let err = child.stderr.take().unwrap();
    let (tx, rx) = std::sync::mpsc::channel::<String>();
    std::thread::spawn(move || BufReader::new(out).lines().map_while(Result::ok).for_each(|l| drop(tx.send(l))));
    let err_tail = std::thread::spawn(move || {
        let mut tail: Vec<String> = Vec::new();
        for l in BufReader::new(err).lines().map_while(Result::ok) {
            tail.push(l);
            if tail.len() > 12 {
                tail.remove(0);
            }
        }
        tail.join("\n")
    });
    let mut error: Option<String> = None;
    let mut paused = false;
    let mut asked = false;
    loop {
        if cancel.load(Ordering::SeqCst) && !asked {
            // the script stops its compilers and exits 2; nothing is killed from here
            let _ = fs::write(&cancel_file, "pause\n");
            asked = true;
        }
        match rx.recv_timeout(Duration::from_millis(200)) {
            Ok(l) => {
                if let Some(rest) = l.strip_prefix("@progress ") {
                    let mut it = rest.splitn(4, ' ');
                    let phase = it.next().unwrap_or("").to_string();
                    let done = it.next().and_then(|x| x.parse().ok()).unwrap_or(0);
                    let total = it.next().and_then(|x| x.parse().ok()).unwrap_or(0);
                    let message = it.next().unwrap_or("").to_string();
                    report(Tick { phase, done, total, message });
                } else if let Some(rest) = l.strip_prefix("@error ") {
                    error = Some(rest.replace("\\n", "\n"));
                } else if l.starts_with("@cancelled") {
                    paused = true;
                }
            }
            Err(std::sync::mpsc::RecvTimeoutError::Timeout) => {}
            Err(std::sync::mpsc::RecvTimeoutError::Disconnected) => break,
        }
    }
    let st = child.wait().map_err(|e| e.to_string())?;
    let _ = fs::remove_file(&cancel_file);
    if paused || (asked && st.code() == Some(2)) {
        return Ok(false);
    }
    if st.success() {
        return Ok(true);
    }
    let tail = err_tail.join().unwrap_or_default();
    Err(error.unwrap_or_else(|| format!("The game build failed ({}). {}", st, tail.trim())))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn temp(name: &str) -> PathBuf {
        let d = std::env::temp_dir().join(format!("pkp-gamebuild-{}-{}", name, std::process::id()));
        let _ = fs::remove_dir_all(&d);
        fs::create_dir_all(d.join("build")).unwrap();
        d
    }

    #[test]
    fn no_build_without_exe_or_record() {
        let d = temp("none");
        assert_eq!(built_state(&d, &d.join("tools")), Built::No);
        fs::write(d.join("pkp.exe"), b"MZ").unwrap();
        assert_eq!(built_state(&d, &d.join("tools")), Built::No);
        fs::remove_dir_all(&d).unwrap();
    }

    #[test]
    fn unchecked_build_is_stale() {
        // a finished build that this release's tools cannot vouch for (here: no tools at all) needs Set up again
        let d = temp("stale");
        fs::write(d.join("pkp.exe"), b"MZ").unwrap();
        fs::write(d.join("build").join("done.json"), b"{\"format\": 1}").unwrap();
        assert_eq!(built_state(&d, &d.join("tools")), Built::Stale);
        assert!(!is_built(&d, &d.join("tools")));
        fs::remove_dir_all(&d).unwrap();
    }
}
