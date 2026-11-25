# PvZ2Native WDRPC

[![Release](https://img.shields.io/github/v/release/Onyxdum/PvZ2NativeWDRPC?label=Release)](https://github.com/Onyxdum/PvZ2NativeWDRPC/releases/latest)
[![Platform](https://img.shields.io/badge/Platform-Windows%20%7C%20Linux-blue)](#)
[![PvZ2](https://img.shields.io/badge/PvZ2-v4.5.2%20%7C%20v6.6.1-green)](#)
[![Based on](https://img.shields.io/badge/Based%20on-ea192000%2FPvZ2Native-orange)](https://github.com/ea192000/PvZ2Native)
[![Language](https://img.shields.io/badge/Language-ES%20%7C%20EN-purple)](#)

> **Aviso / Notice:**  
> Este proyecto parte de [ea192000/PvZ2Native](https://github.com/ea192000/PvZ2Native) (que a su vez viene de [OptiJuegos/PvZ2Native](https://github.com/OptiJuegos/PvZ2Native)) y lo desarrolla y mantiene **[Onyxdum](https://github.com/Onyxdum)**. **No incluye el juego**: necesitas tu propio `libPVZ2.so` y tu `.obb`, sacados legalmente de la versión de Android.  
> This project builds on [ea192000/PvZ2Native](https://github.com/ea192000/PvZ2Native) (itself based on [OptiJuegos/PvZ2Native](https://github.com/OptiJuegos/PvZ2Native)) and is developed and maintained by **[Onyxdum](https://github.com/Onyxdum)**. **The game is not included**: you need your own `libPVZ2.so` and `.obb`, taken legally from the Android release.

---

## Español

**Plants vs. Zombies 2 en PC, y se juega como un juego de PC.** Ejecuta la `libPVZ2.so` real de Android de forma nativa en Windows y Linux, y le añade lo que esperarías de una versión de escritorio. Un mismo ejecutable sirve para **v4.5.2** y **v6.6.1**.

### Qué trae
- **Battlez vuelve a funcionar (v6.6.1):** EA apagó sus servidores hace años, así que este fork tiene el suyo. Partidas **en vivo** contra otra persona en el mismo nivel a la vez, o contra la **repetición** de otro jugador si no hay nadie conectado. Torneos semanales con ligas, coronas y rachas, premio para todos al acabar la semana y [clasificación global en la web](https://pvz2.onyxdum.eu/battlez). Los rivales salen con su nombre y foto de Discord.
- **Amigos:** lista con su nombre y foto de Discord, si están conectados, en qué nivel están y cuánto llevan. Se añaden con un enlace de invitación y os podéis mandar un regalo diario de monedas o gemas.
- **Guardado en la nube:** en la nube por defecto (iniciando sesión con Discord), en tu propio servidor, en Google Drive o en GitHub Gist. Va en segundo plano, guarda copias antes de sobrescribir nada y avisa con una pequeña notificación.
- **Discord Rich Presence:** tu estado enseña el mundo, el nivel, el modo, los objetivos, el jefe y su fase, en seis idiomas.
- **Modo mando estilo consola** como en la versión de Xbox 360: selector de casillas en el jardín, iconos de los botones en pantalla, vibración, selector de semillas, cinta transportadora, mapa del mundo y menú de ajustes. Si tocas el ratón o el teclado se aparta solo.
- **Dificultades extra:** Hard+ (siempre la dificultad más alta), Hard++ (sin cortacéspedes y el triple de zombis) y Point Zero (el cuádruple de zombis y las pociones de la Edad Media en todos los jardines). Por defecto no tocan los minijuegos ni Battlez.
- **Actualizador automático** en Windows y Linux: te avisa de las versiones nuevas, comprueba la descarga y, si algo falla, lo deja todo como estaba. Nunca toca tus partidas y conserva tus ajustes.
- **Escritorio:** asistente de primer arranque (le das el APK y el OBB y lo prepara todo), opción de añadirlo a Steam con sus imágenes, ajustes dentro del juego, logros, atajos de teclado y créditos finales.

### Controles del mando en un nivel
| Botón | Qué hace |
| --- | --- |
| Cruceta / stick izquierdo | Mover el selector por el jardín |
| `LB` / `RB` / stick derecho | Elegir sobre de semillas |
| `A` | Plantar en la casilla |
| `X` / `Y` | Pala / Fertilizante |
| `B` | Quitar lo que tengas elegido |
| `Select` | Potenciadores |
| `RT` | Plantas que se apuntan (Lanzaplátanos y parecidas) |
| `LT` / `Start` | Velocidad x2 / Pausa |
| `L3` / `R3` | Empezar la Última Resistencia / Cursor libre |

`B`, `X`, `Y`, `L3` y `R3` se pueden cambiar en **Ajustes → Mando**.

### Instalación
1. Descarga el zip de tu sistema desde [Releases](https://github.com/Onyxdum/PvZ2NativeWDRPC/releases/latest) y descomprímelo.
2. Abre `pvz2.exe` (Windows) o `pvz2` / `pvz2.AppImage` (Linux).
3. La primera vez se abre un asistente: indícale el APK (o XAPK / ZIP) y el OBB de la **v4.5.2** o la **v6.6.1**, y él saca `libPVZ2.so`, copia el OBB, escribe `config.ini` y arranca el juego.

Si prefieres hacerlo a mano, pon `libPVZ2.so` y `data.obb` junto al ejecutable (en la 6.6.1 también `libNimble.so`). En macOS se puede jugar con [Whisky](https://getwhisky.app/) o CrossOver usando la versión de Windows.

Casi todo se cambia desde los ajustes del juego (`Esc`). Lo avanzado sigue en `config.ini`, y casi todas las opciones también se pueden pasar por línea de comandos, por ejemplo `--fullscreen=0`.

---

## English

**Plants vs. Zombies 2 on PC, and it plays like a PC game.** It runs the real Android `libPVZ2.so` natively on Windows and Linux and adds what you'd expect from a desktop release. One executable runs both **v4.5.2** and **v6.6.1**.

### What you get
- **Battlez works again (v6.6.1):** EA shut its servers down years ago, so this fork runs its own. Play **live** against another person on the same level at the same time, or against another player's **replay** when nobody is online. Weekly tournaments with leagues, crowns and streaks, a reward for everyone when the week ends and a [global leaderboard on the web](https://pvz2.onyxdum.eu/battlez). Opponents show up with their Discord name and picture.
- **Friends:** a list with Discord names and pictures, who's online, what level they're on and how far along they are. Add people with an invite link and send each other a daily gift of coins or gems.
- **Cloud saves:** to the default cloud (sign in with Discord), your own server, Google Drive or GitHub Gist. Runs in the background, backs up before replacing anything and tells you how it went with a small notification.
- **Discord Rich Presence:** your status shows the world, level, mode, objectives, the boss and its phase, in six languages.
- **Console-style controller mode** like the Xbox 360 version: a tile selector on the lawn, button prompts on screen, rumble, seed chooser, conveyor belts, world map and settings menu. Touch the mouse or keyboard and it gets out of the way.
- **Extra difficulty modes:** Hard+ (always the top difficulty), Hard++ (no lawn mowers and three times the zombies) and Point Zero (four times the zombies plus the Dark Ages potions on every lawn). By default they leave minigames and Battlez alone.
- **Auto-updater** on Windows and Linux: tells you about new versions, verifies the download and puts everything back if something goes wrong. It never touches your saves and keeps your settings.
- **Desktop touches:** a first-boot wizard (give it the APK and OBB and it sets everything up), an option to add the game to Steam with artwork, in-game settings, achievements, keyboard shortcuts and the ending credits.

### Controller in a level
| Button | What it does |
| --- | --- |
| D-pad / left stick | Move the selector around the lawn |
| `LB` / `RB` / right stick | Pick a seed packet |
| `A` | Plant on the tile |
| `X` / `Y` | Shovel / Plant Food |
| `B` | Drop whatever you picked |
| `Select` | Power-ups |
| `RT` | Aimed plants (Banana Launcher and the like) |
| `LT` / `Start` | 2x speed / Pause |
| `L3` / `R3` | Start a Last Stand / Free cursor |

`B`, `X`, `Y`, `L3` and `R3` can be changed in **Settings → Controller**.

### Installing
1. Download the zip for your system from [Releases](https://github.com/Onyxdum/PvZ2NativeWDRPC/releases/latest) and unzip it.
2. Run `pvz2.exe` (Windows) or `pvz2` / `pvz2.AppImage` (Linux).
3. The first time, a wizard opens: point it at the **v4.5.2** or **v6.6.1** APK (or XAPK / ZIP) and OBB, and it extracts `libPVZ2.so`, copies the OBB, writes `config.ini` and starts the game.

If you'd rather do it by hand, put `libPVZ2.so` and `data.obb` next to the executable (plus `libNimble.so` for 6.6.1). On macOS you can play the Windows build through [Whisky](https://getwhisky.app/) or CrossOver.

Almost everything can be changed from the in-game settings (`Esc`). Advanced options are still in `config.ini`, and most of them can also be passed on the command line, e.g. `--fullscreen=0`.

---

## Cómo funciona / How it works

Solo se emula la **CPU**: PvZ2 nunca se compiló para x86, así que [dynarmic](dynarmic/) traduce sus instrucciones ARM a x86_64 sobre la marcha. Todo lo demás (Android, JNI, libc, OpenGL ES, el sistema de archivos) se reimplementa de forma nativa, la misma idea que Wine.  
Only the **CPU** is emulated: PvZ2 was never built for x86, so [dynarmic](dynarmic/) translates its ARM instructions to x86_64 on the fly. Everything else (Android, JNI, libc, OpenGL ES, the file system) is reimplemented natively, the same idea as Wine.

```text
libPVZ2.so (ARM32, untouched)
        │  ARM instructions
        ▼
dynarmic: JIT ARM32 → x86_64          (the only emulated part)
        │  "Android" calls
        ▼
Compatibility layer (this project)
  fake JNI / JavaVM · libc / libm / libz
  OpenGL ES → OpenGL · OpenSL ES → SDL
  own guest heap · VFS (paths + .obb)
        │
        ▼
Windows / Linux x64 · SDL2 · OpenGL 2.0
```

The code lives in [pvz2native/](pvz2native/). Only [game/symbols.cpp](pvz2native/src/game/symbols.cpp) may hold addresses from the `.so`; the game version is recognised at boot by a byte fingerprint of known functions, so supporting another version means adding data, not rewriting logic.

| Versión / Version | Estado / Status |
| --- | --- |
| **4.5.2** (2016) | 🟢 Jugable / Playable |
| **6.6.1** (2018) | 🟡 En pruebas / In testing (Arena todavía no / not yet) |
| **1.6.10** (2013) | 🟢 Jugable / Playable |

## Compilar / Building

Necesitas / You need: 64-bit Windows or Linux, CMake, a C++20 compiler (Visual Studio, MinGW-w64, GCC or Clang), OpenGL 2.0 and Boost headers in `third_party/boost_1_84_0`.

| | |
| --- | --- |
| Windows (Visual Studio + Ninja) | `compile-ninja.bat` → `out\build\x64-Release\pvz2native\pvz2.exe` |
| Windows (MinGW) | `compile.bat` → `build\pvz2native\pvz2.exe` |
| Linux | `./compile.sh` → `build/pvz2native/pvz2` (`--clean`, `--debug`, `--jobs N`, `--help`) |
| Linux AppImage | `./compile-appimage.sh` → `build/pvz2native/pvz2.AppImage` |

On Linux the UI assets go in `assets/` next to the loose binary; on Windows they're inside the `.exe`, and the AppImage carries its own.

## Créditos / Credits
- **Fork author & maintainer:** [Onyxdum](https://github.com/Onyxdum)
- **Based on:** [ea192000/PvZ2Native](https://github.com/ea192000/PvZ2Native) and [OptiJuegos/PvZ2Native](https://github.com/OptiJuegos/PvZ2Native)
- **Libraries:** [dynarmic](https://github.com/lioncash/dynarmic), [SDL](https://github.com/libsdl-org/SDL), zlib, glad, [stb](https://github.com/nothings/stb)

## Licencia / License
El código es MIT ([LICENSE](LICENSE)). *Plants vs. Zombies 2* y todos sus recursos son de Electronic Arts / PopCap Games; este proyecto no distribuye ningún archivo del juego y no tiene relación con ellos.  
The code is MIT ([LICENSE](LICENSE)). *Plants vs. Zombies 2* and all its assets belong to Electronic Arts / PopCap Games; this project doesn't distribute any game files and isn't affiliated with them.
