# PvZ2Native WDRPC

📖 **Español** · [English](README.md)

**Plants vs. Zombies 2 en PC, y se juega como un juego de PC.** Es un fork de [ea192000/PvZ2Native](https://github.com/ea192000/PvZ2Native), que a su vez parte de [OptiJuegos/PvZ2Native](https://github.com/OptiJuegos/PvZ2Native). Esos proyectos ejecutan la `libPVZ2.so` real de Android de forma nativa en Windows y Linux. Este fork le añade lo que esperarías de una versión de escritorio: **Discord Rich Presence**, **guardado en la nube**, **Battlez contra gente real otra vez (en vivo o contra repeticiones)**, **lista de amigos**, **modo mando estilo consola con vibración**, **dificultades extra**, **integración con Steam** y **actualizador automático**. Un mismo ejecutable sirve para **v4.5.2** y **v6.6.1**.

> [!WARNING]
> Es un proyecto experimental y **no incluye el juego**. Necesitas tu propio `libPVZ2.so` y tu propio `.obb`, sacados legalmente de la versión de Android.

---

## 🌟 Qué añade este fork

### 🎮 Modo mando estilo consola
Se juega muy parecido a la versión de Xbox 360 del juego original:

| En un nivel | |
| --- | --- |
| Cruceta / stick izquierdo | Mover el selector de casillas por el jardín |
| `LB` / `RB` / stick derecho | Elegir sobre de semillas |
| `A` | Plantar en la casilla seleccionada |
| `X` / `Y` | Pala / Fertilizante en la casilla seleccionada |
| `B` | Quitar la semilla o el potenciador elegido |
| `Select` | Elegir un potenciador y luego `A` para usarlo en la casilla |
| `RT` | Plantas que se apuntan (Lanzaplátanos y similares): elegir y disparar |
| `LT` | Velocidad x2 |
| `Start` | Pausa |
| `L3` | Empezar la Última Resistencia cuando tengas la defensa lista |
| `R3` | Cambiar al cursor libre |

- `B`, `X`, `Y`, `L3` y `R3` siguen lo que pongas en **Ajustes → Mando**, así que puedes poner la pala, el fertilizante o un potenciador en el botón que quieras.
- El sol y las monedas se recogen en cuanto el selector pasa por encima.
- También funciona en los niveles con **cinta transportadora**: el stick derecho mueve el selector por la cinta.
- **Selector de semillas**: recorre las cartas y tu banco de semillas con el selector. `A` elige o devuelve una planta, `X` vuelve a poner tus últimas plantas, `RB` mejora, `LT` abre el Jardín Zen, `RT` enseña el jardín e `Y` empieza el nivel.
- **Mapa del mundo**: `B` atrás, `LB` Almanaque, `RB` Jardín Zen, `X` Diario de viaje, `Y` Tienda, `LT` Rompejarrones y `RT` el evento del momento.
- **Menú de ajustes**: cruceta para moverte, `A` para elegir y `B` para volver.
- Cada botón tiene su icono dibujado al lado de lo que hace.
- **Vibración** al plantar, en las oleadas enormes, explosiones, golpes de Zomboss, cortacéspedes, "¡Preparados, listos, a plantar!" y al ganar o perder.
- Si usas el ratón o el teclado, el modo mando se aparta. Vuelves a coger el mando y vuelve.

### 💬 Discord Rich Presence
Tu estado de Discord enseña lo que estás haciendo, en tu idioma (español, inglés, francés, alemán, italiano o portugués):

- El menú en el que estás (Almanaque, Tienda, Jardín Zen, Diario de viaje) y, dentro de un nivel, el mundo, si es de día o de noche y el icono de ese mundo.
- Modos como Rompejarrones, Fiesta Piñata, Misiones Épicas o Zonas Infinitas, con su nombre traducido.
- Una estrella (`⭐`) por cada objetivo del nivel, el nombre y la fase del Zombot en los jefes, y si estás eligiendo semillas, en pausa o recogiendo el premio.
- Un botón en tu perfil que enlaza a este repositorio.

Viene activado; solo necesitas tener Discord abierto:

```ini
[discord]
enabled = 1
client_id = 1560077802550853694
image = pvz2
button_url = https://github.com/Onyxdum/PvZ2NativeWDRPC
button_label = GitHub
```

### ☁️ Guardado en la nube
- Un botón de nube en el menú principal (también en ajustes) con cuatro opciones: **Nube**, tu propio servidor, **Google Drive** o **GitHub Gist**.
- Inicia sesión con Discord para usar la nube por defecto. También puede sincronizar sola al salir.
- Exporta o importa tu partida como archivo `.json`.
- Los resultados salen como una pequeña notificación arriba de la pantalla.

### 🥊 Vuelve Battlez (v6.6.1)
EA apagó los servidores de Battlez hace mucho. Este fork tiene su propio servidor, así que el modo vuelve a funcionar:

- **Partidas en vivo**: Battlez busca durante unos 15 segundos a otro jugador conectado. Si lo encuentra, empezáis el mismo nivel a la vez y veis cómo se mueve la puntuación del otro en el marcador de Battlez mientras jugáis.
- **Repeticiones si no hay nadie**: si no aparece nadie, el juego te pregunta si quieres jugar contra una partida grabada de otro jugador o cancelar. Tus propias partidas se convierten en repeticiones para los demás. Si todavía no hay ninguna, juegas contra la IA del juego.
- ¿Prefieres jugar solo contra repeticiones? Desactiva **Ajustes → Eventos → Batallaz en vivo (multijugador)**.
- **Tu identidad de Discord**: Battlez necesita la cuenta de Discord que usas para el guardado en la nube. En la clasificación y en los rivales salen los nombres y las fotos de perfil de Discord.
- **Torneos semanales** con las ligas, coronas y rachas originales, y sin límite de tamaño en la clasificación.
- **Premio para todos**: al acabar la semana, cada jugador que haya participado recibe el premio de su puesto (gemas, monedas) y los primeros suben de liga.
- **Clasificación global** en la web: [pvz2.onyxdum.eu/battlez](https://pvz2.onyxdum.eu/battlez).
- El servidor rechaza los resultados claramente falsos: partidas enviadas dos veces, partidas terminadas en un tiempo imposible y puntuaciones fuera de rango.
- Las dificultades extra nunca tocan los niveles de Battlez, así que los dos jugadores tienen siempre el mismo nivel.

### 👥 Amigos (v4.5.2 y v6.6.1)
- Abre la lista con el botón del sobre de la esquina de arriba a la izquierda. En v6.6.1 es un botón nativo del HUD que se esconde junto con las ventanas y los fundidos del propio juego.
- Ve el nombre y la foto de Discord de tus amigos, si están conectados, en qué mundo o nivel están y cuánto llevan de partida.
- Añade amigos con un enlace de invitación (`pvz2native://` o la página web de invitación). Pueden aceptar o rechazar, y para quitar a alguien bastan dos toques.
- Manda y recibe un regalo diario de monedas o gemas.

### 💀 Dificultades extra
En Ajustes → **Dificultad** puedes elegir cualquiera de los ocho niveles de dificultad dinámica del juego, o uno de estos:

- **Hard+**: siempre la dificultad dinámica más alta.
- **Hard++**: sin cortacéspedes, y cada oleada trae el triple de zombis repartidos por todas las filas.
- **Point Zero**: todo lo de Hard++, el cuádruple de zombis y además las pociones de la Edad Media en todos los jardines, que los zombis se beben al pasar.

### 🔄 Actualizador automático (Windows)
- Al arrancar el juego busca una versión nueva en segundo plano, sin retrasar el inicio.
- Si la hay, te dice qué versión es y qué trae, y te deja elegir: **Actualizar**, **Ahora no** u **Omitir** (omitir hace que no vuelva a preguntar por esa versión).
- **Actualizar** cierra el juego de forma normal, así que tu progreso y la sincronización con la nube se guardan. Luego descarga la actualización con barra de progreso y comprueba el archivo con el tamaño y el SHA-256 que publica GitHub.
- Hace copia de seguridad de cada archivo antes de cambiarlo. Si algo no se puede escribir (un archivo bloqueado, por ejemplo), lo deja todo como estaba. En cualquier caso, al final vuelve a abrir el juego.
- Nunca toca tus partidas ni los archivos del juego. `config.ini` y las reglas de presencia se fusionan en vez de sobrescribirse: conservas tus ajustes y solo se añaden las opciones nuevas.
- En Linux te avisa con una notificación cuando sale una versión nueva.
- Para desactivarlo, pon `[game] check_updates = 0`.

### 🚀 Integración con el escritorio
- **Steam**: la primera vez que lo abres te ofrece añadir el juego a tu biblioteca de Steam, con sus imágenes.
- **Asistente de primer arranque**: le indicas el APK y el OBB (4.5.2 o 6.6.1) y lo prepara todo.
- **Créditos finales**: al reclamar la última recompensa del juego salen los créditos.

---

## ✨ También incluye (build para PC)

Pensado para *Plants vs. Zombies 2: It's About Time* **v4.5.2** y **v6.6.1** en Windows y Linux.

### Aspecto e integración con el escritorio
- Icono del ejecutable y cursor propios (el cursor se adapta al tamaño de la ventana y la pantalla).
- Pantalla de inicio y progreso de carga, en lugar de un rato largo con la pantalla en negro.
- Datos de versión correctos en el ejecutable de Windows.

### Ajustes dentro del juego
- Se abren con `Esc` o con el botón de **Google Play** de la esquina inferior derecha del menú principal.
- Cambia el idioma, la moneda, la pantalla completa, el monitor, el límite de FPS, la dificultad, las compras gratis, los eventos, los atajos de teclado y las opciones del mando sin tocar ningún archivo.
- Casi todo se aplica al momento (incluso mover la ventana a otro monitor). El idioma, las compras gratis y las opciones de Eventos reinician el juego para que el motor las cargue.
- Los `config.ini` antiguos o incompletos se arreglan al arrancar: las claves que faltan reciben valores seguros y los valores incorrectos se corrigen.

### Eventos
- Ajustes → **Eventos**: **Activar eventos** (`[runtime] use_cdn`) enciende o apaga el CDN emulado que trae los eventos y el contenido diario. **Forzar eventos locales** (`[runtime] rewrite_cdn`) vuelve a copiar los archivos de eventos desde el OBB en cada arranque. Cambiar cualquiera de los dos reinicia el juego.

### Logros
- Logros locales que se desbloquean mientras juegas y se guardan en `prefs.dat`.
- Una notificación animada (con sonido) al desbloquear uno. La lista completa está en ajustes (**Ver logros**).
- Puedes quitar la notificación y el sonido desde ajustes o con `[game] achievement_notifications = 0`. Los logros se siguen guardando.

### Confirmación al salir
- Confirma o cancela antes de salir, con ratón, teclado o mando.
- `Esc` abre los ajustes, y volver a pulsarlo ahí abre este diálogo (siempre `Esc`, no se puede cambiar). `F11` siempre cambia a pantalla completa y vuelve.

### Configuración
- En el primer arranque detecta tu idioma, tu moneda y otros valores relacionados.
- La ventana de consola/log está oculta por defecto (`[log] console = 0`).
- Las opciones del día a día están en la pantalla de ajustes; las avanzadas siguen en `config.ini`.
- Casi todas las opciones se pueden cambiar desde la línea de comandos, por ejemplo `--fullscreen=0`, `--video.fullscreen=0`, `--console=1`.

### Teclado y ratón
- Atajos configurables que tocan la pantalla por ti (la interfaz se hizo para pantallas táctiles).
- Por defecto: `1`–`8` para las casillas de plantas, `Espacio` para la velocidad, `Enter` para pausar, `F` para el fertilizante, `S` para la pala, `Q`/`W`/`E` para los poderes, `R` para repetir y `Retroceso` para volver, además de teclas para el Almanaque, conectar, el Rompejarrones y el Jardín Zen.
- Los atajos se cambian desde la pantalla de ajustes.
- La rueda del ratón desplaza las listas (simulando un arrastre), el clic derecho vuelve atrás y el clic central abre el menú donde lo hay.
- Los clics caen en su sitio en pantalla completa y después de cambiar el tamaño, y la entrada se recupera si alguna vez se queda atascada.

### Mando (menús)
- Fuera de los niveles el stick izquierdo mueve el cursor y **A** hace clic; la cruceta salta entre los botones de la pantalla.
- Acciones reasignables, controles de velocidad del stick y zona muerta, y puedes elegir qué mando usar (o ninguno).
- Ratón, teclado y mando funcionan a la vez.

### Otros
- Los campos de texto funcionan mejor (haz clic fuera para dejar de escribir).
- Ritmo de fotogramas más estable y menos texto en la consola con el log apagado.
- Los enlaces de Ayuda / Licencia / Privacidad se abren en tu navegador.
- Un ID de jugador que no cambia en cada instalación.

---

## 🧠 Cómo funciona

**Ejecuta en PC la librería nativa real de *Plants vs. Zombies 2* para Android (`libPVZ2.so`, ARM32).** Lo único que se emula es la **CPU**: PvZ2 solo se compiló para ARM (no existe versión x86/x64), así que un JIT traduce sus instrucciones ARM a x86_64. Todo lo demás (Android, JNI, libc, OpenGL ES, el sistema de archivos) **no se emula, se reimplementa de forma nativa**, la misma idea que **Wine** con los programas de Windows.

## 📖 Qué es (y qué no es)

No es un *port* del código fuente del juego. Mezcla **dos técnicas**, y la diferencia importa:

- **Solo se emula la CPU.** El juego es código máquina ARM y PvZ2 nunca se compiló para x86/x64, así que un procesador de PC no puede ejecutarlo directamente. Un JIT (dynarmic) lee las instrucciones ARM y las traduce a x86_64 sobre la marcha. Es la única parte emulada.
- **Android no se emula, se reimplementa.** Cuando el juego llama a una función de Android (abrir un archivo, dibujar con OpenGL ES, reservar memoria, llamar a Java…), la responde **código nativo de PC** que hace el trabajo real con la API de escritorio equivalente. Debajo no hay un Android virtual, solo **implementaciones nuevas** de las funciones que el juego necesita.

Es el mismo enfoque que **Wine** (*"Wine Is Not an Emulator"*), llevado un paso más allá. Wine reimplementa las llamadas de Windows sin emular nada porque los `.exe` ya son x86. Aquí el juego es ARM, así que también hay que emular la CPU. Por lo demás, la idea es la misma: **traducir las llamadas en vez de simular una máquina entera.**

```text
┌──────────────────────────────────────────────┐
│  libPVZ2.so  (ELF ARM32, EABI5, sin tocar)   │   ← el juego real de Android
└───────────────────────┬──────────────────────┘
                        │  instrucciones ARM
                        ▼
┌──────────────────────────────────────────────┐
│  dynarmic:  JIT ARM32 → x86_64               │   ← la ÚNICA parte emulada
└───────────────────────┬──────────────────────┘     (solo la CPU)
                        │  llamadas "de Android"
                        ▼
┌──────────────────────────────────────────────┐
│  Capa de compatibilidad (este proyecto)      │   ← reimplementación NATIVA,
│                                              │     estilo Wine (nada emulado)
│  JNI / JavaVM falsos ·   libc / libm / libz  │
│  OpenGL ES → OpenGL  ·   OpenSL ES → SDL     │
│  GuestHeap propio    ·   VFS (rutas + .obb)  │
└───────────────────────┬──────────────────────┘
                        │
                        ▼
┌──────────────────────────────────────────────┐
│  Windows / Linux x64  ·  SDL2  ·  OpenGL 2.0 │
└──────────────────────────────────────────────┘
```

---

## 🧩 Arquitectura

El código está en [pvz2native/](pvz2native/), dividido en capas con una regla estricta: **solo una capa puede contener direcciones del `.so`.** Por eso dar soporte a otra versión del juego consiste en añadir datos, no en reescribir lógica.

| Capa | De qué se encarga | ¿Puede tener direcciones del `.so`? |
| --- | --- | :---: |
| [runtime/](pvz2native/src/runtime/) | Ejecutar código ARM (JIT, heap, hilos, sincronización) | **Nunca** |
| [game/symbols.cpp](pvz2native/src/game/symbols.cpp) | Dónde está cada cosa en *este* binario | **Solo esta** |
| [engine/](pvz2native/src/engine/) | Qué llamar y en qué orden (arranque, ciclo de vida, fotograma) | Solo mediante `sym()` |
| [dependencies/](pvz2native/src/dependencies/) | Responder a cada `.so` de Android (libc, libm, GLES, …) | No |
| [dex/](pvz2native/src/dex/) | Responder a la parte Java (JNIEnv falso + hooks por clase) | No |
| [diagnostics/](pvz2native/src/diagnostics/) | Inspeccionar el juego invitado (watchpoints, muestreo de PC) | Solo mediante `sym()` |

Algunas piezas principales:

- **JIT:** [dynarmic](dynarmic/) (solo el frontend A32). Un JIT por cada hilo del juego, con una *page table* identidad sobre la memoria del `.so` para ir más rápido.
- **Dependencias nativas:** un módulo por cada `.so` de Android que usa el juego. libc está dividida por cabecera (`string`, `stdio`, `stdlib`, `time`, `pthread`, `locale`, `ctype`, …), y además están `libm`, `libz`, `liblog`, `libgles`, `libstdcxx`, `libopensles` y `libdl`. Cada función es un `void(GuestCall&)` registrado por nombre.
- **Parte Java:** [jni_env.cpp](pvz2native/src/dex/jni_env.cpp) implementa un `JNIEnv`/`JavaVM` falso (objetos, cadenas, arrays, referencias), y [dex/hooks/](pvz2native/src/dex/hooks/) tiene un archivo por clase Java (`AndroidGameApp`, `AndroidSurfaceView`, HTTP, Google Play, Facebook…).
- **Gráficos:** las llamadas GLES del juego se traducen a **OpenGL 2.0 (perfil de compatibilidad)** con `glad`. SDL da la ventana y el contexto GL, haciendo el papel del `GLSurfaceView` de Android.
- **Recursos:** un VFS traduce las rutas de Android a rutas de PC y lee los recursos directamente del `.obb` real del juego (formato RSB/RSG).

---

## 🎮 Versiones del juego soportadas

La versión se reconoce por una **huella de bytes** de dos funciones nativas conocidas. Si no coincide ninguna, se niega a arrancar en lugar de ejecutar a ciegas.

| Versión | OBB habitual | Estado |
| --- | --- | --- |
| **1.6.10** (2013) | `main.7.com.ea.game.pvz2_na.obb` | 🟢 Jugable |
| **4.5.2** (2016) | `main.147.com.ea.game.pvz2_row.obb` (a menudo distribuido o renombrado como `data.obb`) | 🟢 Jugable |
| **6.6.1** (2018) | `main.434.com.ea.game.pvz2_row.obb` (o `data.obb` junto a `libNimble.so`) | 🟡 En pruebas (Aventura, niveles y Battlez jugables; Arena todavía no) |

Un **único ejecutable** sirve para todas las versiones. Al arrancar, [symbols.cpp](pvz2native/src/game/symbols.cpp) reconoce la versión por las huellas de funciones del ELF y carga los parches, las direcciones de símbolos, las librerías que necesita (`libNimble.so`) y las reglas de Discord Rich Presence que tocan. El soporte de 6.6.1 sigue en pruebas.

---

## ⚙️ Requisitos

- **Windows o Linux de 64 bits** (el ejecutable es x86_64 estático; no hay versión de 32 bits porque dynarmic solo tiene backends para x86_64/arm64/riscv64).
- **CMake** (recomendado 3.5 o más nuevo).
- **Windows:** MinGW-w64 (GCC con C++20) **o** Visual Studio con C++ / CMake / Ninja (`compile-ninja.bat`).
- **Linux:** GCC o Clang con C++20.
- Una **GPU con OpenGL 2.0** o superior.
- Tu propio **`libPVZ2.so`** (armeabi-v7a) y el **`.obb`** que le corresponde.
- **Cabeceras de Boost** en `third_party/boost_1_84_0` (los scripts de compilación las buscan ahí).

---

## 🔨 Compilación

### Windows: Visual Studio + Ninja (recomendado)

```bat
compile-ninja.bat
```

Busca Visual Studio (cualquier edición) con `vswhere` y compila un binario Release x64 con Ninja. Resultado:

```text
out\build\x64-Release\pvz2native\pvz2.exe
```

### Windows: MinGW

Si tienes `gcc`, `g++` y `make` en el `PATH`:

```bat
compile.bat
```

Resultado:

```text
build\pvz2native\pvz2.exe
```

### Linux

```bash
./compile.sh
```

Opciones útiles: `--clean`, `--debug`, `--jobs N`, `--no-assets`, `--verbose`, `--help`.

Resultado:

```text
build/pvz2native/pvz2
```

`compile.sh` también copia los recursos de la interfaz a `build/pvz2native/assets/` (pantalla de inicio, imágenes de ajustes, cursor, logros). En Windows van dentro del `.exe` mediante `windows_resources.rc`; en Linux tienen que estar junto al binario, en `assets/`.

### Linux: AppImage

```bash
./compile-appimage.sh
```

Compila con `compile.sh` y luego empaqueta un AppImage tipo 2. Resultado:

```text
build/pvz2native/pvz2.AppImage
```

El AppImage contiene **solo** el binario `pvz2` y `assets/`. Pon `libPVZ2.so`, `data.obb` y `config.ini` **junto al** `.AppImage` (la misma estructura que con el binario suelto). La primera vez puede descargar `appimagetool` en `tools/` si no lo tienes.

---

## ▶️ Ejecución

Pon los archivos del juego de Android **junto al ejecutable** (no en una subcarpeta `lib/`). Por defecto busca `libPVZ2.so` y `data.obb` en esa misma carpeta:

```text
<carpeta con pvz2 / pvz2.exe / pvz2.AppImage>/
├── pvz2(.exe)  o  pvz2.AppImage
├── config.ini                 ← se crea / repara en el primer arranque
├── libPVZ2.so                 ← obligatorio (armeabi-v7a)
├── data.obb                   ← obligatorio (o pon el nombre de tu OBB en [paths] obb)
└── assets/                    ← binario suelto de Linux: interfaz/inicio/cursor/logros
                                   (Windows: dentro del exe; AppImage: dentro de la imagen)
```

Puedes dejar el OBB con su nombre original e indicarlo en `config.ini`:

```ini
[paths]
so  = libPVZ2.so
obb = main.147.com.ea.game.pvz2_row.obb
```

Las rutas relativas se cuentan desde la carpeta del ejecutable. También valen rutas absolutas.

**Asistente de primer arranque:** si al arrancar faltan `libPVZ2.so` o `data.obb`, se abre un asistente. Acepta APKs de **v4.5.2** y **v6.6.1** (o archivos XAPK / ZIP) y OBBs. Extrae `libPVZ2.so` y lo demás que haga falta (`libNimble.so` en 6.6.1), copia el OBB, detecta la variante del paquete (`pvz2_row` o `pvz2_na`) y la versión del juego, escribe `config.ini` y arranca el juego. Si los archivos ya están, el juego arranca directamente.

En el primer arranque el juego escribe un `config.ini` con comentarios. La pantalla de ajustes cubre las opciones del día a día; las avanzadas siguen en el archivo y en la línea de comandos.

### 🍎 macOS (mediante Whisky / Wine)
No hay versión nativa para macOS: Apple ha dejado OpenGL obsoleto y limita los compiladores JIT en Apple Silicon. Aun así puedes jugar con **[Whisky](https://getwhisky.app/)** (gratis y de código abierto) o **CrossOver**:
1. Instala **Whisky**.
2. Crea una "bottle" de Windows 10.
3. Abre `pvz2.exe` de la versión para Windows.

---

## 🔧 Configuración (`config.ini`)

El archivo de configuración principal, para rutas, pantalla, controles y opciones del juego:

| Sección | Clave | Qué hace |
| --- | --- | --- |
| `[paths]` | `so`, `obb` | Rutas a `libPVZ2.so` y `data.obb` (relativas al ejecutable, o absolutas). |
| `[paths]` | `save` | Carpeta de guardado opcional; si no se indica, `prefs.dat` va junto al ejecutable. |
| `[log]` | `console` | Mostrar la consola/log (`1` la muestra, `0` la oculta). |
| `[log]` | `verbose`, `trace`, `input` | Logs detallados del ciclo de vida del motor, las importaciones y la entrada (necesita `console=1` o `logfile`). |
| `[video]` | `fullscreen`, `display`, `width`, `height`, `fps_limit`, `lock_cursor` | Pantalla y monitor (también en ajustes). |
| `[game]` | `user_locale`, `currency_*`, `emulate_iap` | Idioma/región, moneda de la tienda y compras gratis (también en ajustes). |
| `[game]` | `achievement_notifications` | Notificación y sonido de logros (`1` por defecto; `0` solo los silencia). |
| `[game]` | `check_updates` | Buscar versiones nuevas al arrancar (`1` por defecto). |
| `[runtime]` | `use_cdn`, `rewrite_cdn` | CDN emulado encendido/apagado (encendido por defecto) y volver a copiarlo desde el OBB (también en ajustes → Eventos). |
| `[runtime]` | `allow_repeat_quests` | `1` (por defecto): las misiones repetibles que ya reclamaste vuelven a contar muertes en vez de completarse solas cuando reaparecen (al arrancar se limpia la lista de misiones terminadas de la partida). `0` deja la partida como está. |
| `[shortcuts]` | `bind_*` | Teclado → toque en pantalla (también en ajustes). |
| `[board]` / `[gamepad]` / `[gamepad_binds]` | … | Rejilla de foco y botones del mando (también en ajustes). |
| `[discord]` | `enabled`, `client_id`, `image`, `rules` | Discord Rich Presence (activado por defecto). |

---

## ⌨️ Controles (host)

| Acción | Control |
| --- | --- |
| Tocar / seleccionar | Clic izquierdo (o **A** en el mando) |
| Arrastrar (deslizar) | Clic izquierdo + mover (o stick derecho) |
| Desplazar listas | Rueda del ratón |
| Atrás (menús) | Clic derecho / `Retroceso` / Atrás en el mando (igual que Esc: abre ajustes y luego el diálogo de salida) |
| Ajustes → diálogo de salida | `Esc` (fijo): la primera pulsación abre ajustes, la segunda el diálogo de salida |
| Pantalla completa | `F11` (fijo) |
| Ajustes | Botón de Google Play del menú principal |
| Logros | Desde ajustes (**Ver logros**) |
| Atajos / mando | Se configuran en ajustes |
| Mando en los niveles | Ver [Modo mando estilo consola](#-modo-mando-estilo-consola) |

---

## 📜 Licencia

El código está bajo **licencia MIT** (ver [LICENSE](LICENSE)). Usa proyectos de terceros (dynarmic, SDL, zlib, glad, stb), cada uno con su propia licencia.

**Plants vs. Zombies 2**, sus marcas y todos sus recursos pertenecen a **Electronic Arts / PopCap Games**. Este proyecto **no distribuye ni sustituye** ningún archivo del juego: necesitas los tuyos, obtenidos legalmente.

## ⚠️ Descargo de responsabilidad

Es un proyecto independiente, **sin relación, respaldo ni patrocinio** de Electronic Arts, PopCap Games ni nadie vinculado a Plants vs. Zombies. Su objetivo es únicamente **técnico y educativo**: investigar la emulación de binarios ARM, las capas de compatibilidad y la traducción de APIs.

---

<p align="center">
  <b>PvZ2Native</b>: la <code>libPVZ2.so</code> de Android funcionando en PC mediante emulación ARM
</p>
