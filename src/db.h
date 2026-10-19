/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * app.db / appinfo.db access. */
#pragma once

#include <stdint.h>

#include "pkg.h"

/* Read only: the system databases are written by the system alone. */
int db_is_registered(const char *title_id);
/* The APP_VER the system registered for title_id ("" when it has none), NULL when the
 * title is not registered. The PS4 refuses to start a game whose update has another
 * one ("APP_VER disagree"). */
const char *db_app_ver(const char *title_id);
