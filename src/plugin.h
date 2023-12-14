// Copyright (c) 2023 tsl0922. All rights reserved.
// SPDX-License-Identifier: GPL-2.0-only

#ifndef MPV_PLUGIN_H
#define MPV_PLUGIN_H

#include <windows.h>
#include <mpv/client.h>

#include "osdep/threads.h"
#include "misc/dispatch.h"

struct plugin_ctx {
    mp_thread thread;
    struct mp_dispatch_queue *dispatch;  // dispatch queue
    bool terminate;                      // terminate thread

    mpv_handle *mpv;     // mpv client handle
    wchar_t *conf_path;  // path to input.conf

    HWND hwnd;         // window handle
    HMENU hmenu;       // menu handle
    WNDPROC wnd_proc;  // previous window procedure
};

void mp_command_async(const char *args);

#endif