# Changelog

PS4Mount PS4 is a PS4-only port of [PS4Mount](https://github.com/gasb07/ps4mount) by gasb07 (PS5); its versions start again at 0.1.0. The history of the original is in its project and in this repository's git history.

## 0.2.0-beta

Google Drive backups. Tested on a PS4 on firmware 13.52 with GoldHEN, uploading a 7.2 GB game to Drive, deleting it from the console, downloading it back and playing it, and a game with its update and add-on the same way.

**Español**

- **Copias en Google Drive**, opcionales: la consola sube tus juegos (con su actualización y sus DLC) a tu Drive y los vuelve a bajar a un USB o a la memoria interna, sin PC. **Copia en Drive** sube toda la biblioteca y salta lo que ya está; **Enviar a Drive** en la ficha de un juego; la ventana **Google Drive** muestra tu cuenta, tu espacio y tus juegos en Drive, con **Descargar** y **Borrar de Drive** (a la papelera).
- Cada usuario usa **sus propias credenciales de Google** (un proyecto gratuito en modo prueba: la guía está en el README), que se quedan solo en su consola. El inicio de sesión es con un código en google.com/device desde el móvil. PS4Mount solo ve los archivos que él crea en tu Drive (permiso `drive.file`).
- Subida a la velocidad de tu conexión; bajada a unos 15 MB/s, el máximo de la PS4, con 12 conexiones a la vez. Cada archivo se comprueba con su MD5.
- Una transferencia cortada (red, detener PS4Mount, apagar) sigue sola desde donde iba la próxima vez que cargas PS4Mount. Mientras transfiere, la consola no entra en reposo.
- Progreso en el panel (porcentaje, velocidad, tiempo que queda) y avisos en la pantalla de la PS4 al empezar, cada 25 % y al terminar. Si Google pide conectar de nuevo, el código sale en la pantalla.
- Política de privacidad: [PRIVACY.md](PRIVACY.md).
- Arreglos: un DLC que el instalador rechaza ya no impide añadir su juego (se avisa una vez y se reintenta la próxima vez); los DLC se instalan después de registrar el juego; un juego bajado de Drive se añade una sola vez, con todos sus pkgs; el aviso de reiniciar se mantiene al recargar PS4Mount; todos los avisos salen en el idioma de la consola.

**English**

- **Google Drive backups**, optional: the console uploads your games (with their update and add-ons) to your Drive and downloads them back to a USB drive or the console's storage, no computer needed. **Back up to Drive** uploads the whole library and skips what is already there; **Send to Drive** on a game's page; the **Google Drive** window shows your account, your space and your games in Drive, with **Download** and **Delete from Drive** (to the trash).
- Each user uses **their own Google credentials** (a free project in testing: the guide is in the README), which stay on their console only. Signing in is with a code at google.com/device from a phone. PS4Mount only sees the files it creates in your Drive (`drive.file` permission).
- Uploads at your line's speed; downloads at about 15 MB/s, the PS4's most, over 12 connections at once. Every file is checked with its MD5.
- A cut transfer (network, stopping PS4Mount, a power off) goes on by itself from where it was the next time PS4Mount loads. While it transfers, the console does not go to rest mode.
- Progress in the panel (percentage, speed, time left) and notices on the PS4's screen when it starts, every 25% and when it ends. If Google asks to connect again, the code shows on the screen.
- Privacy policy: [PRIVACY.md](PRIVACY.md).
- Fixes: an add-on the installer refuses no longer keeps its game out (one notice, retried the next time); add-ons are installed after their game is registered; a game downloaded from Drive is added once, with all its pkgs; the restart notice survives a reload of PS4Mount; every notice is in the console's language.

Load `ps4mount-ps4.bin` with Payload Guest (copy it to `/data/payloads/`), or send `ps4mount-ps4.elf` to GoldHEN's payloader (port 9090).

## 0.1.0-beta

First version of the PS4 port. Tested on a PS4 on firmware 13.52 with GoldHEN.

**Español**

- Juega pkgs fake de PS4 sin instalarlos, desde un USB exFAT (también pkgs de más de 4 GB) o desde la memoria de la consola. Solo usa lo que pongas en su carpeta `PS4Mount` de cada disco (y `/data/PS4Mount`): el resto, incluido `/data/pkg`, no lo toca.
- Actualizaciones, y DLC de hasta 256 MB instalados con el instalador del sistema en segundos (aparecen en la información del juego).
- Panel web (`http://<IP de la PS4>:8770`) con la biblioteca, portadas y una ficha por juego. Hay una versión sencilla para navegadores antiguos, sin probar todavía en el de la PS4.
- Enviar juegos desde el PC por la red, comprobando cada pkg antes de enviarlo (pkg fake, firmware, si está completo, si cabe), con cancelar y enviar de nuevo.
- Borrar un juego desde la consola borra también sus pkgs; *Quitar de la biblioteca* los conserva. Mientras la consola borra, PS4Mount aparta las carpetas de los juegos: el borrado del sistema nunca llega a los pkgs.
- Carpetas por juego al copiar (hasta dos niveles: `Juego/Game`, `Juego/Update`, `Juego/DLC`).
- Los juegos de un USB desconectado se muestran como *Unidad no conectada*, y abrir uno lo explica.
- Panel y notificaciones en español e inglés, según el idioma de la consola o los ajustes.
- Limitación: un juego nuevo (o una actualización que llega después) necesita reiniciar la consola una vez.

**English**

- Plays PS4 fake pkgs without installing them, from an exFAT USB drive (pkgs over 4 GB too) or from the console's storage. Only what you put in its `PS4Mount` folder of each drive (and `/data/PS4Mount`) is used: nothing else, `/data/pkg` included, is touched.
- Updates, and add-ons up to 256 MB installed with the system's installer in seconds (they show in the game's information).
- Web panel (`http://<PS4 IP>:8770`) with the library, artwork and a page per game. There is a simpler page for older browsers, not yet tested on the PS4's own browser.
- Sending games from a computer over the network, every pkg checked before it is sent (fake pkg, firmware, complete, fits), with cancel and send again.
- Deleting a game on the console deletes its pkgs too; *Remove from library* keeps them. While the console deletes, PS4Mount takes the game folders away: the system's delete never reaches the pkgs.
- A folder per game when copying (two levels down: `Game/Game`, `Game/Update`, `Game/DLC`).
- Games of an unplugged USB drive show as *Drive not connected*, and starting one says why.
- Panel and notifications in English and Spanish, following the console's language or the settings.
- Limitation: a new game (or an update that arrives later) needs one console restart.

Load `ps4mount-ps4.bin` with Payload Guest (copy it to `/data/payloads/`), or send `ps4mount-ps4.elf` to GoldHEN's payloader (port 9090).
