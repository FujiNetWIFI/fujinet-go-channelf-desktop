/* Menu command, control and timer IDs, and the icon.
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#define IDI_APPICON        101

/* Machine menu */
#define IDM_OPEN           1001
#define IDM_EJECT          1002
#define IDM_IMPORT_SD      1003
#define IDM_PAUSE          1006
#define IDM_RESET          1007
#define IDM_RESET_CONFIG   1008
#define IDM_SETTINGS       1009
#define IDM_EXIT           1010
/* View menu */
#define IDM_CONTROLLERS    1020
#define IDM_DEBUGGER       1021
#define IDM_TV_ASPECT      1022
#define IDM_SMOOTH         1023
#define IDM_FULLSCREEN     1024
/* FujiNet and Help menus */
#define IDM_FUJINET_CONFIG 1030
#define IDM_FUJINET_LOG    1031
#define IDM_ABOUT          1040

/* The main window's status bar */
#define IDC_STATUSBAR      2003
#define IDC_LOG_EDIT       2004

/* Timers */
#define IDT_LOG_REFRESH    1
#define IDT_STATUS         2
#define IDT_SYSACT         3
