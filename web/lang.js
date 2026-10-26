/* Texts of both panel pages in other languages, by their English text (English is the
 * page itself, and the fallback for a text a table lacks). build.sh puts this file in
 * the pages where they say <!--LANG-->. A new language: a table here and its tag in
 * src/lang.c. ES5 only: the browser of older firmwares reads it too. */
var LANGS = {
  es: {
    /* fixed texts of the pages */
    'Search game or CUSA': 'Buscar juego o CUSA',
    'Scan for games': 'Buscar juegos',
    'Send games': 'Enviar juegos',
    'Settings': 'Ajustes',
    'Log': 'Log',
    'Stop': 'Detener',
    'Games': 'Juegos',
    'Back': 'Volver',
    'Save': 'Guardar',
    'Cancel': 'Cancelar',
    'Close': 'Cerrar',
    'No PS4 games found. Put the .pkg files (game, update and add-ons) in the PS4Mount folder of a USB drive, or send them with Send games.':
      'No hay juegos de PS4. Pon los .pkg (juego, actualización y DLC) en la carpeta PS4Mount de un USB, o envíalos con Enviar juegos.',
    'No PS4 games found. Put the .pkg files (game, update and add-ons) in the PS4Mount folder of a USB drive.':
      'No hay juegos de PS4. Pon los .pkg (juego, actualización y DLC) en la carpeta PS4Mount de un USB.',
    'Adding games': 'Añadir juegos',
    'Folders to search for pkgs (one per line, relative to each drive)':
      'Carpetas donde buscar pkgs (una por línea, dentro de cada unidad)',
    '/ is the root of the drive. The ps4 folder holds organized games, so it cannot be used here.':
      '/ es la raíz de la unidad. La carpeta ps4 guarda los juegos organizados, así que no se puede usar aquí.',
    '/ is the root of the drive. Games are moved into the ps4 folder, so it cannot be used here.':
      '/ es la raíz de la unidad. Los juegos se mueven a la carpeta ps4, así que no se puede usar aquí.',
    'Add new games in groups': 'Añadir los juegos nuevos por grupos',
    'Games per group': 'Juegos por grupo',
    'Smaller groups are gentler on the system when many games are added at once.':
      'Los grupos pequeños cargan menos el sistema cuando se añaden muchos juegos a la vez.',
    'Install add-ons (DLC)': 'Instalar DLC',
    'Organize pkgs into the ps4 folder': 'Organizar los pkgs en la carpeta ps4',
    'Off (the default): the .pkg files are used where they are, anywhere on the drive, grouped by their title ID. They are not renamed or moved; the games PS4Mount organized before go back to where they were.':
      'Apagado (por defecto): los .pkg se usan donde están, en cualquier parte de la unidad, agrupados por su título. No se renombran ni se mueven; los juegos que PS4Mount organizó antes vuelven a su sitio.',
    "Also use pkgs in the console's /data/PS4Mount": 'Usar también los pkgs de /data/PS4Mount de la consola',
    "Pkgs sent to the console's storage are added from there, with no install to wait for. They still take internal space.":
      'Los pkgs enviados a la memoria de la consola se añaden desde ahí, sin esperar una instalación. Ocupan espacio interno.',
    'NAS (NFS or SMB)': 'NAS (NFS o SMB)',
    'Not available on this firmware: the NAS needs 13.52.': 'No disponible en este firmware: el NAS necesita 13.52.',
    'Use a network share (read only)': 'Usar un recurso de red (solo lectura)',
    'Only firmware 13.52, and only with pkgs used where they are: the share is never renamed or moved. It needs a wired console or a good Wi-Fi: games read at the speed of the network.':
      'Solo firmware 13.52, y solo con los pkgs usados donde están: el recurso nunca se renombra ni se mueve. Hace falta la consola por cable o un buen Wi-Fi: los juegos leen a la velocidad de la red.',
    'Protocol': 'Protocolo',
    'Server': 'Servidor',
    'Export (NFS) or share (SMB)': 'Exportación (NFS) o recurso (SMB)',
    'Folder inside it': 'Carpeta dentro',
    'Port': 'Puerto',
    'User (SMB)': 'Usuario (SMB)',
    'Password (SMB)': 'Contraseña (SMB)',
    'saved': 'guardada',
    "Find servers on the network":
      "Buscar servidores en la red",
    "List the server's shares":
      "Ver los recursos del servidor",
    "How to share a folder from a computer or a NAS":
      "Cómo compartir una carpeta desde un ordenador o un NAS",
    "Windows:":
      "Windows:",
    "right-click the folder, Properties, Sharing, Advanced Sharing, tick \"Share this folder\". Then fill in User and Password here with your Windows account. If the console does not find the PC, set its network as Private.":
      "clic derecho en la carpeta, Propiedades, Compartir, Uso compartido avanzado, marca «Compartir esta carpeta». Luego pon aquí en Usuario y Contraseña tu cuenta de Windows. Si la consola no encuentra el PC, pon su red como Privada.",
    "macOS:":
      "macOS:",
    "System Settings, General, Sharing: turn on File Sharing and add the folder. In Options, turn on SMB sharing for your user, and use that user and password here.":
      "Ajustes del Sistema, General, Compartir: activa Compartir archivos y añade la carpeta. En Opciones, activa compartir por SMB para tu usuario, y usa aquí ese usuario y contraseña.",
    "Linux:":
      "Linux:",
    "share the folder with Samba (read only, guest allowed) or NFS (/etc/exports), and let the console's address through the firewall.":
      "comparte la carpeta con Samba (solo lectura, invitado permitido) o NFS (/etc/exports), y deja pasar la dirección de la consola en el cortafuegos.",
    "NAS box":
      "Caja NAS",
    "(Synology, QNAP, a router with USB): turn on SMB file sharing in its settings and give the shared folder read access.":
      "(Synology, QNAP, un router con USB): activa compartir archivos por SMB en sus ajustes y da permiso de lectura a la carpeta compartida.",
    "A cable is much faster than Wi-Fi. With no switch or free router port, a cable straight from the computer to the console works too: share the computer's internet connection on that cable.":
      "Un cable es mucho más rápido que el Wi-Fi. Si no tienes switch ni un puerto libre en el router, también sirve un cable directo del ordenador a la consola: comparte la conexión a internet del ordenador por ese cable.",
    "Looking for servers on the console's network...":
      "Buscando servidores en la red de la consola...",
    "Choose a server:":
      "Elige un servidor:",
    "No server found. Check that the computer or NAS is on, on the same network as the console, shares a folder, and that its firewall allows file sharing.":
      "No se encontró ningún servidor. Comprueba que el ordenador o el NAS esté encendido, en la misma red que la consola, que comparta una carpeta y que su cortafuegos permita compartir archivos.",
    "Find a server first, or type its address in Server.":
      "Busca primero un servidor, o escribe su dirección en Servidor.",
    "Asking {0} what it shares...":
      "Preguntando a {0} qué comparte...",
    "Fill in User and Password, then press \"List the server's shares\".":
      "Rellena Usuario y Contraseña y pulsa «Ver los recursos del servidor».",
    "Choose the shared folder with your games:":
      "Elige la carpeta compartida con tus juegos:",
    "This server shares no folders.":
      "Este servidor no comparte ninguna carpeta.",
    "Ready: press Save to use it.":
      "Listo: pulsa Guardar para usarlo.",
    'Turn on BinLoader (port 9090) in GoldHEN to use the share.':
      'Enciende BinLoader (puerto 9090) en GoldHEN para usar el recurso.',
    'guest': 'invitado',
    'Language': 'Idioma',
    'Restore original files': 'Restaurar archivos originales',
    'Takes games off the console and puts their pkgs (game, update and add-ons) back where they were, with the names they had. Nothing is deleted. The open game is closed first.':
      'Quita juegos de la consola y vuelve a dejar sus pkgs (juego, actualización y DLC) donde estaban, con el nombre que tenían. No se borra nada. Antes se cierra el juego abierto.',
    'Takes the game off the console and puts its pkgs back where they were, with the names they had. Nothing is deleted.':
      'Quita el juego de la consola y vuelve a dejar sus pkgs donde estaban, con el nombre que tenían. No borra nada.',
    'Restore the files of all games': 'Restaurar los archivos de todos los juegos',
    'Restore the files of chosen games': 'Restaurar los archivos de los juegos elegidos',
    'Use restored pkgs again': 'Volver a usar los pkgs restaurados',
    'Each chosen game gets its pkgs back where they were, with its update and all its add-ons. The open game is closed first.':
      'A cada juego elegido se le devuelven sus pkgs donde estaban, con su actualización y todos sus DLC. Antes se cierra el juego abierto.',
    'Files': 'Archivos',
    'Restore the chosen ones': 'Restaurar los elegidos',
    'Restore the original files of {0}?\n\nThe game leaves the console and its pkgs (game, update and add-ons) go back where they were, with the names they had. The open game is closed first. PS4Mount stops using these pkgs. Nothing is deleted.':
      '¿Restaurar los archivos originales de {0}?\n\nEl juego sale de la consola y sus pkgs (juego, actualización y DLC) vuelven donde estaban, con el nombre que tenían. Antes se cierra el juego abierto. PS4Mount deja de usar esos pkgs. No se borra nada.',
    'WARNING: this restores the original files of ALL {0} games: their pkgs (game, update and add-ons) go back where they were, with the names they had.\n\nThe open game is closed now, without saving. All PS4Mount games leave the console, {1} at a time, and PS4Mount stops using these pkgs. Nothing is deleted.\n\nContinue?':
      'ATENCIÓN: esto restaura los archivos originales de LOS {0} juegos: sus pkgs (juego, actualización y DLC) vuelven donde estaban, con el nombre que tenían.\n\nEl juego abierto se cierra ahora, sin guardar. Todos los juegos de PS4Mount salen de la consola, de {1} en {1}, y PS4Mount deja de usar esos pkgs. No se borra nada.\n\n¿Continuar?',
    'Restore the original files of {0} game(s), each with its update and all its add-ons?\n\nThe open game is closed now, without saving. The games leave the console{1}. Nothing is deleted.':
      '¿Restaurar los archivos originales de {0} juego(s), cada uno con su actualización y todos sus DLC?\n\nEl juego abierto se cierra ahora, sin guardar. Los juegos salen de la consola{1}. No se borra nada.',
    'Use the restored pkgs again? They are moved into the ps4 folder and added on the next scan.':
      '¿Volver a usar los pkgs restaurados? Se moverán a la carpeta ps4 y se añadirán en la próxima búsqueda.',
    "The console's": 'El de la consola',
    'Games removed from the library per group (1 to': 'Juegos que salen de la biblioteca por grupo (1 a',
    'Select all': 'Elegir todos',
    'Game': 'Juego',
    'Pick the pkgs on this computer. Each one is checked first (it takes a second): only pkgs this console can use are sent, so no transfer ends in a game that does not start. They are added to the library when they arrive.':
      'Elige los pkgs de este equipo. Cada uno se comprueba antes (tarda un segundo): solo se envían los que esta consola puede usar, así ningún envío acaba en un juego que no arranca. Se añaden a la biblioteca al llegar.',
    'Drop .pkg files here, or click to pick them': 'Suelta aquí los .pkg, o pulsa para elegirlos',
    'Send to': 'Enviar a',
    'Cancel transfer': 'Cancelar el envío',
    'Send': 'Enviar',
    /* Google Drive */
    'uploading to Drive ({0}%)': 'subiendo a Drive ({0} %)',
    'downloading from Drive ({0}%)': 'descargando de Drive ({0} %)',
    'waiting to go to Drive': 'en cola para subir a Drive',
    'waiting to come from Drive': 'en cola para bajar de Drive',
    'reading again what already arrived…': 'releyendo lo que ya llegó…',
    'reading again what Drive already has…': 'releyendo lo que Drive ya tiene…',
    'Google Drive asks to connect again: open {0} and type {1}': 'Google Drive pide conectar de nuevo: abre {0} y escribe {1}',
    'The transfer goes on once you do': 'La transferencia sigue en cuanto lo hagas',
    '{0} of {1}': '{0} de {1}',
    'connection lost, trying again in {0} s': 'se cortó la conexión, reintento en {0} s',
    'checking the file…': 'comprobando el archivo…',
    '{0} left': 'quedan {0}',
    'measuring the speed…': 'midiendo la velocidad…',
    'running for {0}': 'lleva {0}',
    'less than a minute': 'menos de un minuto',
    'First, your own Google client (once). PS4Mount comes with none: yours stays on this console only.':
      'Primero, tu propio cliente de Google (una sola vez). PS4Mount no trae ninguno: el tuyo se queda solo en esta consola.',
    'Open': 'Abre',
    ', create a project and enable the Google Drive API.': ', crea un proyecto y activa la Google Drive API.',
    'In Google Auth Platform (OAuth consent screen): audience External, and add your Google email in Audience → Test users (the app stays in Testing).':
      'En Google Auth Platform (pantalla de consentimiento OAuth): público Externo, y añade tu correo de Google en Público → Usuarios de prueba (la app se queda en Prueba).',
    'In Clients (Credentials), create an OAuth client of type TVs and Limited Input devices, and copy its client ID and secret here.':
      'En Clientes (Credenciales), crea un cliente OAuth de tipo TV y dispositivos de entrada limitada, y copia aquí su ID de cliente y su secreto.',
    'Client ID': 'ID de cliente',
    'Client secret': 'Secreto del cliente',
    'Change': 'Cambiar',
    'Your Google client: {0}': 'Tu cliente de Google: {0}',
    'Google Drive': 'Google Drive',
    'console.cloud.google.com': 'console.cloud.google.com',
    'Back up to Drive': 'Copia en Drive',
    'Connect Google Drive': 'Conectar Google Drive',
    'Copies of your games in your own Google Drive: back up the library, send a game, and download it back to any drive of the console. PS4Mount can only see the files it puts in Drive (folder PS4Mount), nothing else of your Drive.':
      'Copias de tus juegos en tu propio Google Drive: haz una copia de la biblioteca, envía un juego y descárgalo después a cualquier unidad de la consola. PS4Mount solo puede ver los archivos que él pone en Drive (carpeta PS4Mount), nada más de tu Drive.',
    'On your phone or computer, open': 'En el móvil o el ordenador, abre',
    'and type this code:': 'y escribe este código:',
    'Waiting for you to allow it in your Google account…': 'Esperando a que lo permitas en tu cuenta de Google…',
    'Connect': 'Conectar',
    'Back up the library': 'Copiar la biblioteca',
    'Refresh': 'Actualizar',
    'Disconnect': 'Desconectar',
    'Download to': 'Descargar en',
    'In Drive': 'En Drive',
    'Send to Drive': 'Enviar a Drive',
    'Send to Drive again': 'Enviar a Drive de nuevo',
    'In Google Drive': 'En Google Drive',
    'in Drive ({0})': 'en Drive ({0})',
    'not in Drive': 'no está en Drive',
    'Uploading {0} to Drive': 'Subiendo {0} a Drive',
    'Downloading {0} from Drive': 'Descargando {0} de Drive',
    'Moving {0} to the Drive trash': 'Moviendo {0} a la papelera de Drive',
    'Reading Google Drive': 'Leyendo Google Drive',
    'Asking Google for a code': 'Pidiendo un código a Google',
    '{0} more waiting': '{0} más en espera',
    '{0} of {1} used': '{0} de {1} usados',
    '{0} used': '{0} usados',
    'on the console': 'en la consola',
    'Download': 'Descargar',
    'Delete from Drive': 'Borrar de Drive',
    "Nothing in Google Drive yet: use Back up the library, or Send to Drive on a game's page.":
      'Aún no hay nada en Google Drive: usa Copiar la biblioteca, o Enviar a Drive en la ficha de un juego.',
    'Disconnect Google Drive? The copies stay in your Drive; PS4Mount forgets the account.':
      '¿Desconectar Google Drive? Las copias se quedan en tu Drive; PS4Mount olvida la cuenta.',
    'Back up the whole library to Google Drive? Games already there are skipped. The console must stay on (not in rest mode) until it ends.':
      '¿Copiar toda la biblioteca a Google Drive? Los juegos que ya están se saltan. La consola debe seguir encendida (no en reposo) hasta que termine.',
    'Stop the Drive transfer? An upload goes on from where it was the next time.':
      '¿Detener la transferencia de Drive? Una subida sigue desde donde iba la próxima vez.',
    'Move {0} to the Drive trash? The pkgs on the console stay.': '¿Mover {0} a la papelera de Drive? Los pkgs de la consola se quedan.',
    /* states */
    'Ready': 'Listo',
    'Restart to play': 'Reinicia para jugar',
    'Running': 'Abierto',
    'Not in library': 'Fuera de la biblioteca',
    'Installed on console': 'Instalado en la consola',
    'Adding': 'Añadiéndose',
    'On another drive': 'En otra unidad',
    'Drive not connected': 'Unidad no conectada',
    /* game cards and pages */
    'Add to library': 'Añadir a la biblioteca',
    'Remove from library': 'Quitar de la biblioteca',
    'Close the game to remove it': 'Cierra el juego para quitarlo',
    'Please wait…': 'Espera…',
    'Please wait...': 'Espera...',
    ' (with update)': ' (con actualización)',
    'mounted {0}': 'montado {0}',
    '1 add-on': '1 DLC',
    '{0} add-ons': '{0} DLC',
    'no add-ons': 'sin DLC',
    'Game{0}{1}': 'Juego{0}{1}',
    ', update': ', actualización',
    ' and {0}': ' y {0}',
    'Added. Restart the console once to play it.': 'Añadido. Reinicia la consola una vez para jugarlo.',
    'On {0}, which is not connected. Connect it to play.': 'En {0}, que no está conectado. Conéctalo para jugar.',
    '{0} game(s)': '{0} juego(s)',
    'State': 'Estado',
    'Version': 'Versión',
    'v{0} (game v{1} with its update)': 'v{0} (juego v{1} con su actualización)',
    'Add-ons': 'DLC',
    'none': 'ninguno',
    'Needs firmware': 'Necesita firmware',
    'unknown': 'desconocido',
    'Size': 'Tamaño',
    'On': 'En',
    'game': 'juego',
    'update': 'actualización',
    'add-on': 'DLC',
    'other': 'otro',
    /* messages */
    'Error: {0}': 'Error: {0}',
    "Also install big add-ons (over 256 MB)":
      "Instalar también los DLC grandes (más de 256 MB)",
    "A game only sees an add-on the system has installed: big ones are copied to the console's storage, in the background (a few minutes). Off, they are not used.":
      "Un juego solo ve un DLC que el sistema ha instalado: los grandes se copian a la memoria de la consola, en segundo plano (unos minutos). Apagado, no se usan.",
    "Ready to install":
      "Lista para instalar",
    "A homebrew app: it is sent to the console and installed with the system's installer, as by hand. The copy is deleted once it is installed.":
      "Una app homebrew: se envía a la consola y se instala con el instalador del sistema, como a mano. La copia se borra cuando queda instalada.",
    'Could not read the list: {0}': 'No se pudo leer la lista: {0}',
    'Could not read the settings: {0}': 'No se pudieron leer los ajustes: {0}',
    'Could not read the log: {0}': 'No se pudo leer el log: {0}',
    'Games per group must be a number from 1 to {0}': 'Los juegos por grupo deben ser un número de 1 a {0}',
    'Remove {0} from the library? Its pkgs stay where they are (deleting it on the console deletes them).':
      '¿Quitar {0} de la biblioteca? Sus pkgs se quedan donde están (borrarlo desde la consola sí los borra).',
    '{0} game(s) selected': '{0} juego(s) elegido(s)',
    ', removed from the library {0} at a time': ', salen de la biblioteca de {0} en {0}',
    'No games selected': 'No hay juegos elegidos',
    'No PS4Mount games on the connected drives': 'No hay juegos de PS4Mount en las unidades conectadas',
    ', {0} at a time': ', de {0} en {0}',
    '{0} free': '{0} libres',
    'No drive connected': 'No hay ninguna unidad conectada',
    'no place to send it to': 'no hay dónde enviarlo',
    'not enough space in {0}: {1} free, {2} needed': 'no hay espacio en {0}: {1} libres, {2} necesarios',
    'Checking…': 'Comprobando…',
    'Sending {0}%': 'Enviando {0}%',
    'Sent': 'Enviado',
    'Failed': 'Falló',
    'Cannot be used': 'No se puede usar',
    'Ready to send': 'Listo para enviar',
    'needs firmware {0}': 'necesita el firmware {0}',
    'Send again': 'Enviar de nuevo',
    'Remove': 'Quitar',
    'could not be checked: {0}': 'no se pudo comprobar: {0}',
    'the connection to the console was lost': 'se perdió la conexión con la consola',
    'cancelled': 'cancelado',
    'Cancel the transfer in progress?': '¿Cancelar el envío en curso?',
    'Stop PS4Mount? The games stay unavailable until it is loaded again.':
      '¿Detener PS4Mount? Los juegos no se podrán abrir hasta que se vuelva a cargar.',
    /* downloads from links */
    'Download links': 'Descargar enlaces',
    'The console downloads them itself (you can close this page): a pkg, a ZIP or a RAR, also in parts. Paste the direct links, the ones that start the download, one per line; all the parts of a RAR (name.part1.rar, name.part2.rar...) become one download. Its pkgs are added to the library when they arrive.':
      'La consola los descarga sola (puedes cerrar esta página): un pkg, un ZIP o un RAR, también en partes. Pega los enlaces directos, los que empiezan la descarga, uno por línea; todas las partes de un RAR (nombre.part1.rar, nombre.part2.rar...) son una sola descarga. Sus pkgs se añaden a la biblioteca al llegar.',
    'A link with no file name in it: add | and its name.': 'Un enlace sin nombre de archivo: añade | y su nombre.',
    'Password (if the archive has one)': 'Contraseña (si el archivo la tiene)',
    'Not needed for one that opened another download, nor for the site in brackets at the start of the name ([SITE]-game): those are tried by themselves.':
      'No hace falta si ya abrió otra descarga, ni si es el sitio entre corchetes al principio del nombre ([SITIO]-juego): esas se prueban solas.',
    'The password of the archive:': 'La contraseña del archivo comprimido:',
    'Save to': 'Guardar en',
    'Checking the links of {0}…': 'Comprobando los enlaces de {0}…',
    'through the browser': 'a través del navegador',
    'Downloading {0}': 'Descargando {0}',
    'Extracting {0}': 'Extrayendo {0}',
    'extracting {0}': 'extrayendo {0}',
    'downloading': 'descargando',
    'waiting': 'en espera',
    '{0} parts': '{0} partes',
    'Try again': 'Reintentar',
    'paste at least one link': 'pega al menos un enlace',
    'Remove this download? What it downloaded so far is deleted.': '¿Quitar esta descarga? Se borra lo que ya bajó.',
    'Cancel the download of {0}? What it downloaded so far is deleted.': '¿Cancelar la descarga de {0}? Se borra lo que ya bajó.'
  }
};
var LANG = 'en';

/* The text in the panel's language; {0}, {1}... are replaced by the other arguments. */
function t(s) {
  var tab = LANGS[LANG], r = (tab && tab[s]) || s;
  for (var i = 1; i < arguments.length; i++) r = r.split('{' + (i - 1) + '}').join(arguments[i]);
  return r;
}

function known(k) {
  for (var l in LANGS) if (LANGS[l][k]) return true;
  return false;
}

/* The fixed texts of the page (text nodes and placeholder/title), by their English text,
 * kept on each node so the page can switch back. Generated parts use t() themselves. */
function translatePage() {
  var walker = document.createTreeWalker(document.body, 4, null, false), n, k, attrs, i, j, el, a;
  while ((n = walker.nextNode())) {
    if (/^(SCRIPT|STYLE|PRE|TEXTAREA)$/.test(n.parentNode.nodeName)) continue;
    if (n.psmEn === undefined) {
      k = n.nodeValue.replace(/\s+/g, ' ').replace(/^ | $/g, '');
      if (!k || !known(k)) continue; /* only texts some table knows */
      n.psmEn = k;
      n.psmLead = /^\s/.test(n.nodeValue) ? ' ' : '';
      n.psmTrail = /\s$/.test(n.nodeValue) ? ' ' : '';
    }
    n.nodeValue = n.psmLead + t(n.psmEn) + n.psmTrail;
  }
  attrs = ['placeholder', 'title'];
  for (i = 0; i < attrs.length; i++) {
    a = attrs[i];
    el = document.querySelectorAll('[' + a + ']');
    for (j = 0; j < el.length; j++) {
      if (!el[j].getAttribute('data-en-' + a)) el[j].setAttribute('data-en-' + a, el[j].getAttribute(a));
      el[j].setAttribute(a, t(el[j].getAttribute('data-en-' + a)));
    }
  }
}

/* Switches the page to tag ("es", "en"); English for a language with no table. */
function setLang(tag) {
  var code = LANGS[tag] ? tag : 'en';
  if (code === LANG) return false;
  LANG = code;
  document.documentElement.lang = code;
  translatePage();
  return true;
}
