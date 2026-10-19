<p align="center"><img src="assets/ps4mount-ps4-512.png" width="180" alt="PS4Mount PS4"></p>

# PS4Mount PS4

**Juega pkgs fake de PS4 desde un USB o desde la memoria de la consola, sin instalarlos.**
**Play PS4 fake pkgs from a USB drive or from the console's storage, without installing them.**

**[Español](#español) · [English](#english)**

PS4Mount PS4 es un port, exclusivo para PS4, de [PS4Mount](https://github.com/gasb07/ps4mount) de **gasb07** (para PS5).
PS4Mount PS4 is a PS4-only port of [PS4Mount](https://github.com/gasb07/ps4mount) by **gasb07** (for the PS5).

*Proyecto no oficial, sin relación con Sony Interactive Entertainment. PlayStation y PS4 son marcas de Sony Interactive Entertainment. Úsalo solo con juegos que te pertenecen.*
*Unofficial project, not affiliated with Sony Interactive Entertainment. PlayStation and PS4 are trademarks of Sony Interactive Entertainment. Only use it with games you own.*

---

<a name="español"></a>
# Español

PS4Mount es un payload para una PS4 con jailbreak. Pones los pkgs del juego (y su actualización y sus DLC) en un USB o en la memoria de la consola, y PS4Mount los añade a la pantalla de inicio para jugarlos **desde el pkg, sin instalarlos**: no hay que esperar a una instalación ni gastar espacio en una segunda copia.

Además tiene un **panel web** (desde el móvil, el PC o el navegador de la PS4) con tu biblioteca, portadas y fichas de cada juego, y **envío de juegos desde el PC por la red local**: cada pkg se comprueba antes de enviarlo, para no pasar horas enviando un juego que la consola no puede usar.

Probado en una PS4 con firmware **13.52** y GoldHEN. El panel y las notificaciones están en **español e inglés**: salen en el idioma de la consola (inglés si no es ninguno de los dos), y se puede cambiar en **Ajustes → Idioma**.

> [!WARNING]
> **Versión beta.** Haz una copia de seguridad de tus partidas guardadas antes de usarlo, y [cuéntanos cualquier problema](#problemas-frecuentes) con el log.

![El panel de PS4Mount](docs/panel-main-es.png)

## Qué funciona y qué no

| | Estado |
| --- | --- |
| Juegos desde un **USB exFAT** (también pkgs de más de 4 GB) | ✅ probado |
| Juegos desde la **memoria interna** (`/data/PS4Mount`) | ✅ probado |
| **Actualizaciones** | ✅ probado |
| **DLC** de hasta 256 MB (casi todos: trajes, packs, licencias) | ✅ probado: PS4Mount los instala con el instalador del sistema en segundos, y aparecen en la información del juego |
| DLC de más de 256 MB | ⚠️ se montan sin instalar, pero el sistema puede no mostrarlos |
| **Enviar juegos desde el PC** por la red, con comprobación previa | ✅ probado (unos 16 MB/s por cable) |
| **Borrar** un juego desde la consola | ✅ probado: borra también sus pkgs y libera el espacio |
| Un juego **nuevo**, o una actualización que llega después del juego | ⚠️ necesita **reiniciar la consola una vez**; después se juega siempre |
| Otros firmwares | ⚠️ sin probar todavía |
| Pkgs **retail** (no fake) | ❌ |
| Apps homebrew (tiendas, emuladores…) | ❌ instálalas de la forma normal; pesan poco |

## Requisitos

- Una PS4 con jailbreak y **GoldHEN** (con su BinLoader activado, el puerto 9090).
- **Payload Guest** u otra forma de cargar payloads.
- Pkgs **fake** de PS4 (`CUSA…`).
- Para jugar desde un USB: un disco formateado en **exFAT**.

## Uso

1. **Descarga** `ps4mount-ps4.bin` de [Releases](https://github.com/Vld439/PS4Mount-PS4/releases) y cópialo en **`/data/payloads/`** de la consola (por FTP, o como copies tus otros payloads).
2. **Después de cada arranque**, abre **Payload Guest** y elige PS4Mount. Aparece la notificación `PS4Mount 0.1.0-beta en marcha. Panel en http://<IP de la PS4>:8770`.
3. **Pon los juegos**, de una de estas formas:
   - **En un USB:** copia los pkgs en la carpeta **`PS4Mount`** del disco y conéctalo a la PS4. PS4Mount la crea la primera vez que ve el disco, o puedes crearla tú; solo usa lo que pongas ahí, el resto del disco no lo toca. Dentro puedes organizarlos en carpetas, hasta dos niveles: `PS4Mount/Overcooked/Game/`, `…/Update/`, `…/DLC/`.
   - **Desde el PC:** con **Enviar juegos** en el panel (ver más abajo).
   - **En la memoria interna:** pkgs en `/data/PS4Mount` (por FTP o con Enviar juegos). `/data/pkg` sigue siendo del instalador normal: PS4Mount no lo toca.
4. Espera las notificaciones: `usb0: 1 juego(s) de PS4 encontrado(s), 1 nuevo(s) o actualizado(s)` → `Añadiendo 1/1: <juego>` → `<juego> añadido. Reinicia la consola una vez para jugarlo`.
5. **Reinicia la consola una vez**, vuelve a cargar PS4Mount y juega desde la pantalla de inicio.

PS4Mount ordena los pkgs que usa en una carpeta por juego, en el mismo disco (es un cambio de nombre, sin copiar nada):

```
ps4/CUSAxxxxx/app.pkg                           juego
ps4/CUSAxxxxx/patch.pkg                         actualización
ps4/CUSAxxxxx/addcont/CUSAyyyyy/<LABEL>/ac.pkg  DLC
```

Mantén esa estructura. Para añadir más juegos, vuelve a ponerlos en la carpeta de búsqueda.

### Por qué hay que reiniciar una vez

La consola solo inicia un juego cuando su servicio de descargas (BGFT) conoce la tarea del juego, y ese servicio solo las lee al arrancar. PS4Mount escribe la tarea al añadir el juego, así que el primer arranque del juego necesita un reinicio. Hemos probado a cerrar la sesión y a terminar una instalación real, y ninguna de las dos lo evita. Los DLC no necesitan reinicio.

### El panel

Abre `http://<IP de la PS4>:8770` en el navegador de un móvil o un PC de la **misma red**, o en el navegador de la propia PS4 (que recibe una versión más sencilla).

| Estado | Significa |
| --- | --- |
| Listo | En la biblioteca, listo para jugar |
| Reinicia para jugar | Añadido en este arranque: reinicia una vez |
| Abierto | El juego está abierto |
| Añadiéndose | Se está añadiendo |
| Fuera de la biblioteca | Quitado de la biblioteca; sus pkgs siguen en el disco |
| Unidad no conectada | Su disco no está conectado: conéctalo para jugar |
| Instalado en la consola | Hay una instalación normal de ese juego: PS4Mount no lo toca |
| En otra unidad | El mismo juego está en otro disco conectado, que es el que se usa |

Cada juego tiene una ficha (pulsa su portada) con su versión, el firmware que necesita, su tamaño y dónde está.

![Ficha de un juego](docs/panel-game-es.png)

Botones: **Quitar de la biblioteca** (lo quita de la consola y conserva sus pkgs), **Añadir a la biblioteca**, **Buscar juegos**, **Enviar juegos**, **Ajustes**, **Log** y **Detener**.

En la ficha del juego también está **Restaurar archivos originales**: deshace lo que PS4Mount hizo con sus archivos. Quita el juego de la consola y vuelve a dejar sus pkgs donde estaban, con el nombre que tenían (por ejemplo, `ps4/CUSA00092/app.pkg` vuelve a ser `PS4Mount/Flower/Flower.pkg`). Sirve para recuperar tus archivos tal como los tenías, por ejemplo para instalar el juego de la forma normal. No borra nada, y PS4Mount no vuelve a tomar esos pkgs.

### Enviar juegos desde el PC

En el panel, **Enviar juegos**: arrastra los pkgs y elige el destino (el USB o la memoria interna).

- Cada pkg se **comprueba antes de enviarse**, en un segundo, con sus primeros 4 MB: si es un pkg de PS4 fake, si es juego, actualización o DLC, si tu firmware basta, si está completo y si cabe. Solo se envían los que la consola puede usar.
- Se envían primero los juegos, luego las actualizaciones y al final los DLC.
- Si cortas o falla un envío, no queda ningún archivo a medias, y puedes pulsar **Enviar de nuevo**.
- Al llegar, el juego se añade solo.

![Enviar juegos](docs/panel-send-es.png)

### Ajustes

| Ajuste | Por defecto | Significa |
| --- | --- | --- |
| Carpetas donde buscar pkgs | `/PS4Mount` | Carpetas de cada disco donde se buscan pkgs nuevos, una por línea (`/` es la raíz: todos los pkgs de juegos de la raíz se moverían a `ps4/`). Se miran hasta dos niveles de subcarpetas. `ps4` no se puede usar. |
| Añadir los juegos nuevos por grupos / Juegos por grupo | Sí / 5 | Añade muchos juegos por grupos, para no saturar la consola. |
| Instalar DLC | Sí | Instala los DLC (ver arriba). |
| Usar también los pkgs de /data/PS4Mount | Sí | Usa también los pkgs de la memoria interna. |
| Idioma | El de la consola | El idioma del panel y de las notificaciones: el de la consola, inglés o español. |

Se guardan en `/data/ps4mount/config.txt`. Los ajustes también tienen **Restaurar los archivos de todos los juegos** (o de los elegidos) y **Volver a usar los pkgs restaurados**.

### Quitar y borrar juegos

- **Borrar desde la consola** (Options → Eliminar, o Ajustes → Almacenamiento): borra el juego **y sus pkgs** (juego, actualización y DLC), y libera el espacio, como con un juego instalado. Solo cuando confirmas el borrado en esas pantallas, y solo la carpeta de ese juego.
- **Quitar de la biblioteca** en el panel: quita el juego de la consola y **conserva sus pkgs**, para volver a añadirlo con **Añadir a la biblioteca**.

Mientras están abiertas las pantallas de borrado, PS4Mount desmonta las carpetas de los juegos, así que el borrado del sistema nunca llega a los pkgs: los borra PS4Mount, y solo la carpeta de ese juego.

### Bueno saber

- **Carga PS4Mount después de cada arranque.** Sin él, los juegos no pueden iniciarse (siguen en la pantalla de inicio).
- **Cierra los juegos de PS4Mount antes de poner la consola en reposo**: el reposo apaga el USB.
- Si **desconectas el USB**, sus juegos se quedan en la pantalla de inicio y el panel los muestra como *Unidad no conectada*. Al reconectarlo, espera la notificación `usb0: … encontrado(s)` antes de abrirlos.
- Si GoldHEN carga PS4Mount automáticamente y luego lo cargas tú, verás `PS4Mount … ya está en marcha`: es normal, el segundo se cierra solo.
- El panel no tiene contraseña: cualquiera en tu red local puede abrirlo. No expongas el puerto 8770 a Internet.

### Problemas frecuentes

| Problema | Qué hacer |
| --- | --- |
| CE-32930-7 al abrir un juego recién añadido | Reinicia la consola una vez y vuelve a cargar PS4Mount. |
| "No se puede iniciar la aplicación" | Comprueba que PS4Mount está en marcha y que el disco del juego está conectado (PS4Mount avisa si no lo está). |
| El panel no abre | Usa la IP de la **consola**, con `http://` delante y `:8770`, desde la misma red. |
| Un pkg no se añade | Mira el log (panel → **Log**): dice por qué (no es fake, necesita otro firmware, está incompleto…). |
| El panel o las notificaciones salen en otro idioma | Cámbialo en **Ajustes → Idioma**. |
| Un juego falla o pierde partidas también instalado de forma normal | Es el pkg (o su backport), no PS4Mount: prueba otro dump. |

Para informar de un problema, abre un [issue](https://github.com/Vld439/PS4Mount-PS4/issues) con la versión de PS4Mount, el firmware, el juego (`CUSA…`), qué pasó y el log (panel → **Log**, o `/data/ps4mount/log.txt` por FTP). No adjuntes pkgs ni enlaces a ellos.

### Ficheros en la consola

| Fichero | Para qué |
| --- | --- |
| `/data/ps4mount/log.txt` | El log |
| `/data/ps4mount/config.txt` | Los ajustes |
| `/data/ps4mount/stop` | Vacío: para PS4Mount |
| `/data/ps4mount/klog_all` | Vacío: copia todo el log del sistema en el log de PS4Mount (diagnóstico) |
| `/system_data/ps4mount/` | Las carpetas internas de los juegos |

## Compilar

```
bash test/run.sh        # tests en el ordenador (SANITIZE=1 para ASan/UBSan)
bash test/policy.sh     # reglas del proyecto
cd test/web && npm test # el panel
PS4_PAYLOAD_SDK=/ruta/al/sdk bash build.sh   # build/ps4mount-ps4.elf y .bin
```

Necesitas el [ps4-payload-sdk](https://github.com/ps4-payload-dev/sdk) **compilado desde git**: la release v0.9 no arranca en 13.52 (su runtime solo conoce hasta 13.50). La CI usa el commit indicado en `SDK_COMMIT` de `.github/workflows/build.yml`. [AI-WORKFLOW.md](AI-WORKFLOW.md) explica cómo trabajar en PS4Mount con un asistente de IA.

## Créditos

- **[PS4Mount](https://github.com/gasb07/ps4mount)**, de **gasb07**: el proyecto del que nace este port (la lectura de pkgs, los ficheros de instalación, el montaje, el panel). Su historial está en el proyecto original y en el historial de git de este repositorio.
- **[LibOrbisPkg](https://github.com/maxton/LibOrbisPkg)** (maxton, LGPL-3.0): la clave de los pkgs fake (`src/keys.h`) y la referencia del formato.
- **[ps4-payload-sdk](https://github.com/ps4-payload-dev/sdk)** (John Törnblom, GPL-3.0-or-later): el toolchain y el runtime del payload.
- **flatz**: la interfaz de BGFT documentada en [ps4_stub_lib_maker_v2](https://github.com/flatz/ps4_stub_lib_maker_v2), y **[ps4-ipi](https://github.com/0x199/ps4-ipi)** como referencia de su uso, para instalar los DLC.
- **[SQLite](https://sqlite.org)** (dominio público): la lectura de la base de datos de la consola.
- **[ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus)** (drakmor): inspiración del original para montar juegos desde USB.

## Licencia

GPL-3.0-or-later (ver [LICENSE](LICENSE)). Si publicas tu propia versión, conserva los avisos, distribuye los cambios con la misma licencia y, por favor, menciona a gasb07 y enlaza el proyecto original.

---

<a name="english"></a>
# English

PS4Mount is a payload for a jailbroken PS4. Put the game pkgs (with their update and add-ons) on a USB drive or in the console's storage, and PS4Mount adds them to the home screen so they are played **from the pkg, without installing them**: no install to wait for, no space spent on a second copy.

It also has a **web panel** (from a phone, a computer or the PS4's browser) with your library, artwork and a page per game, and **sending games from a computer over the local network**: every pkg is checked before it is sent, so no one spends hours sending a game the console cannot use.

Tested on a PS4 on firmware **13.52** with GoldHEN. The panel and the notifications are in **English and Spanish**: they follow the console's language (English for any other), and **Settings → Language** changes it.

> [!WARNING]
> **Beta version.** Back up your saved data before using it, and [tell us about any problem](#troubleshooting) with the log.

![The PS4Mount panel](docs/panel-main.png)

## What works and what does not

| | State |
| --- | --- |
| Games from an **exFAT USB drive** (pkgs over 4 GB too) | ✅ tested |
| Games from the **console's storage** (`/data/PS4Mount`) | ✅ tested |
| **Updates** | ✅ tested |
| **Add-ons (DLC)** up to 256 MB (almost all: costumes, packs, licenses) | ✅ tested: PS4Mount installs them with the system's installer in seconds, and they show in the game's information |
| Add-ons over 256 MB | ⚠️ mounted without installing, but the system may not show them |
| **Sending games from a computer** over the network, checked first | ✅ tested (about 16 MB/s over a cable) |
| **Deleting** a game on the console | ✅ tested: its pkgs are deleted too and the space is freed |
| A **new** game, or an update that arrives after its game | ⚠️ needs **one console restart**; it plays every time after that |
| Other firmwares | ⚠️ not tested yet |
| **Retail** (not fake) pkgs | ❌ |
| Homebrew apps (stores, emulators…) | ❌ install them the usual way; they are small |

## Requirements

- A jailbroken PS4 with **GoldHEN** (its BinLoader on, port 9090).
- **Payload Guest** or another way to load payloads.
- PS4 **fake** pkgs (`CUSA…`).
- To play from USB: a drive formatted as **exFAT**.

## Use

1. **Download** `ps4mount-ps4.bin` from [Releases](https://github.com/Vld439/PS4Mount-PS4/releases) and copy it to **`/data/payloads/`** on the console (over FTP, or however you copy your other payloads).
2. **After every boot**, open **Payload Guest** and pick PS4Mount. The notification `PS4Mount 0.1.0-beta running. Panel at http://<PS4 IP>:8770` shows up.
3. **Add games**, one of these ways:
   - **On a USB drive:** copy the pkgs to the **`PS4Mount`** folder of the drive and plug it into the PS4. PS4Mount creates the folder the first time it sees the drive, or you create it; only what you put there is used, the rest of the drive is left alone. Inside, they can be in folders, two levels down: `PS4Mount/Overcooked/Game/`, `…/Update/`, `…/DLC/`.
   - **From a computer:** with **Send games** in the panel (see below).
   - **In the console's storage:** pkgs in `/data/PS4Mount` (over FTP or with Send games). `/data/pkg` stays the usual installer's: PS4Mount leaves it alone.
4. Wait for the notifications: `usb0: 1 PS4 game(s) found, 1 new or updated` → `Adding 1/1: <game>` → `<game> added. Restart the console once to play it`.
5. **Restart the console once**, load PS4Mount again and play from the home screen.

PS4Mount keeps the pkgs it uses in a folder per game on the same drive (a rename, nothing is copied):

```
ps4/CUSAxxxxx/app.pkg                           game
ps4/CUSAxxxxx/patch.pkg                         update
ps4/CUSAxxxxx/addcont/CUSAyyyyy/<LABEL>/ac.pkg  add-on
```

Keep this layout. To add more games, put them in the scan folder again.

### Why one restart

The console only starts a game when its background transfer service (BGFT) knows the game's task, and the service reads its tasks at boot only. PS4Mount writes the task when it adds the game, so the game's first start needs a restart. Logging out and finishing a real install were both tried: neither avoids it. Add-ons need no restart.

### The panel

Open `http://<PS4 IP>:8770` in the browser of a phone or computer on the **same network**, or in the PS4's own browser (which gets a simpler page).

| State | Meaning |
| --- | --- |
| Ready | In the library, ready to play |
| Restart to play | Added in this boot: restart once |
| Running | Open |
| Adding | Being added |
| Not in library | Out of the library; its pkgs are still on the drive |
| Drive not connected | Its drive is not connected: connect it to play |
| Installed on console | A normal install of this game exists: PS4Mount leaves it alone |
| On another drive | The same game is on another connected drive, which is the one used |

Each game has a page (click its artwork) with its version, the firmware it needs, its size and where it is.

![A game's page](docs/panel-game.png)

Buttons: **Remove from library** (takes it off the console and keeps its pkgs), **Add to library**, **Scan for games**, **Send games**, **Settings**, **Log** and **Stop**.

A game's page also has **Restore original files**: it undoes what PS4Mount did to its files. It takes the game off the console and puts its pkgs back where they were, with the names they had (for example `ps4/CUSA00092/app.pkg` becomes `PS4Mount/Flower/Flower.pkg` again). It is there to get your files back as they were, for example to install the game the usual way. Nothing is deleted, and PS4Mount does not pick those pkgs up again.

### Sending games from a computer

In the panel, **Send games**: drop the pkgs and pick where they go (the USB drive or the console's storage).

- Every pkg is **checked before it is sent**, in a second, from its first 4 MB: whether it is a fake PS4 pkg, a game, an update or an add-on, whether your firmware is enough, whether it is complete and whether it fits. Only the pkgs the console can use are sent.
- Games go first, then updates, then add-ons.
- A cancelled or failed send leaves no half file behind, and **Send again** retries it.
- When it arrives, the game is added by itself.

![Sending games](docs/panel-send.png)

### Settings

| Setting | Default | Meaning |
| --- | --- | --- |
| Folders to search for pkgs | `/PS4Mount` | Folders of each drive where new pkgs are picked up, one per line (`/` is the root: every game pkg in the root would be moved into `ps4/`). Two levels of subfolders are looked into. `ps4` cannot be used. |
| Add new games in groups / Games per group | On / 5 | Adds many games in groups, which keeps the console stable. |
| Install add-ons (DLC) | On | Installs add-ons (see above). |
| Also use pkgs in the console's /data/PS4Mount | On | Uses the pkgs in the console's storage too. |
| Language | The console's | The language of the panel and the notifications: the console's, English or Spanish. |

They are saved in `/data/ps4mount/config.txt`. The settings also have **Restore the files of all games** (or of chosen ones) and **Use restored pkgs again**.

### Removing and deleting games

- **Deleting on the console** (Options → Delete, or Settings → Storage): deletes the game **and its pkgs** (game, update and add-ons) and frees the space, as with an installed game. Only when the delete is confirmed on those screens, and only that game's folder.
- **Remove from library** in the panel: takes the game off the console and **keeps its pkgs**, to add it again with **Add to library**.

While the delete screens are open, PS4Mount unmounts the game folders, so the system's delete never reaches the pkgs: PS4Mount deletes them, and only that game's folder.

### Good to know

- **Load PS4Mount after every boot.** Without it, games cannot start (they stay on the home screen).
- **Close PS4Mount games before putting the console in rest mode**: rest mode powers the USB drive off.
- If you **unplug the USB drive**, its games stay on the home screen and the panel shows them as *Drive not connected*. When you plug it back, wait for the `usb0: … found` notification before starting them.
- If GoldHEN loads PS4Mount automatically and you load it again, you see `PS4Mount … is already running`: that is fine, the second one stops by itself.
- The panel has no password: anyone on your local network can open it. Do not expose port 8770 to the Internet.

<a name="troubleshooting"></a>
### Troubleshooting

| Problem | What to do |
| --- | --- |
| CE-32930-7 when starting a game just added | Restart the console once and load PS4Mount again. |
| "Cannot start the application" | Check that PS4Mount is running and that the game's drive is connected (PS4Mount says so when it is not). |
| The panel does not open | Use the **console's** IP, with `http://` in front and `:8770`, from the same network. |
| A pkg is not added | Read the log (panel → **Log**): it says why (not fake, needs another firmware, incomplete…). |
| A game crashes or loses saves when installed the normal way too | It is the pkg (or its backport), not PS4Mount: try another dump. |

To report a problem, open an [issue](https://github.com/Vld439/PS4Mount-PS4/issues) with the PS4Mount version, the firmware, the game (`CUSA…`), what happened and the log (panel → **Log**, or `/data/ps4mount/log.txt` over FTP). Do not attach pkgs or links to them.

### Files on the console

| File | Use |
| --- | --- |
| `/data/ps4mount/log.txt` | The log |
| `/data/ps4mount/config.txt` | The settings |
| `/data/ps4mount/stop` | Empty: stops PS4Mount |
| `/data/ps4mount/klog_all` | Empty: copies the whole system log into PS4Mount's log (diagnosis) |
| `/system_data/ps4mount/` | The games' internal folders |

## Building

```
bash test/run.sh        # tests on the computer (SANITIZE=1 for ASan/UBSan)
bash test/policy.sh     # the project's rules
cd test/web && npm test # the panel
PS4_PAYLOAD_SDK=/path/to/sdk bash build.sh   # build/ps4mount-ps4.elf and .bin
```

You need the [ps4-payload-sdk](https://github.com/ps4-payload-dev/sdk) **built from git**: the v0.9 release does not start on 13.52 (its runtime knows firmwares up to 13.50). CI uses the commit in `SDK_COMMIT` of `.github/workflows/build.yml`. [AI-WORKFLOW.md](AI-WORKFLOW.md) explains how to work on PS4Mount with an AI assistant.

## Credits

- **[PS4Mount](https://github.com/gasb07/ps4mount)** by **gasb07**: the project this port comes from (pkg reading, the install files, mounting, the panel). Its history is in the original project and in this repository's git history.
- **[LibOrbisPkg](https://github.com/maxton/LibOrbisPkg)** (maxton, LGPL-3.0): the fake pkg key (`src/keys.h`) and the reference for the format.
- **[ps4-payload-sdk](https://github.com/ps4-payload-dev/sdk)** (John Törnblom, GPL-3.0-or-later): the toolchain and the payload runtime.
- **flatz**: the BGFT interface documented in [ps4_stub_lib_maker_v2](https://github.com/flatz/ps4_stub_lib_maker_v2), and **[ps4-ipi](https://github.com/0x199/ps4-ipi)** as a reference for its use, to install add-ons.
- **[SQLite](https://sqlite.org)** (public domain): reading the console's database.
- **[ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus)** (drakmor): the original's inspiration for mounting games from USB drives.

## License

GPL-3.0-or-later (see [LICENSE](LICENSE)). If you publish your own version, keep the notices, distribute the changes under the same license and, please, mention gasb07 and link the original project.
