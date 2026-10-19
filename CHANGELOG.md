# Changelog

PS4Mount PS4 is a PS4-only port of [PS4Mount](https://github.com/gasb07/ps4mount) by gasb07 (PS5); its versions start again at 0.1.0. The history of the original is in its project and in this repository's git history.

## 0.1.0-beta

First version of the PS4 port. Tested on a PS4 on firmware 13.52 with GoldHEN.

**Español**

- Juega pkgs fake de PS4 sin instalarlos, desde un USB exFAT (también pkgs de más de 4 GB) o desde la memoria de la consola (`/data/pkg`).
- Actualizaciones, y DLC de hasta 256 MB instalados con el instalador del sistema en segundos (aparecen en la información del juego).
- Panel web (`http://<IP de la PS4>:8770`) con la biblioteca, portadas y una ficha por juego, también desde el navegador de la PS4.
- Enviar juegos desde el PC por la red, comprobando cada pkg antes de enviarlo (pkg fake, firmware, si está completo, si cabe), con cancelar y enviar de nuevo.
- Borrar un juego desde la consola borra también sus pkgs; *Quitar de la biblioteca* los conserva. Mientras la consola borra, PS4Mount aparta las carpetas de los juegos: el borrado del sistema nunca llega a los pkgs.
- Carpetas por juego al copiar (hasta dos niveles: `Juego/Game`, `Juego/Update`, `Juego/DLC`).
- Los juegos de un USB desconectado se muestran como *Unidad no conectada*, y abrir uno lo explica.
- Panel y notificaciones en español e inglés, según el idioma de la consola o los ajustes.
- Limitación: un juego nuevo (o una actualización que llega después) necesita reiniciar la consola una vez.

**English**

- Plays PS4 fake pkgs without installing them, from an exFAT USB drive (pkgs over 4 GB too) or from the console's storage (`/data/pkg`).
- Updates, and add-ons up to 256 MB installed with the system's installer in seconds (they show in the game's information).
- Web panel (`http://<PS4 IP>:8770`) with the library, artwork and a page per game, also from the PS4's browser.
- Sending games from a computer over the network, every pkg checked before it is sent (fake pkg, firmware, complete, fits), with cancel and send again.
- Deleting a game on the console deletes its pkgs too; *Remove from library* keeps them. While the console deletes, PS4Mount takes the game folders away: the system's delete never reaches the pkgs.
- A folder per game when copying (two levels down: `Game/Game`, `Game/Update`, `Game/DLC`).
- Games of an unplugged USB drive show as *Drive not connected*, and starting one says why.
- Panel and notifications in English and Spanish, following the console's language or the settings.
- Limitation: a new game (or an update that arrives later) needs one console restart.

Load `ps4mount-ps4.bin` with Payload Guest (copy it to `/data/payloads/`), or send `ps4mount-ps4.elf` to GoldHEN's payloader (port 9090).
