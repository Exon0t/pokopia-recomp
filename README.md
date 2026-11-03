<h1 align="center">🏝️ Pokopia Recomp</h1>

<p align="center">
  <b>Pokémon Pokopia (Switch 2) recompiled to run natively on Windows, with multiplayer support.</b>
</p>

<p align="center">
  <a href="https://github.com/featureJosh/pokopia-recomp/releases/latest"><img src="https://img.shields.io/github/v/release/featureJosh/pokopia-recomp?label=latest&color=d8b46a&labelColor=1b2740" alt="Latest release"></a>
  <a href="https://github.com/featureJosh/pokopia-recomp/releases/latest"><img src="https://img.shields.io/github/downloads/featureJosh/pokopia-recomp/latest/total?label=latest%20downloads&color=3fb950&labelColor=1b2740" alt="Latest release downloads"></a>
  <a href="https://github.com/featureJosh/pokopia-recomp/releases"><img src="https://img.shields.io/github/downloads/featureJosh/pokopia-recomp/total?label=downloads&color=3fb950&labelColor=1b2740" alt="Total downloads"></a>
  <img src="https://img.shields.io/badge/platform-Windows%2010%2F11-0078d6?logo=windows&logoColor=white&labelColor=1b2740" alt="Platform: Windows 10/11">
  <a href="https://github.com/featureJosh/pokopia-recomp/commits/main"><img src="https://img.shields.io/github/last-commit/featureJosh/pokopia-recomp/main?label=last%20commit&color=58a6ff&labelColor=1b2740" alt="Last commit on main"></a>
  <a href="https://github.com/featureJosh/pokopia-recomp/issues"><img src="https://img.shields.io/github/issues/featureJosh/pokopia-recomp?label=open%20issues&color=db61a2&labelColor=1b2740" alt="Open issues"></a>
</p>

<p align="center">
  <a href="https://github.com/featureJosh/pokopia-recomp/releases/latest"><img src="https://img.shields.io/badge/%E2%AC%87_Download-latest_for_Windows-d8b46a?style=for-the-badge&labelColor=1b2740" alt="Download the latest release for Windows"></a>
</p>

<p align="center">
  <b><a href="https://github.com/featureJosh/pokopia-recomp/releases">Releases</a></b> ·
  <a href="CHANGELOG.md">Changelog</a> ·
  <a href="CONTRIBUTING.md">Contributing</a> ·
  <a href="CREDITS.md">Credits</a> ·
  <a href="SECURITY.md">Security</a> ·
  <a href="LEGAL.md">Legal</a> ·
  <a href="https://github.com/featureJosh/pokopia-recomp/issues">Issues</a>
</p>

> [!IMPORTANT]
> **No game files are included.** You need your own extracted copy of the game, version **2.0.1a**. This project provides no game files, keys or dumping tools. Not affiliated with Nintendo, The Pokémon Company or Koei Tecmo; Pokémon is a trademark of Nintendo, Creatures Inc. and GAME FREAK inc. (see [LEGAL.md](LEGAL.md)).

> [!NOTE]
> **Built with AI, supervised by a human.** This project is developed primarily with AI, with me as the supervisor. I have a handful of years of development experience, so it's not all slop. Every commit is reviewed, verified and tested by me before it lands.

## Screenshots

<table>
  <tr>
    <td width="50%"><img src="docs/assets/screenshots/title-screen.svg" alt="Title screen (screenshot placeholder)"><br><sub>Title screen</sub></td>
    <td width="50%"><img src="docs/assets/screenshots/island-gameplay.svg" alt="Island gameplay (screenshot placeholder)"><br><sub>Building your island</sub></td>
  </tr>
  <tr>
    <td width="50%"><img src="docs/assets/screenshots/item-placement.svg" alt="Item placement (screenshot placeholder)"><br><sub>Placing items with Joy-Con 2 mouse mode</sub></td>
    <td width="50%"><img src="docs/assets/screenshots/link-play.svg" alt="Link Play (screenshot placeholder)"><br><sub>Link Play with a friend (work in progress)</sub></td>
  </tr>
</table>

## Progress

<p align="center"><img src="docs/assets/progress.svg" alt="Progress bars: single player, on-screen keyboard, fast boot and gamepad prompts done; handheld support at 60 percent; multiplayer at 15 percent" width="100%"></p>

Handheld support covers Steam Deck, Legion Go S and SteamOS. Link Play (two players on one island, PC ↔ PC through a relay) is being built on the [`experimental-coop`](https://github.com/featureJosh/pokopia-recomp/tree/experimental-coop) branch. A first session has worked, but it is not in any release yet. Progress, screenshots and known issues: [#54](https://github.com/featureJosh/pokopia-recomp/issues/54). Builds from `main` stay offline. What changed per version: [CHANGELOG.md](CHANGELOG.md).

## Requirements

- Windows 10 or 11, 64-bit
- GPU with Vulkan 1.3
- 16 GB of RAM

Linux, SteamOS and the Steam Deck: the launcher AppImage, the Linux build and the setup are in [linux/README.md](linux/README.md).

## Quick start

1. Download the zip from the [latest release](https://github.com/featureJosh/pokopia-recomp/releases/latest) and extract it anywhere.
2. Run `PokopiaLauncher.exe`. If SmartScreen warns, choose **More info → Run anyway**. Pick your dump folder and press **Set up**: it compiles the game on your PC (about 7 minutes), then builds the shader cache (1 to 2 hours, you can pause and continue later).
3. Press **Play**.

F1 opens the settings in game. Middle mouse toggles Joy-Con 2 mouse mode for placing items.

## Reporting problems

[Open an issue](https://github.com/featureJosh/pokopia-recomp/issues/new/choose) with `pkp.log` from the session where it happened, your GPU and your Windows version.

## License

GPL-3.0. Third-party code keeps its own licence, see [CREDITS.md](CREDITS.md).
