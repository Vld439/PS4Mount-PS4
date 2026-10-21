# Privacy / Privacidad

**English** · [Español](#español)

PS4Mount PS4 is a payload that runs on your own PlayStation 4. It has no server, no account of its own, no analytics and no telemetry: nothing is sent to the authors or to anyone else.

**Google Drive (optional).** If you connect Google Drive in PS4Mount's panel:

- You use **your own** Google Cloud OAuth client (client ID and secret). PS4Mount does not ship one. They are stored only on your console, in `/data/ps4mount/drive_client.txt`.
- PS4Mount asks for one permission, `https://www.googleapis.com/auth/drive.file`: it can see, create and delete **only the files it creates** in your Drive (the `PS4Mount` folder). It cannot see any other file of your Drive.
- The access it receives (a refresh token) is stored only on your console, in `/data/ps4mount/drive.txt`. *Disconnect* in the panel revokes it at Google and deletes it; you can also remove the access at any time in your Google account (Security → Third-party connections).
- Your console talks directly to Google's servers over HTTPS. The files it uploads are the game packages you choose to back up, and they go to your own Drive. No data passes through any other server.
- PS4Mount reads your account's email address and Drive storage use only to show them in the panel.

PS4Mount's use of information received from Google APIs adheres to the [Google API Services User Data Policy](https://developers.google.com/terms/api-services-user-data-policy), including the Limited Use requirements.

**Downloads from links (optional).** The links you paste in the panel are stored on your console only (`/data/ps4mount/links_queue.txt`, until the download ends or you remove it), as are the passwords of archives you write and the ones that opened one (`/data/ps4mount/passwords.txt`, until you delete it), and your console connects directly to the websites in them. Those websites see your console's address, as with any download; PS4Mount sends them nothing else (no cookies, no account).

**The web panel** runs on your console (port 8770) and is reachable from your local network only. It has no password: stop PS4Mount when you are done if others share your network.

Questions: open an issue at https://github.com/Vld439/PS4Mount-PS4/issues.

## Español

PS4Mount PS4 es un payload que se ejecuta en tu propia PlayStation 4. No tiene servidor, ni cuenta propia, ni analíticas, ni telemetría: no se envía nada a los autores ni a nadie.

**Google Drive (opcional).** Si conectas Google Drive en el panel de PS4Mount:

- Usas **tu propio** cliente OAuth de Google Cloud (ID de cliente y secreto). PS4Mount no trae ninguno. Se guardan solo en tu consola, en `/data/ps4mount/drive_client.txt`.
- PS4Mount pide un único permiso, `https://www.googleapis.com/auth/drive.file`: puede ver, crear y borrar **solo los archivos que él crea** en tu Drive (la carpeta `PS4Mount`). No puede ver ningún otro archivo de tu Drive.
- El acceso que recibe (un token de actualización) se guarda solo en tu consola, en `/data/ps4mount/drive.txt`. *Desconectar* en el panel lo revoca en Google y lo borra; también puedes quitar el acceso cuando quieras en tu cuenta de Google (Seguridad → Conexiones de terceros).
- Tu consola habla directamente con los servidores de Google por HTTPS. Los archivos que sube son los paquetes de juegos que tú decides copiar, y van a tu propio Drive. Ningún dato pasa por otro servidor.
- PS4Mount lee el correo de tu cuenta y el espacio usado de tu Drive solo para mostrarlos en el panel.

El uso que PS4Mount hace de la información recibida de las APIs de Google cumple la [Política de Datos de Usuario de los Servicios de API de Google](https://developers.google.com/terms/api-services-user-data-policy), incluidos los requisitos de Uso Limitado.

**Descargas desde enlaces (opcional).** Los enlaces que pegas en el panel se guardan solo en tu consola (`/data/ps4mount/links_queue.txt`, hasta que termina la descarga o la quitas), igual que las contraseñas de archivos comprimidos que escribes y las que abrieron alguno (`/data/ps4mount/passwords.txt`, hasta que lo borres), y tu consola se conecta directamente a las webs de esos enlaces. Esas webs ven la dirección de tu consola, como en cualquier descarga; PS4Mount no les envía nada más (ni cookies, ni cuenta).

**El panel web** se ejecuta en tu consola (puerto 8770) y solo es accesible desde tu red local. No tiene contraseña: detén PS4Mount al terminar si compartes la red con otras personas.

Preguntas: abre un issue en https://github.com/Vld439/PS4Mount-PS4/issues.
