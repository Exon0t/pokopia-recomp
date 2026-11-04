# Linux, Steam Deck and AMD GPUs

The game stays a Windows program (`pkp.exe` + `pkp_rt.dll`) and runs under Proton or Wine. It draws with Vulkan, which Wine
hands straight to your GPU driver. `pkp.exe` is recompiled on your computer from your own dump; `pkp_rt.dll` (the runtime) is
built from this repository. Nothing from the game is ever downloaded or distributed.

Tested: Fedora 44 with an RTX 4070 (Proton Experimental 11 and Wine 11), and a Steam Deck LCD in Game Mode (Proton
Experimental).

## The easy way: the launcher AppImage

1. Download `PokopiaLauncher-<version>-x86_64.AppImage` from this fork's Releases, make it executable
   (`chmod +x`, or Properties, Executable) and run it.
2. **Game files**: press **Install**. It downloads the upstream Windows release (pinned by size and SHA-256), keeps what the
   game needs from it (upscalers, the recompiler, fonts, ...) and puts this fork's runtime in place of the release's. The game
   folder is `~/Games/PokopiaRecomp` unless you pick another.
3. Choose your dump and press **Set up**: it checks the dump, recompiles the game code into `pkp.exe` (~10 min) and translates
   the game's shaders (1-2 hours on a desktop; it can pause and continue).
4. **Play**: **Add to Steam** adds `pkp.exe` as a non-Steam game with Proton Experimental (restart Steam afterwards), then
   **Play** starts it through Steam. Without Steam it uses Wine if installed.

The AppImage carries its own Python, Zig (to link `pkp.exe`) and shader translator, so nothing else needs installing, on
SteamOS included.

The dump is the usual layout: `base/` and `updates/<version>/`, each with `exefs/` and `romfs/`. Version 2.0.1a (update
655360) is required.

## Steam Deck

Shader translation takes hours of CPU, so do the setup on a desktop and copy the finished game folder to the Deck. The shader
cache is SPIR-V, which runs on any GPU, so a cache built on an NVIDIA desktop is fine on the Deck.

- **Put it on the internal SSD.** A microSD card works, but the first start reads ~2 GB of shaders and game data; from a card
  that is around 30 s before the title screen, from the SSD a few seconds.
- Copy with `rsync -aL` (or a USB drive): `-L` copies the dump that `exefs/` and `romfs/` may link to.
  `rsync -aL --info=progress2 --exclude build/ --exclude tools/ ~/Games/PokopiaRecomp/ deck@steamdeck:/home/deck/Games/PokopiaRecomp/`
  (`tools/` and `build/` are only needed for setup).
- Add it to Steam in Desktop Mode: the AppImage's **Add to Steam**, or by hand (*Add a Non-Steam Game*, `pkp.exe`, Start in =
  the folder, Compatibility: Proton Experimental). Then play from Game Mode.
- The runtime recognises the Deck (Steam sets `SteamDeck=1`): it renders at 720p and shows the controller's button prompts.
- When the game asks for a name, Steam's on-screen keyboard comes up by itself; its Enter confirms. Closing it and pressing any
  button shows the game's own on-screen keys instead. `[applet] steam_keyboard = off` in `pkp.ini` keeps Steam's keyboard away.
  `[display] render_scale` in `controls.ini` overrides the resolution (`100` = 1080p), `[host] handheld = off` in `pkp.ini`
  turns the Deck defaults off.
- On a Deck LCD the game plays at about 30 fps (720p, the default).
- The first start after a new shader cache takes longest: the game writes `shader_cache/spirv.preload.pack` in the background
  once it has loaded, and later starts read that one file instead of thousands.

## Doing it by hand (desktop)

```sh
sudo dnf install wine                                   # or your distribution's wine
python3 -m venv tools/.venv && tools/.venv/bin/pip install lz4 capstone==5.0.9

# the upstream release, verified against the sha256 GitHub lists for it
mkdir -p ~/Games && cd ~/Games
curl -LO https://github.com/drakolordx7/pokopia-recomp/releases/download/v0.1.1/PokopiaRecomp-0.1.1-win64.zip
echo "170493047c51608e570dd081b57e4a1b80b665e54a9b6fdaa071df95a4674258  PokopiaRecomp-0.1.1-win64.zip" | sha256sum -c
python3 -m zipfile -e PokopiaRecomp-0.1.1-win64.zip . && mv PokopiaRecomp-0.1.1 PokopiaRecomp

# Zig for Linux, the version the release bundles for Windows
curl -LO https://ziglang.org/download/0.17.0/zig-x86_64-linux-0.17.0.tar.xz
echo "1cbe9df9f27e6b78d14ccbca43b6703a404ef79ef1c463de901d7f088d4e2026  zig-x86_64-linux-0.17.0.tar.xz" | sha256sum -c
mkdir -p PokopiaRecomp/tools/build/zig-linux
tar -xJf zig-x86_64-linux-0.17.0.tar.xz -C PokopiaRecomp/tools/build/zig-linux --strip-components=1
```

Then replace the release's `pkp_rt.dll` with this fork's (from Releases, or built with `linux/winbuild/build.sh`, below), and
from the repository root:

```sh
tools/.venv/bin/python -I linux/pkp_linux.py check --dump /path/to/dump    # optional: setup checks too
tools/.venv/bin/python -I linux/pkp_linux.py setup --dump /path/to/dump    # recompile + shaders; Ctrl+C pauses
tools/.venv/bin/python -I linux/pkp_linux.py play                          # add --offline for no network at all
```

Pass `--harness` to `setup` with a current shader translator: the release's (`tools/spirv_harness/spirv_harness.exe`, run under
Wine otherwise) predates its own sine/cosine fix and lacks this fork's half-float fix, and the game's colours look washed out
with it. `linux/appimage/build.sh` builds one at `linux/appimage/build/linux-tools/spirv_harness/spirv_harness`.

Setup links `exefs/` and `romfs/` into the game folder and writes `pkp.ini` with relative paths, so the folder is
self-contained and portable. Saves go to `save/`, the log to `pkp.log`. In game, F1 opens the settings.

## Settings worth knowing

- **Shaders the game builds while it runs.** Setup translates every shader in the game files, but the game also assembles
  some while it runs (about 200 in the first hours). Until those are translated, whatever uses them is not drawn (a black
  character, for example). Setup turns on `[nvn] dump_missing`, which saves each one to `shader_cache/missing`; translate them
  with the launcher's **Translate now** or `pkp_linux.py refresh` (seconds). Do it again after reaching new areas.
- **`[nvn] pool_mirror = off`** (setup writes it). Upstream's "Faster GPU memory" keeps copies of the game's memory in video
  memory and tracks the game's writes with page protection; under Wine that cost 0.4 ms per write fault and 17-98 fps on an
  RTX 4070 against 162-165 fps without it. Under Wine the heap is now private memory (`[memory] heap_section = auto`), which has
  no alias views, so the mirror stays off there either way.
- **AMD GPUs** need nothing: when the driver refuses to import the game's memory the runtime logs `GPU memory import
  unavailable` and keeps copies itself. `[nvn] pool_import = off` forces that path on NVIDIA, for testing.
- Everything else: [runtime/docs/CONFIG.md](../runtime/docs/CONFIG.md).

## Building

- **The runtime** (`pkp_rt.dll`, a Windows DLL) on Linux: `linux/winbuild/build.sh` in a podman container with clang-cl and
  lld-link. Once, `linux/winbuild/build.sh sdk` downloads the MSVC C runtime and the Windows SDK from Microsoft with
  [xwin](https://github.com/Jake-Shadle/xwin); running it means **you** accept Microsoft's licence terms for them. Output:
  `game-build/build/linux-rt/pkp_rt.dll` (plus a linker map for symbolising crash stacks).
- **The AppImage**: `linux/appimage/build.sh` (podman and the .NET 10 SDK; build the runtime first). It pins and checks every
  download: python-build-standalone, Zig, appimagetool, the Ryubing sources of the shader translator.

## What you are trusting

Everything the AppImage runs is built from this repository and the pinned upstream sources listed in the build scripts. From
the upstream Windows release it installs only the files it does not build itself (the recompiler scripts, the texture and
upscaler DLLs, fonts); the AMD and NVIDIA upscaler DLLs carry AMD and NVIDIA signatures. Wine is not a sandbox: a Windows
program under Wine can read your home folder. `play --offline` cuts the network; use a separate user account for more
isolation.
