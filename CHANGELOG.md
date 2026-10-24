# Changelog

PS4Mount PS4 is a PS4-only port of [PS4Mount](https://github.com/gasb07/ps4mount) by gasb07 (PS5); its versions start again at 0.1.0. The history of the original is in its project and in this repository's git history.

## 0.4.18-beta

Games are now used **where they are** by default: PS4Mount no longer moves or renames their pkgs unless you turn on `Organize pkgs into the ps4 folder`. Games already organized in `ps4/<TITLE_ID>/` are used in place too, so an existing library keeps working untouched. The catalog code comes from [EzioRamesha's fork](https://github.com/EzioRamesha/ps4mount), adapted to keep the old `ps4/<TITLE_ID>` layout working. Tested on a PS4 on firmware 13.52 with GoldHEN: 31 pkgs on a USB drive (base games, updates, add-ons, merged pkgs and a duplicate copy) plus the games already organized in `/data/ps4/<TITLE_ID>/`, nothing moved or renamed.

**Español**

- **Nuevo ajuste "Organizar los pkgs en la carpeta ps4"**, **apagado por defecto**: los `.pkg` se usan **donde están, en cualquier carpeta de la unidad y con su nombre** (en la memoria interna se buscan solo `/data/PS4Mount` y `ps4`), y PS4Mount los agrupa por el título que llevan dentro (juego, actualización y DLC), sin renombrar ni mover nada. Al activarlo se mueven a `ps4/<TITLE_ID>/`, como antes.
- Los juegos **ya organizados** en `ps4/<TITLE_ID>/` se usan **en su sitio**, sin moverlos: la biblioteca antigua se sigue usando igual, y las configuraciones sin el ajuste pasan a este modo al actualizar.
- Los pkgs **base+actualización fusionados** funcionan: una actualización suelta más vieja que el juego no se usa, así que no lo hace retroceder de versión.
- Cada unidad se agrupa por separado: una actualización o un DLC que esté en **otra** unidad no se usa, para que desenchufar una no rompa los juegos de las demás.
- Borrar desde la consola un juego usado donde está **no borra tus pkgs**: lo quita de la biblioteca y lo marca como quitado (los pkgs son tuyos). **Restaurar los archivos originales** no aplica en ese modo: no hay nada que devolver.
- La búsqueda completa de la unidad (para encontrar pkgs nuevos) se hace cada minuto o al pulsar **Buscar juegos**; entre medias se comprueban los juegos ya conocidos. El catálogo se guarda en `/data/ps4mount/catalog-<unidad>.txt`, así que un reinicio no vuelve a leer todos los pkgs.

**English**

- **New "Organize pkgs into the ps4 folder" setting**, **off by default**: the `.pkg` files are used **where they are, in any folder of the drive and with their own names** (on the console's storage, only `/data/PS4Mount` and `ps4` are searched), and PS4Mount groups them by the title they carry inside (game, update and add-ons), without renaming or moving anything. Turned on, they are moved to `ps4/<TITLE_ID>/` as before.
- Games **already organized** in `ps4/<TITLE_ID>/` are used **in place**, without moving them: the old library layout keeps working as usual, and configs without the setting switch to this mode on upgrade.
- **Merged base+update pkgs** work: a separate update older than the game is not used, so it does not take the game back to an older version.
- Each drive is grouped on its own: an update or add-on on **another** drive is not used, so unplugging one drive never breaks the games of the others.
- Deleting an in-place game from the console **does not delete your pkgs**: the game leaves the library and is marked as removed (the pkgs are yours). **Restore original files** does not apply in that mode: there is nothing to move back.
- The full walk of a drive (to find new pkgs) runs every minute or when **Scan for games** is pressed; in between, the games already known are checked. The catalog is saved in `/data/ps4mount/catalog-<drive>.txt`, so a restart does not read every pkg again.

## 0.4.17-beta

The `.bin` works with older GoldHEN versions too (such as 2.4b18): from Payload Guest, Vue or the BinLoader on port 9090. Tested on firmware 13.52 with GoldHEN (port 9090 and Payload Guest): not yet with an older GoldHEN.

**Español**

- El `.bin` ahora es compatible con versiones viejas de GoldHEN (como la 2.4b18): desde Payload Guest, Vue o el BinLoader del puerto 9090.

**English**

- The `.bin` now works with older GoldHEN versions (such as 2.4b18): from Payload Guest, Vue or the BinLoader on port 9090.

## 0.4.16-beta

Games whose pkg asks for a newer firmware than the console's, made to run with a backport, are no longer refused. Tested on firmware 13.52 only: not yet on an older console.

**Español**

- Un pkg hecho para un firmware más nuevo que el de la consola ya no se rechaza (con un backport funciona): se añade, se envía o se descarga igual, con un aviso.

**English**

- A pkg built for a newer firmware than the console's is no longer refused (a backport makes it run): it is added, sent or downloaded as usual, with a warning.

## 0.4.15-beta

Downloads from links, and a browser extension that sends downloads to the console. Tested on a PS4 on firmware 13.52 with GoldHEN: a pkg and a RAR in 5 parts from a computer on the network, pkgs over HTTPS from file hosts, a 24.6 GB game in a RAR5 with its file list encrypted, in 3 parts with a code before each name, its password tried by itself (the game plays), a ZIP with AES; the extension in Brave. (The version number jumps from 0.2.0: no 0.3.)

**Español**

- **Descargar desde enlaces**, en el panel: pega enlaces directos a un pkg, un ZIP o un RAR (también en partes: `.part1.rar`… o `.rar`, `.r00`…) y la consola los descarga sola, con varias conexiones a la vez. Las partes de un RAR se juntan en una descarga, en cualquier orden, y se avisa si falta alguna.
- Un RAR o ZIP se descomprime mientras baja y solo se sacan sus pkgs; con poco espacio, cada parte se borra al descomprimirse.
- Cada pkg se comprueba con sus primeros 4 MB, como al enviar desde el PC: uno que no sirve para la descarga ahí.
- Sigue sola tras un corte de red o al volver a cargar PS4Mount; la consola no entra en reposo mientras descarga. Sigue las redirecciones, pide de nuevo un enlace que caduca y baja el número de conexiones si el servidor lo pide.
- **RAR5 con contraseña**, también con la lista de archivos cifrada (`rar -hp`) y en partes: se escribe al añadir la descarga, o al pulsar Reintentar si falta. Se prueban solas las que ya abrieron otra descarga (`/data/ps4mount/passwords.txt`) y el sitio entre corchetes al principio del nombre (`[SITIO]-juego`); una equivocada se nota al instante, sin bajar el resto. También ZIP con contraseña, clásica o AES.
- Junta las partes de un RAR aunque el sitio ponga un código distinto delante de cada nombre (`abc-juego.part1.rar`, `xyz-juego.part2.rar`).
- No abre RAR4 con contraseña, 7z, ni enlaces que piden iniciar sesión.
- **Extensión del navegador** (Chrome, Brave, Edge, Firefox; se carga a mano desde `ps4mount-extension.zip`): las descargas de pkg, ZIP y RAR van a la consola en vez de al ordenador; las partes de un RAR esperan en una cesta hasta pulsar Enviar, con la contraseña si hace falta.
- Experimental, en la extensión: el navegador baja también parte de los bloques y se los pasa a la consola por la red local (puerto 8771); si se cierra, la consola sigue sola.

**English**

- **Downloading from links**, in the panel: paste direct links to a pkg, a ZIP or a RAR (also in parts: `.part1.rar`… or `.rar`, `.r00`…) and the console downloads them by itself, over several connections at once. The parts of a RAR become one download, in any order, and a missing part is reported.
- A RAR or ZIP is extracted while it downloads and only its pkgs come out; with little space, each part is deleted once extracted.
- Every pkg is checked from its first 4 MB, as when sending from a computer: one that will not do stops the download there.
- It goes on by itself after a lost connection or when PS4Mount loads again; the console stays out of rest mode while it downloads. It follows redirects, asks again for a link that expires and uses fewer connections when the server asks for it.
- **RAR5 with a password**, also with its file list encrypted (`rar -hp`) and in parts: written when adding the download, or when pressing Try again if it is missing. The ones that opened another download (`/data/ps4mount/passwords.txt`) and the site in brackets at the start of the name (`[SITE]-game`) are tried by themselves; a wrong one shows at once, without downloading the rest. ZIPs with a password too, classic or AES.
- The parts of a RAR join up even when the host puts a different code before each name (`abc-game.part1.rar`, `xyz-game.part2.rar`).
- It does not open older RARs (4) with a password, 7z, nor links that need a sign-in.
- **Browser extension** (Chrome, Brave, Edge, Firefox; loaded by hand from `ps4mount-extension.zip`): pkg, ZIP and RAR downloads go to the console instead of the computer; the parts of a RAR wait in a basket until Send, with the password if needed.
- Experimental, in the extension: the browser fetches part of the blocks too and hands them to the console over your network (port 8771); closed, the console goes on by itself.

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
