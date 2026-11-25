# PvZ2Native WDRPC

📖 [Español](README_SPANISH.md) · **English**

**Plants vs. Zombies 2 on PC, and it plays like a PC game.** This is a fork of [ea192000/PvZ2Native](https://github.com/ea192000/PvZ2Native), itself based on [OptiJuegos/PvZ2Native](https://github.com/OptiJuegos/PvZ2Native). Those projects run the real Android `libPVZ2.so` natively on Windows and Linux. This fork adds what you'd expect from a desktop release: **Discord Rich Presence**, **cloud saves**, **Battlez against real people again (live or against replays)**, a **friends list**, a **console-style controller mode with rumble**, **extra difficulty modes**, **Steam integration** and an **auto-updater**. One executable runs both **v4.5.2** and **v6.6.1**.

> [!WARNING]
> This is an experimental project and **the game is not included**. You need your own `libPVZ2.so` and `.obb`, taken legally from the Android release.

---

## 🌟 What this fork adds

### 🎮 Console-style controller mode
Plays a lot like the Xbox 360 version of the original game:

| In a level | |
| --- | --- |
| D-pad / left stick | Move the tile selector around the lawn |
| `LB` / `RB` / right stick | Pick a seed packet |
| `A` | Plant on the selected tile |
| `X` / `Y` | Shovel / Plant Food on the selected tile |
| `B` | Clear the seed or power-up you picked |
| `Select` | Pick a power-up, then `A` to use it on the selected tile |
| `RT` | Aimed plants (Banana Launcher and the like): pick, then fire |
| `LT` | 2x speed |
| `Start` | Pause |
| `L3` | Start a Last Stand once your defence is ready |
| `R3` | Switch to the free cursor |

- `B`, `X`, `Y`, `L3` and `R3` follow **Settings → Controller**, so you can put the shovel, Plant Food or a power-up on whichever button you like.
- Sun and coins get picked up as soon as the selector passes over them.
- **Conveyor belt** levels work too: the right stick slides the selector along the belt.
- **Seed chooser**: move over the cards and your seed bank with the selector. `A` picks or returns a plant, `X` brings back your last plants, `RB` upgrades, `LT` opens the Zen Garden, `RT` shows the lawn and `Y` starts the level.
- **World map**: `B` back, `LB` Almanac, `RB` Zen Garden, `X` Travel Log, `Y` Shop, `LT` Vasebreaker and `RT` the current event.
- **Settings menu**: D-pad to move, `A` to choose, `B` to go back.
- Each button's prompt is drawn right next to what it does.
- **Rumble** when you plant, on huge waves, explosions, Zomboss hits, lawn mowers, "Ready, Set, Plant!" and when you win or lose.
- Use the mouse or keyboard and the controller mode steps aside. Pick the controller up again and it's back.

### 💬 Discord Rich Presence
Your Discord status shows what you're up to, in your language (Spanish, English, French, German, Italian or Portuguese):

- The menu you're in (Almanac, Shop, Zen Garden, Travel Log) and, in a level, the world, whether it's day or night, and that world's icon.
- Modes like Vasebreaker, Piñata Party, Epic Quests and Endless Zones, with their localized names.
- A star (`⭐`) for each level objective, the Zombot's name and phase in boss fights, and whether you're choosing seeds, paused or collecting the reward.
- A button on your profile that links here.

It's on by default; all you need is Discord running:

```ini
[discord]
enabled = 1
client_id = 1560077802550853694
image = pvz2
button_url = https://github.com/Onyxdum/PvZ2NativeWDRPC
button_label = GitHub
```

### ☁️ Cloud saves
- A cloud button on the main menu (also in settings) with four choices: **Cloud**, your own server, **Google Drive** or **GitHub Gist**.
- Sign in with Discord for the default cloud. It can also sync for you when you quit.
- Export or import your save as a `.json` file.
- Uploads and downloads run in the background, so the game never freezes. With no internet you get a quick "no connection" message instead of a hang.
- Before a download replaces your local save, a copy goes to `cloud_backups/` (the last five are kept). If the cloud copy is older than your last upload, it asks you to press Download again to confirm.
- The server keeps your last five uploads, so an upload from a fresh install can be undone.
- 4.5.2 and 6.6.1 each have their own cloud save, so you can't load a save from the other version by mistake. If you're moving to 6.6.1, its cloud menu has a button to bring your 4.5.2 save over. It asks before doing anything and backs up your current save first.
- Results appear as a small notification at the top of the screen.

### 🥊 Battlez is back (v6.6.1)
EA shut down the Battlez servers a long time ago. This fork has its own server, so the mode works again:

- **Live matches**: Battlez looks for another player who is online for about 15 seconds. When it finds one, you both start the same level together and watch each other's score move on the Battlez meter as you play.
- **Replays when nobody is around**: if no one turns up, the game asks whether you want to play a recorded match from another player or cancel. Your own matches become replays for other people. With no replays yet, you face the game's AI.
- Prefer replays only? Turn off **Settings → Events → Live Battlez (multiplayer)**.
- **Your Discord identity**: Battlez needs the Discord account you use for cloud saves. The leaderboard and your opponents show Discord names and profile pictures.
- **Weekly tournaments** with the original leagues, crowns and win streaks, and no limit on leaderboard size.
- **Everyone gets a reward**: when the week ends, every player who took part gets the reward for their place (gems, coins), and the top players move up a league.
- **Global leaderboard** on the web: [pvz2.onyxdum.eu/battlez](https://pvz2.onyxdum.eu/battlez).
- The server rejects obviously faked results: duplicate match reports, matches that finish impossibly fast, and scores out of range.
- The extra difficulty modes never touch Battlez levels, so both players always get the same level.

### 👥 Friends (v4.5.2 and v6.6.1)
- Open the list from the envelope button in the top-left corner. On v6.6.1 it's a native HUD button that hides along with the game's own windows and fades.
- See your friends' Discord names and pictures, whether they're online, the world or level they're playing, and how far along their save is.
- Add friends with an invite link (`pvz2native://` or the web invite page). They can accept or reject, and removing someone takes two taps.
- Send and receive a daily gift of coins or gems.

### 💀 Extra difficulty modes
Under Settings → **Difficulty** you can pick any of the game's eight dynamic difficulty levels, or one of these:

- **Hard+**: always the highest dynamic difficulty.
- **Hard++**: no lawn mowers, and each wave brings three times the zombies, spread over every row.
- **Point Zero**: everything in Hard++, four times the zombies, plus the Dark Ages potions on every lawn, which zombies drink as they walk past.

### 🔄 Auto-updater (Windows and Linux)
- When the game starts, it checks for a new release in the background without slowing the launch.
- If there's one, it tells you which version it is and what's new, and lets you choose: **Update**, **Not now** or **Skip** (skip means it won't ask about that version again).
- **Update** closes the game the normal way, so your progress and cloud sync are saved. Then it downloads the update with a progress bar and checks the file against the size and SHA-256 that GitHub publishes.
- It backs up every file before replacing it. If something can't be written (a locked file, say), it puts everything back as it was. Either way, the game reopens at the end.
- Your saves and game files are never touched. `config.ini` and the presence rules are merged, not overwritten: you keep your settings and just get the new options.
- On Linux it works the same way, with one difference: it downloads in the background while you play and installs when you close the game. It picks the right package for you (the loose binary or the AppImage).
- To turn it off, set `[game] check_updates = 0`.

### 🚀 Desktop integration
- **Steam**: the first time you launch it, it offers to add the game to your Steam library, artwork included.
- **First-boot wizard**: point it at the APK and OBB (4.5.2 or 6.6.1) and it sets everything up for you.
- **Ending credits**: claiming the game's last reward rolls the credits.

---

## ✨ Also included (PC build)

Built for *Plants vs. Zombies 2: It's About Time* **v4.5.2** and **v6.6.1** on Windows and Linux.

### Looks and desktop integration
- Its own executable icon and cursor (the cursor scales with the window and screen).
- A startup splash and loading progress, instead of a long black screen.
- Proper version details on the Windows executable.

### In-game settings
- Open them with `Esc`, or with the **Google Play** button in the bottom-right corner of the main menu.
- Change the language, currency, fullscreen, monitor, FPS limit, difficulty, free purchases, events, keyboard shortcuts and controller options without touching any file.
- Most changes apply right away (even moving the window to another monitor). Language, free purchases and the Events options restart the game so the engine picks them up.
- Old or incomplete `config.ini` files get fixed at launch: missing keys get safe defaults and wrong values are corrected.

### Events
- Settings → **Events**: **Enable events** (`[runtime] use_cdn`) turns on or off the emulated CDN that delivers events and daily content. **Force local events** (`[runtime] rewrite_cdn`) copies the event files from the OBB again on every launch. Changing either one restarts the game.

### Achievements
- Local achievements that unlock as you play, saved in `prefs.dat`.
- An animated notification (with sound) when you unlock one. The full list is in settings (**View achievements**).
- You can turn off the notification and sound in settings or with `[game] achievement_notifications = 0`. Achievements still save.

### Exit confirmation
- Confirm or cancel before quitting, with the mouse, keyboard or controller.
- `Esc` opens settings, and pressing it again there opens this dialog (always `Esc`, can't be remapped). `F11` always toggles fullscreen.

### Configuration
- On first run it works out your language, currency and related values.
- The console/log window is hidden by default (`[log] console = 0`).
- Everyday options live in the settings screen; advanced ones are still in `config.ini`.
- Almost every option can be overridden from the command line, e.g. `--fullscreen=0`, `--video.fullscreen=0`, `--console=1`.

### Keyboard and mouse
- Configurable shortcuts that tap the screen for you (the UI was made for touch).
- Defaults include `1`–`8` for plant slots, `Space` for speed, `Enter` to pause, `F` for Plant Food, `S` for the shovel, `Q`/`W`/`E` for powers, `R` to replay and `Backspace` to go back, plus Almanac / connect / Vasebreaker / Zen Garden keys.
- Shortcuts can be edited from the settings screen.
- The mouse wheel scrolls lists (by simulating a drag), right-click goes back and middle-click opens the menu where there is one.
- Clicks land in the right place in fullscreen and after resizing, and input recovers if it ever gets stuck.

### Controller (menus)
- Outside levels the left stick moves the cursor and **A** clicks; the D-pad jumps between buttons on screen.
- Remappable actions, stick speed and deadzone sliders, and a choice of which controller to use (or none).
- Mouse, keyboard and controller all work at the same time.

### Other
- Text fields behave better (click outside one to stop typing).
- Smoother frame pacing, and less console output when logging is off.
- Help / License / Privacy links open in your browser.
- A player ID that stays the same for each installation.

---

## 🧠 How it works

**It runs the real native Android library of *Plants vs. Zombies 2* (`libPVZ2.so`, ARM32) on a PC.** Only the **CPU** is emulated: PvZ2 was only ever compiled for ARM (there's no x86/x64 build), so a JIT translates its ARM instructions to x86_64. Everything else (Android, JNI, libc, OpenGL ES, the file system) **isn't emulated but reimplemented natively**, the same idea as **Wine** with Windows programs.

## 📖 What it is (and what it isn't)

It isn't a *port* of the game's source code. It mixes **two techniques**, and the difference matters:

- **Only the CPU is emulated.** The game is ARM machine code and PvZ2 was never built for x86/x64, so a PC processor can't run it directly. A JIT (dynarmic) reads the ARM instructions and translates them to x86_64 as it goes. That's the only emulated part.
- **Android isn't emulated, it's reimplemented.** When the game calls an Android function (opening a file, drawing with OpenGL ES, allocating memory, calling Java…), **native PC code** answers the call and does the real work with the equivalent desktop API. There's no virtual Android underneath, just **new implementations** of the functions the game needs.

It's the same approach as **Wine** (*"Wine Is Not an Emulator"*), taken one step further. Wine reimplements Windows calls without emulating anything because `.exe` files are already x86. Here the game is ARM, so the CPU has to be emulated too. Otherwise the idea is the same: **translate the calls instead of simulating a whole machine.**

```text
┌──────────────────────────────────────────────┐
│  libPVZ2.so  (ELF ARM32, EABI5, untouched)   │   ← the real Android game
└───────────────────────┬──────────────────────┘
                        │  ARM instructions
                        ▼
┌──────────────────────────────────────────────┐
│  dynarmic:  JIT ARM32 → x86_64               │   ← the ONLY emulated part
└───────────────────────┬──────────────────────┘     (just the CPU)
                        │  "Android" calls
                        ▼
┌──────────────────────────────────────────────┐
│  Compatibility layer (this project)          │   ← NATIVE reimplementation,
│                                              │     Wine-style (nothing emulated)
│  Fake JNI / JavaVM   ·   libc / libm / libz  │
│  OpenGL ES → OpenGL  ·   OpenSL ES → SDL     │
│  Own GuestHeap       ·   VFS (paths + .obb)  │
└───────────────────────┬──────────────────────┘
                        │
                        ▼
┌──────────────────────────────────────────────┐
│  Windows / Linux x64  ·  SDL2  ·  OpenGL 2.0 │
└──────────────────────────────────────────────┘
```

---

## 🧩 Architecture

The code is in [pvz2native/](pvz2native/), split into layers with one strict rule: **only one layer may contain addresses from the `.so`.** That's why supporting another game version means adding data, not rewriting logic.

| Layer | What it handles | Can it hold a `.so` address? |
| --- | --- | :---: |
| [runtime/](pvz2native/src/runtime/) | Running ARM code (JIT, heap, threads, synchronization) | **Never** |
| [game/symbols.cpp](pvz2native/src/game/symbols.cpp) | Where each thing lives in *this* binary | **Only this one** |
| [engine/](pvz2native/src/engine/) | What to call and in what order (boot, lifecycle, frame) | Only through `sym()` |
| [dependencies/](pvz2native/src/dependencies/) | Answering each Android `.so` (libc, libm, GLES, …) | No |
| [dex/](pvz2native/src/dex/) | Answering the Java side (fake JNIEnv + per-class hooks) | No |
| [diagnostics/](pvz2native/src/diagnostics/) | Inspecting the guest (watchpoints, PC sampling) | Only through `sym()` |

Some of the main pieces:

- **JIT:** [dynarmic](dynarmic/) (A32 frontend only). One JIT per guest thread, with an identity *page table* over the `.so`'s memory for speed.
- **Native dependencies:** one module for each Android `.so` the game links against. libc is split by header (`string`, `stdio`, `stdlib`, `time`, `pthread`, `locale`, `ctype`, …), plus `libm`, `libz`, `liblog`, `libgles`, `libstdcxx`, `libopensles` and `libdl`. Each handler is a `void(GuestCall&)` registered by name.
- **Java side:** [jni_env.cpp](pvz2native/src/dex/jni_env.cpp) implements a fake `JNIEnv`/`JavaVM` (objects, strings, arrays, references), and [dex/hooks/](pvz2native/src/dex/hooks/) has one file per Java class (`AndroidGameApp`, `AndroidSurfaceView`, HTTP, Google Play, Facebook…).
- **Graphics:** the game's GLES calls are translated to **OpenGL 2.0 (compatibility profile)** through `glad`. SDL provides the window and GL context, doing the job of Android's `GLSurfaceView`.
- **Resources:** a VFS maps Android paths to PC paths and reads assets straight from the game's real `.obb` (RSB/RSG format).

---

## 🎮 Supported game versions

The version is recognised by a **byte fingerprint** of two known native functions. If nothing matches, it refuses to boot rather than run blind.

| Version | Typical OBB | Status |
| --- | --- | --- |
| **1.6.10** (2013) | `main.7.com.ea.game.pvz2_na.obb` | 🟢 Playable |
| **4.5.2** (2016) | `main.147.com.ea.game.pvz2_row.obb` (often shipped or renamed as `data.obb`) | 🟢 Playable |
| **6.6.1** (2018) | `main.434.com.ea.game.pvz2_row.obb` (or `data.obb` next to `libNimble.so`) | 🟡 In testing (Adventure, levels and Battlez playable; Arena not yet) |

A **single executable** handles every version. At boot, [symbols.cpp](pvz2native/src/game/symbols.cpp) identifies the version from ELF function fingerprints and loads the right patches, symbol offsets, companion libraries (`libNimble.so`) and Discord Rich Presence rules. 6.6.1 support is still being tested.

---

## ⚙️ Requirements

- **64-bit Windows or Linux** (the executable is static x86_64; there's no 32-bit build because dynarmic only has x86_64/arm64/riscv64 backends).
- **CMake** (3.5 or newer recommended).
- **Windows:** MinGW-w64 (GCC with C++20) **or** Visual Studio with C++ / CMake / Ninja (`compile-ninja.bat`).
- **Linux:** GCC or Clang with C++20.
- A **GPU with OpenGL 2.0** or newer.
- Your own **`libPVZ2.so`** (armeabi-v7a) and the matching **`.obb`**.
- **Boost headers** in `third_party/boost_1_84_0` (the build scripts expect them there).

---

## 🔨 Building

### Windows: Visual Studio + Ninja (recommended)

```bat
compile-ninja.bat
```

It finds Visual Studio (any edition) with `vswhere` and builds a Release x64 binary with Ninja. Output:

```text
out\build\x64-Release\pvz2native\pvz2.exe
```

### Windows: MinGW

If `gcc`, `g++` and `make` are on your `PATH`:

```bat
compile.bat
```

Output:

```text
build\pvz2native\pvz2.exe
```

### Linux

```bash
./compile.sh
```

Handy options: `--clean`, `--debug`, `--jobs N`, `--no-assets`, `--verbose`, `--help`.

Output:

```text
build/pvz2native/pvz2
```

`compile.sh` also copies the UI assets to `build/pvz2native/assets/` (splash, settings art, cursor, achievements). On Windows they're embedded in the `.exe` through `windows_resources.rc`; on Linux they have to sit next to the binary in `assets/`.

### Linux: AppImage

```bash
./compile-appimage.sh
```

Builds with `compile.sh`, then packs a type-2 AppImage. Output:

```text
build/pvz2native/pvz2.AppImage
```

The AppImage contains **only** the `pvz2` binary and `assets/`. Put `libPVZ2.so`, `data.obb` and `config.ini` **next to** the `.AppImage` (same layout as the loose binary). The first run may download `appimagetool` into `tools/` if it's missing.

---

## ▶️ Running

Put the Android game files **next to the executable** (not in a `lib/` subfolder). By default it looks for `libPVZ2.so` and `data.obb` in that same folder:

```text
<folder with pvz2 / pvz2.exe / pvz2.AppImage>/
├── pvz2(.exe)  or  pvz2.AppImage
├── config.ini                 ← created / repaired on first launch
├── libPVZ2.so                 ← required (armeabi-v7a)
├── data.obb                   ← required (or set [paths] obb to your OBB's name)
└── assets/                    ← Linux loose binary: UI/splash/cursor/achievements
                                   (Windows: inside the exe; AppImage: inside the image)
```

You can keep the OBB's original name and point to it in `config.ini`:

```ini
[paths]
so  = libPVZ2.so
obb = main.147.com.ea.game.pvz2_row.obb
```

Relative paths are relative to the executable's folder. Absolute paths work too.

**First-boot wizard:** if `libPVZ2.so` or `data.obb` are missing at launch, a setup wizard opens. It accepts **v4.5.2** and **v6.6.1** APKs (or XAPK / ZIP files) and OBBs. It extracts `libPVZ2.so` and anything else needed (`libNimble.so` for 6.6.1), copies the OBB, detects the package variant (`pvz2_row` or `pvz2_na`) and the game version, writes `config.ini` and starts the game. If the files are already there, the game just starts.

On first launch the game writes a commented `config.ini`. The settings screen covers everyday options; advanced keys stay in the file and on the command line.

### 🍎 macOS (through Whisky / Wine)
There are no native macOS builds: Apple has deprecated OpenGL and restricts JIT compilers on Apple Silicon. You can still play with **[Whisky](https://getwhisky.app/)** (free and open source) or **CrossOver**:
1. Install **Whisky**.
2. Create a Windows 10 bottle.
3. Run `pvz2.exe` from the Windows release.

---

## 🔧 Configuration (`config.ini`)

The main configuration file, for paths, display, input and game options:

| Section | Key | What it does |
| --- | --- | --- |
| `[paths]` | `so`, `obb` | Paths to `libPVZ2.so` and `data.obb` (relative to the executable, or absolute). |
| `[paths]` | `save` | Optional save folder; if unset, `prefs.dat` goes next to the executable. |
| `[log]` | `console` | Show the console/log output (`1` shows it, `0` hides it). |
| `[log]` | `verbose`, `trace`, `input` | Detailed logs for the engine lifecycle, imports and input (needs `console=1` or `logfile`). |
| `[video]` | `fullscreen`, `display`, `width`, `height`, `fps_limit`, `lock_cursor` | Display and monitor (also in settings). |
| `[game]` | `user_locale`, `currency_*`, `emulate_iap` | Language/region, store currency and free purchases (also in settings). |
| `[game]` | `achievement_notifications` | Unlock notification and sound (`1` by default; `0` only silences them). |
| `[game]` | `check_updates` | Look for new versions at startup (`1` by default). |
| `[runtime]` | `use_cdn`, `rewrite_cdn` | Emulated CDN on/off (on by default) and copying it again from the OBB (also in settings → Events). |
| `[runtime]` | `allow_repeat_quests` | `1` (default): repeatable quests you already claimed count kills again instead of completing on their own when they come back (the save's list of finished quests is cleaned at launch). `0` leaves the save alone. |
| `[shortcuts]` | `bind_*` | Keyboard → on-screen tap (also in settings). |
| `[board]` / `[gamepad]` / `[gamepad_binds]` | … | Controller focus grid and buttons (also in settings). |
| `[discord]` | `enabled`, `client_id`, `image`, `rules` | Discord Rich Presence (on by default). |

---

## ⌨️ Controls (host)

| Action | Control |
| --- | --- |
| Tap / select | Left click (or controller **A**) |
| Drag (swipe) | Left click + move (or right stick) |
| Scroll lists | Mouse wheel |
| Back (menus) | Right click / `Backspace` / controller Back (same as Esc: opens settings, then the quit dialog) |
| Settings → quit dialog | `Esc` (fixed): the first press opens settings, the second opens the quit dialog |
| Toggle fullscreen | `F11` (fixed) |
| Settings | Google Play button on the main menu |
| Achievements | From settings (**View achievements**) |
| Shortcuts / controller | Set them up in settings |
| Controller in levels | See [Console-style controller mode](#-console-style-controller-mode) |

---

## 📜 License

The code is under the **MIT license** (see [LICENSE](LICENSE)). It uses third-party projects (dynarmic, SDL, zlib, glad, stb), each under its own license.

**Plants vs. Zombies 2**, its trademarks and all its assets belong to **Electronic Arts / PopCap Games**. This project **doesn't distribute or replace** any game files: you need your own, obtained legally.

## ⚠️ Disclaimer

This is an independent project, **not affiliated with, endorsed by or sponsored by** Electronic Arts, PopCap Games or anyone connected to Plants vs. Zombies. Its purpose is purely **technical and educational**: research into ARM binary emulation, compatibility layers and API translation.

---

<p align="center">
  <b>PvZ2Native</b>: Android's <code>libPVZ2.so</code> running on PC through ARM emulation
</p>
