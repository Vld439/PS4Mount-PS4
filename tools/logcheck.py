#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Reads PS4Mount's log and says, in plain words, what happened and what to do.

    python3 tools/logcheck.py log.txt [log.txt.old]        (Spanish)
    python3 tools/logcheck.py --lang en log.txt
    curl -s http://<console-ip>:8770/api/log | python3 tools/logcheck.py -

Get log.txt from the console over FTP (/data/ps4mount/log.txt, port 2121) or from the
panel (/api/log). It only reads: it changes nothing, and it never sends the log anywhere.
Every line of the log is "date time [tag] text"; the signatures below come from the
meanings in AI-WORKFLOW.md (sections 4 and 5).
"""
import argparse
import collections
import re
import sys

LINE = re.compile(r"^(?:(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d)\.\d+ )?(?:\[([a-z0-9_]+)\] )?(.*)$")

# (id, pattern, level, Spanish, English). Level: ERROR stops something working, WARN is a
# thing to look at, INFO is the project telling what it did.
SIGNATURES = [
    ("content_url", r"invalid content_url", "ERROR",
     "El instalador del sistema (BGFT) rechazó la ruta del pkg. Pasa si la ruta es demasiado larga (más de 95 bytes) o pasa por /data en vez de /user/data. Mueve el pkg a una carpeta con nombre corto.",
     "The system's installer (BGFT) refused the pkg's path: too long (over 95 bytes) or through /data instead of /user/data. Move the pkg to a folder with a short name."),
    ("no_task", r"0x80990019", "ERROR",
     "El sistema no tiene la tarea del juego. Reinicia la consola: la lee solo al arrancar.",
     "The system has no task for the game. Restart the console: it reads tasks only at boot."),
    ("blockmap", r"0x8002000c", "ERROR",
     "El sistema no pudo construir el mapa de bloques del pkg: pasa cuando el pkg se alcanza por el alias /data. PS4Mount lo evita; si lo ves, es un fallo: guarda el log.",
     "The system could not build the pkg's block map: it happens when the pkg is reached through the /data alias. PS4Mount avoids it; if you see it, it is a bug: keep the log."),
    ("svc_open", r"0x80990001", "WARN",
     "Otro payload dejó abierto el servicio de instalación (BGFT). PS4Mount lo cierra y lo abre otra vez; si se repite, reinicia la consola.",
     "Another payload left the installer service (BGFT) open. PS4Mount closes and opens it again; if it repeats, restart the console."),
    ("app_ver", r"APP_VER disagree", "WARN",
     "La versión de la actualización no coincide con la del juego registrado. Esa actualización se deja fuera hasta que cambie el pkg.",
     "The update's version differs from the registered game's. That update is left out until the pkg changes."),
    ("refused_update", r"the system refuses update", "WARN",
     "El sistema rechaza una actualización: se deja fuera hasta que cambie su pkg (el juego base sigue funcionando).",
     "The system refuses an update: it is left out until its pkg changes (the base game keeps working)."),
    ("erofs", r"EROFS|read-only file system", "ERROR",
     "El sistema intentó escribir en el pkg y no pudo (solo lectura). El juego no arranca. Es el fallo que PS4Mount evita: no uses una vista de solo lectura de los pkgs.",
     "The system tried to write to the pkg and could not (read-only). The game will not start. PS4Mount avoids this: do not use a read-only view of the pkgs."),
    ("playgo", r"playgo-chunk\.dat unusable", "WARN",
     "Un archivo de datos de instalación (PlayGo) del juego no sirve. Se vuelve a crear al registrar el juego otra vez.",
     "A game's install data file (PlayGo) is unusable. It is made again when the game is registered again."),
    ("license", r"no license\.dat", "WARN",
     "Al pkg le falta su archivo de licencia (license.dat): el juego puede no abrir.",
     "The pkg has no license file (license.dat): the game may not open."),
    ("task_id", r"no free BGFT task id", "ERROR",
     "No había un identificador libre para la tarea del juego. Prueba a quitar de la biblioteca juegos que no uses y a reiniciar la consola.",
     "There was no free id for the game's task. Try removing games you do not use from the library and restarting the console."),
    ("not_registered", r"not registered by the system", "ERROR",
     "El sistema no registró el juego. Mira las líneas [reg] anteriores y reinicia la consola.",
     "The system did not register the game. Read the [reg] lines before it and restart the console."),
    ("appdb", r"app\.db could not be read", "WARN",
     "No se pudo leer la base de datos de apps del sistema (solo lectura): el escaneo espera y vuelve a intentarlo.",
     "The system's app database could not be read (read-only): the scan waits and tries again."),
    ("grant", r"invalid_grant", "WARN",
     "Google terminó el acceso de Drive (pasa con apps en modo prueba). PS4Mount pide un código nuevo en pantalla.",
     "Google ended the Drive access (apps in testing do). PS4Mount asks for a new code on screen."),
    ("memory", r"no memory", "ERROR",
     "Se quedó sin memoria (el payload dispone de muy poca). Reinicia la consola y guarda el log.",
     "Out of memory (the payload has very little). Restart the console and keep the log."),
    ("running", r"game running", "INFO",
     "Una acción se negó porque el juego estaba en marcha: ciérralo y repite.",
     "An action was refused because the game was running: close it and try again."),
]
GENERIC = re.compile(r"could not|cannot|failed|refus|unusable|unable", re.I)

TEXT = {
    "es": {
        "file": "Archivo", "lines": "líneas", "span": "Desde {a} hasta {b}",
        "version": "Versión y consola", "none": "(no aparece: ¿es un log cortado?)",
        "tags": "Líneas por módulo", "problems": "Qué hay que mirar",
        "clean": "Archivos temporales que PS4Mount borró de la memoria interna",
        "clean_none": "Ninguno.", "clean_n": "{n} aviso(s) [clean], {b} bytes en total ({mb} MB)",
        "other": "Otros avisos sin explicación conocida (primeros {n})",
        "ok": "Resultado: sin problemas conocidos.",
        "bad": "Resultado: {e} error(es) y {w} aviso(s) conocidos. Léelos arriba.",
        "x": "{n} vez/veces", "first": "primera línea", "stops": "Paradas y arranques",
        "starts": "{n} arranque(s) de PS4Mount en este log",
    },
    "en": {
        "file": "File", "lines": "lines", "span": "From {a} to {b}",
        "version": "Version and console", "none": "(not there: a cut log?)",
        "tags": "Lines by module", "problems": "What to look at",
        "clean": "Temporary files PS4Mount deleted from internal storage",
        "clean_none": "None.", "clean_n": "{n} [clean] notice(s), {b} bytes in all ({mb} MB)",
        "other": "Other warnings with no known explanation (first {n})",
        "ok": "Result: no known problems.",
        "bad": "Result: {e} known error(s) and {w} warning(s). Read them above.",
        "x": "{n} time(s)", "first": "first line", "stops": "Starts and stops",
        "starts": "{n} start(s) of PS4Mount in this log",
    },
}


def parse(lines):
    for raw in lines:
        raw = raw.rstrip("\n")
        m = LINE.match(raw)
        if m is None:  # cannot happen: every line matches, but say what to do if it does
            yield raw, None, None, raw
            continue
        yield raw, m.group(1), m.group(2), m.group(3)


def analyze(paths, lang):
    t = TEXT[lang]
    idx = 1 if lang == "es" else 2
    rows = []
    for p in paths:
        fh = sys.stdin if p == "-" else open(p, encoding="utf-8", errors="replace")
        with fh:
            rows.extend(parse(fh))
    out = []
    out.append(f"{t['file']}: {', '.join(paths)}  ({len(rows)} {t['lines']})")
    stamps = [s for _, s, _, _ in rows if s]
    if stamps:
        out.append(t["span"].format(a=stamps[0], b=stamps[-1]))

    out.append("")
    out.append(f"== {t['version']}")
    info = [r for r, _, tag, body in rows if tag == "main" and re.match(r"(firmware|console language|start)", body)]
    out.extend("  " + re.sub(r"^\S+ \S+ ", "", r) for r in info[:6]) if info else out.append("  " + t["none"])
    starts = sum(1 for _, _, tag, body in rows if tag == "main" and body.startswith("start "))
    out.append("  " + t["starts"].format(n=starts))

    out.append("")
    out.append(f"== {t['tags']}")
    tags = collections.Counter(tag or "-" for _, _, tag, _ in rows)
    out.append("  " + "  ".join(f"[{k}] {v}" for k, v in tags.most_common()))

    out.append("")
    out.append(f"== {t['problems']}")
    known = set()
    errors = warns = 0
    for _, pat, level, es, en in SIGNATURES:
        hits = [i for i, (_, _, _, body) in enumerate(rows) if re.search(pat, body)]
        if not hits:
            continue
        known.update(hits)
        if level == "ERROR":
            errors += 1
        elif level == "WARN":
            warns += 1
        first = re.sub(r"^\S+ \S+ ", "", rows[hits[0]][0])[:160]
        out.append(f"  [{level}] {(es, en)[idx - 1]}")
        out.append(f"        {t['x'].format(n=len(hits))}; {t['first']}: {first}")
    if not known:
        out.append("  -")
    others = [i for i, (_, _, tag, body) in enumerate(rows)
              if i not in known and tag != "clean" and GENERIC.search(body)]
    if others:
        out.append("")
        out.append(f"== {t['other'].format(n=min(len(others), 10))}")
        out.extend("  " + re.sub(r"^\S+ \S+ ", "", rows[i][0])[:180] for i in others[:10])

    out.append("")
    out.append(f"== {t['clean']}")
    cl = [body for _, _, tag, body in rows if tag == "clean"]
    if cl:
        total = 0
        for body in cl:
            m = re.search(r"\((\d+) bytes\)", body)
            total += int(m.group(1)) if m else 0
        out.append("  " + t["clean_n"].format(n=len(cl), b=total, mb=round(total / 1048576, 1)))
    else:
        out.append("  " + t["clean_none"])

    out.append("")
    out.append(t["ok"] if not (errors or warns or others) else t["bad"].format(e=errors, w=warns + len(others)))
    return "\n".join(out) + "\n", errors


def main():
    ap = argparse.ArgumentParser(description="Explains PS4Mount's log in plain words.")
    ap.add_argument("logs", nargs="+", help="log.txt (and log.txt.old); - reads the standard input")
    ap.add_argument("--lang", choices=["es", "en"], default="es")
    a = ap.parse_args()
    text, errors = analyze(a.logs, a.lang)
    sys.stdout.write(text)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
