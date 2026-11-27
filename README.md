# PvZ2Native WDRPC

[![Release](https://img.shields.io/github/v/release/Onyxdum/PvZ2NativeWDRPC?label=Release)](https://github.com/Onyxdum/PvZ2NativeWDRPC/releases/latest)
[![Platform](https://img.shields.io/badge/Platform-Windows%20%7C%20Linux-blue)](#)
[![PvZ2](https://img.shields.io/badge/PvZ2-v4.5.2%20%7C%20v6.6.1-green)](#)
[![Based on](https://img.shields.io/badge/Based%20on-ea192000%2FPvZ2Native-orange)](https://github.com/ea192000/PvZ2Native)
[![Language](https://img.shields.io/badge/Language-ES%20%7C%20EN-purple)](#)

> **Aviso / Notice:**  
> Este fork parte de [ea192000/PvZ2Native](https://github.com/ea192000/PvZ2Native) (basado a su vez en [OptiJuegos/PvZ2Native](https://github.com/OptiJuegos/PvZ2Native)) y lo mantiene **[Onyxdum](https://github.com/Onyxdum)**. **No incluye el juego**: necesitas tu propio `libPVZ2.so` y tu `.obb` de la versión de Android.  
> This fork is based on [ea192000/PvZ2Native](https://github.com/ea192000/PvZ2Native) (itself based on [OptiJuegos/PvZ2Native](https://github.com/OptiJuegos/PvZ2Native)) and maintained by **[Onyxdum](https://github.com/Onyxdum)**. **The game is not included**: you need your own `libPVZ2.so` and `.obb` from the Android version.

---

## Español

**Plants vs. Zombies 2 nativo en PC.** Ejecuta la `libPVZ2.so` original de Android en Windows y Linux, con soporte de mando, atajos de teclado, ajustes dentro del juego y más. El mismo ejecutable sirve tanto para la **v4.5.2** como para la **v6.6.1**.

### Qué trae
- **Batallaz (v6.6.1):** como EA cerró los servidores hace años, este fork usa uno propio. Puedes jugar **en vivo** contra otro jugador en el mismo nivel a la vez, o contra una **repetición** si no hay nadie buscando partida. Tiene torneos semanales con ligas, coronas, rachas, recompensas al acabar la semana y [clasificación global en la web](https://pvz2.onyxdum.eu/battlez). Los rivales salen con su nombre y foto de Discord.
- **Amigos:** ves la lista de amigos con su nombre y foto de Discord, si están conectados, en qué nivel están y cuánto llevan jugando. Se añaden con un enlace de invitación y pueden mandarse un regalo diario de monedas o gemas.
- **Guardado en la nube:** funciona con el servidor del proyecto (iniciando sesión con Discord), con tu propio servidor, Google Drive o GitHub Gist. Sube y baja la partida en segundo plano, hace copia de seguridad antes de sobrescribir nada y te avisa con una notificación.
- **Discord Rich Presence:** muestra el mundo, el nivel, el modo de juego, los objetivos, el jefe y su fase, en seis idiomas.
- **Modo mando** (al estilo de la versión de Xbox 360): selector de casillas en el jardín, botones en pantalla, vibración, selector de semillas, cinta transportadora, mapa del mundo y menú de ajustes. En cuanto mueves el ratón o tocas el teclado, vuelve a ratón y teclado automáticamente.
- **Dificultades extra:** Hard+ (siempre en la dificultad más alta), Hard++ (sin podadoras y el triple de zombis) y Point Zero (el cuádruple de zombis y las pociones de Edad Oscura en todos los jardines). Por defecto no afectan a los minijuegos ni a Batallaz.
- **Actualizador automático** en Windows y Linux: te avisa cuando sale una versión nueva, comprueba la descarga y, si algo sale mal, vuelve a dejarlo como estaba sin tocar tus partidas ni tus ajustes.
- **Extras de PC:** asistente en el primer arranque (le pasas el APK y el OBB y lo prepara todo), opción para añadirlo a Steam con portadas, menú de ajustes dentro del juego, logros, atajos de teclado y créditos finales.

### Controles del mando en nivel
| Botón | Acción |
| --- | --- |
| Cruceta / stick izquierdo | Mover el selector por el jardín |
| `LB` / `RB` / stick derecho | Seleccionar planta |
| `A` | Plantar en la casilla |
| `X` / `Y` | Pala / Nutriente |
| `B` | Cancelar selección |
| `Select` | Potenciadores |
| `RT` | Plantas manuales (Lanzaplátanos y similares) |
| `LT` / `Start` | Velocidad x2 / Pausa |
| `L3` / `R3` | Empezar El último batallón / Cursor libre |

`B`, `X`, `Y`, `L3` y `R3` se pueden cambiar en **Ajustes → Mando**.

### Instalación
1. Descarga el `.zip` de tu sistema en [Releases](https://github.com/Onyxdum/PvZ2NativeWDRPC/releases/latest) y descomprímelo.
2. Abre `pvz2.exe` (Windows) o `pvz2` / `pvz2.AppImage` (Linux).
3. La primera vez saldrá un asistente: selecciona el APK (o XAPK / ZIP) y el OBB de la **v4.5.2** o la **v6.6.1**; extraerá `libPVZ2.so`, copiará el OBB, creará `config.ini` y abrirá el juego.

Si prefieres hacerlo a mano, pon `libPVZ2.so` y `data.obb` junto al ejecutable (en la 6.6.1 hace falta también `libNimble.so`). En macOS funciona la versión de Windows usando [Whisky](https://getwhisky.app/) o CrossOver.

Casi todo se puede cambiar desde los ajustes del juego (`Esc`). Lo avanzado está en `config.ini`, y la mayoría de opciones también se pueden pasar por línea de comandos (por ejemplo, `--fullscreen=0`).

---

## English

**Plants vs. Zombies 2 running natively on PC.** It runs the original Android `libPVZ2.so` on Windows and Linux, with controller support, keyboard shortcuts, in-game settings, and more. The same executable works for both **v4.5.2** and **v6.6.1**.

### What's included
- **Battlez (v6.6.1):** since EA shut down the servers years ago, this fork runs its own. You can play **live** against another player on the same level at the same time, or play against a **replay** when nobody else is queuing. Weekly tournaments with leagues, crowns, streaks, end-of-week rewards, and a [global web leaderboard](https://pvz2.onyxdum.eu/battlez). Opponents show up with their Discord name and avatar.
- **Friends:** see your friends' Discord names and avatars, whether they're online, what level they're playing, and how long they've been in it. Add each other with an invite link and send a daily gift of coins or gems.
- **Cloud saves:** works with the project's server (sign in with Discord), your own server, Google Drive, or GitHub Gist. Runs in the background, makes a backup before overwriting anything, and shows a notification when it finishes.
- **Discord Rich Presence:** shows the world, level, mode, objectives, boss, and boss phase in six languages.
- **Controller mode** (inspired by the Xbox 360 version): tile selector on the lawn, button prompts on screen, rumble, seed chooser, conveyor belts, world map, and settings menu. Move the mouse or press a key and it switches back automatically.
- **Extra difficulties:** Hard+ (always max difficulty), Hard++ (no lawn mowers and triple zombies), and Point Zero (quadruple zombies plus Dark Ages potions on every lawn). Minigames and Battlez aren't affected by default.
- **Auto-updater** on Windows and Linux: notifies you when a new version is out, verifies the download, and restores the previous files if anything goes wrong without touching your saves or settings.
- **PC extras:** first-launch setup wizard (pick your APK and OBB and it sets everything up), option to add the game to Steam with cover art, in-game settings, achievements, keyboard shortcuts, and ending credits.

### Controller in a level
| Button | Action |
| --- | --- |
| D-pad / left stick | Move the selector around the lawn |
| `LB` / `RB` / right stick | Pick a seed packet |
| `A` | Plant on the tile |
| `X` / `Y` | Shovel / Plant Food |
| `B` | Cancel selection |
| `Select` | Power-ups |
| `RT` | Aimed plants (Banana Launcher, etc.) |
| `LT` / `Start` | 2x speed / Pause |
| `L3` / `R3` | Start Last Stand / Free cursor |

`B`, `X`, `Y`, `L3`, and `R3` can be rebound in **Settings → Controller**.

### Installation
1. Download the `.zip` for your OS from [Releases](https://github.com/Onyxdum/PvZ2NativeWDRPC/releases/latest) and extract it.
2. Run `pvz2.exe` (Windows) or `pvz2` / `pvz2.AppImage` (Linux).
3. On first launch, a wizard will open: pick the **v4.5.2** or **v6.6.1** APK (or XAPK / ZIP) and OBB; it will extract `libPVZ2.so`, copy the OBB, write `config.ini`, and launch the game.

If you'd rather do it manually, put `libPVZ2.so` and `data.obb` next to the executable (and `libNimble.so` for 6.6.1). On macOS, you can run the Windows build with [Whisky](https://getwhisky.app/) or CrossOver.

Almost everything can be changed from the in-game settings (`Esc`). Advanced options live in `config.ini`, and most can also be passed via command-line flags (for example, `--fullscreen=0`).

---

## Cómo funciona / How it works

Solo se emula la **CPU**: PvZ2 nunca salió para x86, así que [dynarmic](dynarmic/) traduce sus instrucciones ARM a x86_64 sobre la marcha. Todo lo demás (Android, JNI, libc, OpenGL ES y el sistema de archivos) está reimplementado de forma nativa, parecido a como funciona Wine.  
Only the **CPU** is emulated: PvZ2 was never built for x86, so [dynarmic](dynarmic/) translates its ARM instructions to x86_64 on the fly. Everything else (Android, JNI, libc, OpenGL ES, and the file system) is reimplemented natively, similar to how Wine works.

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

El código está en [pvz2native/](pvz2native/). Solo [game/symbols.cpp](pvz2native/src/game/symbols.cpp) guarda direcciones del `.so`; la versión del juego se reconoce al arrancar comprobando la firma de bytes de funciones conocidas, así que añadir otra versión consiste en añadir datos, no en reescribir la lógica.  
The code lives in [pvz2native/](pvz2native/). Only [game/symbols.cpp](pvz2native/src/game/symbols.cpp) holds addresses from the `.so`; the game version is detected at boot by fingerprinting known functions, so supporting another version means adding data rather than rewriting logic.

| Versión / Version | Estado / Status |
| --- | --- |
| **4.5.2** (2016) | 🟢 Jugable / Playable |
| **6.6.1** (2018) | 🟡 En pruebas / In testing (Batallaz funcional / Battlez working) |
| **1.6.10** (2013) | 🟢 Jugable / Playable |

## Compilar / Building

Necesitas / You need: 64-bit Windows or Linux, CMake, a C++20 compiler (Visual Studio, MinGW-w64, GCC or Clang), OpenGL 2.0 and Boost headers in `third_party/boost_1_84_0`.

| | |
| --- | --- |
| Windows (Visual Studio + Ninja) | `compile-ninja.bat` → `out\build\x64-Release\pvz2native\pvz2.exe` |
| Windows (MinGW) | `compile.bat` → `build\pvz2native\pvz2.exe` |
| Linux | `./compile.sh` → `build/pvz2native/pvz2` (`--clean`, `--debug`, `--jobs N`, `--help`) |
| Linux AppImage | `./compile-appimage.sh` → `build/pvz2native/pvz2.AppImage` |

En Linux los archivos de la interfaz van en `assets/` junto al ejecutable; en Windows van dentro del `.exe`, y el AppImage ya trae los suyos.  
On Linux the UI assets go in `assets/` next to the binary; on Windows they're embedded in the `.exe`, and the AppImage bundles its own.

## Créditos / Credits
- **Fork author & maintainer:** [Onyxdum](https://github.com/Onyxdum)
- **Based on:** [ea192000/PvZ2Native](https://github.com/ea192000/PvZ2Native) and [OptiJuegos/PvZ2Native](https://github.com/OptiJuegos/PvZ2Native)
- **Libraries:** [dynarmic](https://github.com/lioncash/dynarmic), [SDL](https://github.com/libsdl-org/SDL), zlib, glad, [stb](https://github.com/nothings/stb)

## Licencia / License
El código usa licencia MIT ([LICENSE](LICENSE)). *Plants vs. Zombies 2* y todos sus recursos pertenecen a Electronic Arts / PopCap Games; este proyecto no distribuye ningún archivo del juego ni tiene relación con ellos.  
The code is MIT-licensed ([LICENSE](LICENSE)). *Plants vs. Zombies 2* and all its assets belong to Electronic Arts / PopCap Games; this project doesn't distribute any game files and isn't affiliated with them.
