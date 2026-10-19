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
    '/ is the root of the drive. Games are moved into the ps4 folder, so it cannot be used here.':
      '/ es la raíz de la unidad. Los juegos se mueven a la carpeta ps4, así que no se puede usar aquí.',
    'Add new games in groups': 'Añadir los juegos nuevos por grupos',
    'Games per group': 'Juegos por grupo',
    'Smaller groups are gentler on the system when many games are added at once.':
      'Los grupos pequeños cargan menos el sistema cuando se añaden muchos juegos a la vez.',
    'Install add-ons (DLC)': 'Instalar DLC',
    "Also use pkgs in the console's /data/PS4Mount": 'Usar también los pkgs de /data/PS4Mount de la consola',
    "Pkgs sent to the console's storage are added from there, with no install to wait for. They still take internal space.":
      'Los pkgs enviados a la memoria de la consola se añaden desde ahí, sin esperar una instalación. Ocupan espacio interno.',
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
      '¿Detener PS4Mount? Los juegos no se podrán abrir hasta que se vuelva a cargar.'
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
