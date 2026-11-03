// SPDX-License-Identifier: GPL-3.0-or-later
//! Reads and edits the runtime's ini files next to pkp.exe. The key schema lives in the frontend;
//! this side only moves `section.key` values in and out, with the careful editor in ini.rs.

use crate::ini::Ini;
use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use std::fs;
use std::path::{Path, PathBuf};

pub const PKP_INI: &str = "pkp.ini";
pub const CONTROLS_INI: &str = "controls.ini";

#[derive(Serialize)]
pub struct SettingsDump {
    pub game_dir: String,
    pub pkp_exists: bool,
    pub controls_exists: bool,
    pub pkp: HashMap<String, String>,
    pub controls: HashMap<String, String>,
}

#[derive(Deserialize)]
pub struct Edit {
    /// "pkp" or "controls"
    pub file: String,
    pub section: String,
    pub key: String,
    pub value: String,
}

fn load(path: &Path) -> Option<Ini> {
    let bytes = fs::read(path).ok()?;
    // the files are UTF-8 (controls.ini has a non-ASCII header); a stray bad byte must not lose the file
    Some(Ini::parse(&String::from_utf8_lossy(&bytes)))
}

fn map_of(ini: &Option<Ini>) -> HashMap<String, String> {
    ini.as_ref().map(|i| i.all().into_iter().collect()).unwrap_or_default()
}

pub fn file_for(dir: &Path, which: &str) -> Result<PathBuf, String> {
    match which {
        "pkp" => Ok(dir.join(PKP_INI)),
        "controls" => Ok(dir.join(CONTROLS_INI)),
        other => Err(format!("unknown settings file '{}'", other)),
    }
}

pub fn read(dir: &Path) -> SettingsDump {
    let pkp = load(&dir.join(PKP_INI));
    let controls = load(&dir.join(CONTROLS_INI));
    SettingsDump {
        game_dir: crate::dump::fwd(dir),
        pkp_exists: pkp.is_some(),
        controls_exists: controls.is_some(),
        pkp: map_of(&pkp),
        controls: map_of(&controls),
    }
}

pub fn write_atomic(path: &Path, text: &str) -> Result<(), String> {
    let tmp = PathBuf::from(format!("{}.tmp", path.display()));
    fs::write(&tmp, text).map_err(|e| format!("cannot write {}: {}", tmp.display(), e))?;
    fs::rename(&tmp, path).map_err(|e| {
        let _ = fs::remove_file(&tmp);
        format!("cannot replace {}: {}", path.display(), e)
    })
}

/// Applies edits grouped per file. controls.ini is never created: the game writes it with its factory
/// bindings on first run, and a file with only a few keys would lose them.
pub fn apply(dir: &Path, edits: &[Edit]) -> Result<(), String> {
    for which in ["pkp", "controls"] {
        let mine: Vec<&Edit> = edits.iter().filter(|e| e.file == which).collect();
        if mine.is_empty() {
            continue;
        }
        let path = file_for(dir, which)?;
        let mut ini = match load(&path) {
            Some(i) => i,
            None if which == "pkp" => Ini::empty(),
            None => return Err(format!("{} does not exist yet. Start the game once so it creates it.", CONTROLS_INI)),
        };
        for e in mine {
            ini.set(&e.section, &e.key, &e.value);
        }
        write_atomic(&path, &ini.to_text())?;
    }
    Ok(())
}

/// A path for pkp.ini: relative to the game folder (forward slashes) when it lies inside it, so the folder can be moved or copied to
/// another PC or to SteamOS/Proton with its dump and saves; absolute otherwise. The game resolves relative paths against its folder.
pub fn portable(dir: &Path, p: &str) -> String {
    let full = Path::new(p.trim());
    match (fs::canonicalize(dir), fs::canonicalize(full)) {
        (Ok(d), Ok(f)) => match f.strip_prefix(&d) {
            Ok(rel) if !rel.as_os_str().is_empty() => crate::dump::fwd(rel),
            _ => p.to_string(),
        },
        _ => p.to_string(),
    }
}

/// A path as pkp.exe sees it. On Linux the game runs under Wine/Proton, where an absolute Linux path is only reachable
/// through the Z: drive (/ of the Linux file system); a bare "/home/..." would be read as C:\\home\\....
pub fn game_path(p: &str) -> String {
    #[cfg(unix)]
    if p.starts_with('/') {
        return format!("Z:{}", p);
    }
    p.to_string()
}

/// game_path of `portable`: relative when `p` is inside the game folder (the game's working directory), so such a folder can be
/// copied to a Steam Deck or another drive and still work.
pub fn game_path_in(dir: &Path, p: &str) -> String {
    game_path(&portable(dir, p))
}

/// The inverse of game_path, for checking a path the ini holds.
fn host_path(p: &str) -> &str {
    #[cfg(unix)]
    if let Some(rest) = p.strip_prefix("Z:").or_else(|| p.strip_prefix("z:")) {
        return rest;
    }
    p
}

/// Used by Play and setup: the ini keys the launcher owns. pkp.exe reads every path from pkp.ini (a pkp.ini next to it makes
/// the game folder its working directory), so it needs no command-line paths.
pub fn ensure_play_paths(dir: &Path, exefs: &str, romfs: &str) -> Result<(), String> {
    let path = dir.join(PKP_INI);
    let mut ini = load(&path).unwrap_or_else(Ini::empty);
    let mut changed = !path.exists();
    let (exefs, romfs) = (game_path_in(dir, exefs), game_path_in(dir, romfs));

    // [fs] exefs and [fs] romfs: kept while they name an existing folder or .nup (the user may have pointed them elsewhere)
    for (key, value) in [("exefs", exefs.as_str()), ("romfs", romfs.as_str())] {
        let cur = ini.get("fs", key).unwrap_or_default();
        let cur_path = Path::new(host_path(cur.trim()));
        // relative paths in the ini are relative to the game folder, the working directory of the game
        let cur_full = if cur_path.is_absolute() { cur_path.to_path_buf() } else { dir.join(cur_path) };
        if cur.trim().is_empty() || !(cur_full.is_dir() || cur_full.is_file()) {
            ini.set("fs", key, &portable(dir, value));
            changed = true;
        }
    }
    if ini.get("nvn", "shader_cache").unwrap_or_default().trim() != crate::shaders::SPIRV_REL {
        ini.set("nvn", "shader_cache", crate::shaders::SPIRV_REL);
        changed = true;
    }
    // shaders the game builds while it runs, for play.rs to translate after it exits (kept if the user set it, even empty)
    if ini.get("nvn", "dump_missing").is_none() {
        ini.set("nvn", "dump_missing", crate::shaders::MISSING_REL);
        changed = true;
    }
    let cur_save = ini.get("fs", "save_dir").unwrap_or_default();
    if cur_save.trim().is_empty() {
        let _ = fs::create_dir_all(dir.join("save"));
        ini.set("fs", "save_dir", "save");   // relative: the saves move with the folder
        changed = true;
    }
    #[cfg(unix)]
    {
        // Under Wine/Proton every write fault of the pool mirror's page tracking is a signal turned into a Windows
        // exception (~0.4 ms); some scenes take ~200 a frame. Off: 17-98 fps -> a steady 160+ on an RTX 4070. Kept if set.
        if ini.get("nvn", "pool_mirror").is_none() {
            ini.set("nvn", "pool_mirror", "off");
            changed = true;
        }
    }
    if changed {
        write_atomic(&path, &ini.to_text())?;
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn portable_paths_inside_the_game_folder_are_relative() {
        let dir = std::env::temp_dir().join(format!("pkp_portable_{}", std::process::id()));
        let inside = dir.join("dump").join("updates").join("655360").join("exefs");
        fs::create_dir_all(&inside).unwrap();
        assert_eq!(portable(&dir, &inside.to_string_lossy()), "dump/updates/655360/exefs");
        let outside = std::env::temp_dir();
        assert_eq!(portable(&dir, &outside.to_string_lossy()), outside.to_string_lossy());   // outside: unchanged
        assert_eq!(portable(&dir, "Z:/does/not/exist"), "Z:/does/not/exist");
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn play_paths_turn_on_dump_missing_once() {
        let dir = std::env::temp_dir().join(format!("pkp_dm_{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        let d = crate::dump::fwd(&dir);
        ensure_play_paths(&dir, &d, &d).unwrap();
        assert_eq!(load(&dir.join(PKP_INI)).unwrap().get("nvn", "dump_missing").as_deref(), Some(crate::shaders::MISSING_REL));
        // the user turned it off: kept
        let mut ini = load(&dir.join(PKP_INI)).unwrap();
        ini.set("nvn", "dump_missing", "");
        write_atomic(&dir.join(PKP_INI), &ini.to_text()).unwrap();
        ensure_play_paths(&dir, &d, &d).unwrap();
        assert_eq!(load(&dir.join(PKP_INI)).unwrap().get("nvn", "dump_missing").as_deref(), Some(""));
        let _ = fs::remove_dir_all(&dir);
    }
}
