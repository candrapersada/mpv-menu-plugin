// Copyright (c) 2023 tsl0922. All rights reserved.
// SPDX-License-Identifier: GPL-2.0-only

#include "misc/bstr.h"
#include "menu.h"

#define MENU_PREFIX "#menu:"
#define MENU_PREFIX_UOSC "#!"
#define MENU_PREFIX_DYN "#@"

typedef struct dyn_item {
    HMENU hmenu;       // submenu handle
    UINT id;           // menu command id
    void *talloc_ctx;  // talloc context
    void (*update)(mp_state *state, struct dyn_item *item);
} dyn_entry;

typedef struct {
    dyn_entry *entries;
    int num_entries;
} dyn_list;

typedef struct {
    char *keyword;  // keyword in menu title
    void (*update)(mp_state *state, dyn_entry *item);
} dyn_provider;

// forward declarations
static void update_video_track_menu(mp_state *state, dyn_entry *item);
static void update_audio_track_menu(mp_state *state, dyn_entry *item);
static void update_sub_track_menu(mp_state *state, dyn_entry *item);
static void update_sub_track_menu2(mp_state *state, dyn_entry *item);
static void update_chapter_menu(mp_state *state, dyn_entry *item);
static void update_edition_menu(mp_state *state, dyn_entry *item);
static void update_audio_device_menu(mp_state *state, dyn_entry *item);

// dynamic menu providers
static const dyn_provider dyn_providers[] = {
    {"tracks/video", update_video_track_menu},
    {"tracks/audio", update_audio_track_menu},
    {"tracks/sub", update_sub_track_menu},
    {"tracks/sub-secondary", update_sub_track_menu2},
    {"chapters", update_chapter_menu},
    {"editions", update_edition_menu},
    {"audio-devices", update_audio_device_menu},
};

// dynamic menu list
static dyn_list *dyn_menus = NULL;

static bool add_dyn_menu(void *talloc_ctx, HMENU hmenu, int id, bstr keyword) {
    for (int i = 0; i < ARRAYSIZE(dyn_providers); i++) {
        dyn_provider provider = dyn_providers[i];
        if (!bstr_equals0(keyword, provider.keyword)) continue;

        MP_TARRAY_APPEND(talloc_ctx, dyn_menus->entries, dyn_menus->num_entries,
                         (dyn_entry){
                             .hmenu = hmenu,
                             .id = id,
                             .talloc_ctx = talloc_new(talloc_ctx),
                             .update = provider.update,
                         });
        return true;
    }
    return false;
}

static HMENU find_submenu(HMENU hmenu, wchar_t *name, UINT *id) {
    MENUITEMINFOW mii;
    int count = GetMenuItemCount(hmenu);

    for (int i = 0; i < count; i++) {
        memset(&mii, 0, sizeof(mii));
        mii.cbSize = sizeof(mii);
        mii.fMask = MIIM_STRING;
        if (!GetMenuItemInfoW(hmenu, i, TRUE, &mii) || mii.cch == 0) continue;

        mii.cch++;
        wchar_t buf[mii.cch];
        mii.dwTypeData = buf;
        mii.fMask |= MIIM_ID | MIIM_SUBMENU;
        if (!GetMenuItemInfoW(hmenu, i, TRUE, &mii) || !mii.hSubMenu) continue;
        if (wcscmp(mii.dwTypeData, name) == 0) {
            if (id) *id = mii.wID;
            return mii.hSubMenu;
        }
    }
    return NULL;
}

// escape & to && for menu title
static wchar_t *escape_title(void *talloc_ctx, bstr title) {
    void *tmp = talloc_new(NULL);
    bstr left, rest;
    bstr escaped = bstr0(NULL);

    left = bstr_split(title, "&", &rest);
    while (rest.len > 0) {
        bstr_xappend(tmp, &escaped, left);
        bstr_xappend(tmp, &escaped, bstr0("&&"));
        left = bstr_split(rest, "&", &rest);
    }
    bstr_xappend(tmp, &escaped, left);

    wchar_t *ret = mp_from_utf8(talloc_ctx, bstrdup0(tmp, escaped));
    talloc_free(tmp);
    return ret;
}

// format title as name\tkey
static wchar_t *format_title(void *talloc_ctx, bstr name, bstr key) {
    void *tmp = talloc_new(NULL);
    bstr title = bstrdup(tmp, name);

    if (key.len > 0 && !bstr_equals0(key, "_")) {
        bstr_xappend(tmp, &title, bstr0("\t"));
        bstr_xappend(tmp, &title, key);
    }

    wchar_t *ret = escape_title(talloc_ctx, title);
    talloc_free(tmp);
    return ret;
}

static int append_menu(HMENU hmenu, UINT fMask, UINT fType, UINT fState,
                       wchar_t *title, HMENU submenu, void *data) {
    static UINT id = WM_USER + 100;
    MENUITEMINFOW mii = {0};

    mii.cbSize = sizeof(mii);
    mii.fMask = MIIM_ID | fMask;
    mii.wID = id++;

    if (fMask & MIIM_FTYPE) mii.fType = fType;
    if (fMask & MIIM_STATE) mii.fState = fState;
    if (fMask & MIIM_STRING) {
        mii.dwTypeData = title;
        mii.cch = wcslen(title);
    }
    if (fMask & MIIM_SUBMENU) mii.hSubMenu = submenu;
    if (fMask & MIIM_DATA) mii.dwItemData = (ULONG_PTR)data;

    return InsertMenuItemW(hmenu, -1, TRUE, &mii) ? mii.wID : -1;
}

static int append_seprator(HMENU hmenu) {
    return append_menu(hmenu, MIIM_FTYPE, MFT_SEPARATOR, 0, NULL, NULL, NULL);
}

static HMENU append_submenu(HMENU hmenu, wchar_t *title, int *id) {
    HMENU menu = find_submenu(hmenu, title, id);
    if (menu != NULL) return menu;

    menu = CreatePopupMenu();
    int wid =
        append_menu(hmenu, MIIM_STRING | MIIM_SUBMENU, 0, 0, title, menu, NULL);
    if (id) *id = wid;
    return menu;
}

static void update_track_menu(mp_state *state, dyn_entry *item,
                              const char *type, const char *prop, int64_t pos) {
    mp_track_list *list = state->track_list;
    if (list == NULL || list->num_entries == 0) return;

    int count = GetMenuItemCount(item->hmenu);

    for (int i = 0; i < list->num_entries; i++) {
        mp_track_item *entry = &list->entries[i];
        if (strcmp(entry->type, type) != 0) continue;

        UINT fState = entry->selected ? MFS_CHECKED : MFS_UNCHECKED;
        if (strcmp(type, "sub") == 0 && entry->selected && pos != entry->id)
            fState |= MFS_DISABLED;
        append_menu(
            item->hmenu, MIIM_STRING | MIIM_DATA | MIIM_STATE, 0, fState,
            format_title(item->talloc_ctx, bstr0(entry->title),
                         bstr0(entry->lang)),
            NULL,
            talloc_asprintf(item->talloc_ctx, "set %s %d", prop, entry->id));
    }

    if (GetMenuItemCount(item->hmenu) > count) {
        append_menu(item->hmenu, MIIM_STRING | MIIM_DATA | MIIM_STATE, 0,
                    pos < 0 ? MFS_CHECKED : MFS_UNCHECKED,
                    escape_title(item->talloc_ctx, bstr0("Off")), NULL,
                    talloc_asprintf(item->talloc_ctx, "set %s no", prop));
    }
}

static void update_video_track_menu(mp_state *state, dyn_entry *item) {
    update_track_menu(state, item, "video", "vid", state->vid);
}

static void update_audio_track_menu(mp_state *state, dyn_entry *item) {
    update_track_menu(state, item, "audio", "aid", state->aid);
}

static void update_sub_track_menu(mp_state *state, dyn_entry *item) {
    update_track_menu(state, item, "sub", "sid", state->sid);
}

static void update_sub_track_menu2(mp_state *state, dyn_entry *item) {
    update_track_menu(state, item, "sub", "secondary-sid", state->sid2);
}

static void update_chapter_menu(mp_state *state, dyn_entry *item) {
    mp_chapter_list *list = state->chapter_list;
    if (list == NULL || list->num_entries == 0) return;

    void *tmp = talloc_new(NULL);

    for (int i = 0; i < list->num_entries; i++) {
        mp_chapter_item *entry = &list->entries[i];
        const char *time =
            talloc_asprintf(tmp, "[%02d:%02d:%02d]", (int)entry->time / 3600,
                            (int)entry->time / 60 % 60, (int)entry->time % 60);
        append_menu(
            item->hmenu, MIIM_STRING | MIIM_DATA, 0, 0,
            format_title(item->talloc_ctx, bstr0(entry->title), bstr0(time)),
            NULL,
            talloc_asprintf(item->talloc_ctx, "seek %f absolute", entry->time));
    }
    if (state->chapter >= 0) {
        CheckMenuRadioItem(item->hmenu, 0, list->num_entries, state->chapter,
                           MF_BYPOSITION);
    }

    talloc_free(tmp);
}

static void update_edition_menu(mp_state *state, dyn_entry *item) {
    mp_edition_list *list = state->edition_list;
    if (list == NULL || list->num_entries == 0) return;

    int pos = -1;
    for (int i = 0; i < list->num_entries; i++) {
        mp_edition_item *entry = &list->entries[i];
        if (entry->id == state->edition) pos = i;
        append_menu(
            item->hmenu, MIIM_STRING | MIIM_DATA, 0, 0,
            escape_title(item->talloc_ctx, bstr0(entry->title)), NULL,
            talloc_asprintf(item->talloc_ctx, "set edition %d", entry->id));
    }
    if (pos >= 0) {
        CheckMenuRadioItem(item->hmenu, 0, list->num_entries, pos,
                           MF_BYPOSITION);
    }
}

static void update_audio_device_menu(mp_state *state, dyn_entry *item) {
    mp_audio_device_list *list = state->audio_device_list;
    if (list == NULL || list->num_entries == 0) return;

    void *tmp = talloc_new(NULL);

    char *name = state->audio_device;
    int pos = -1;
    for (int i = 0; i < list->num_entries; i++) {
        mp_audio_device *entry = &list->entries[i];
        if (strcmp(entry->name, name) == 0) pos = i;
        char *title = entry->desc;
        if (title == NULL || strlen(title) == 0)
            title = talloc_strdup(tmp, entry->name);
        append_menu(item->hmenu, MIIM_STRING | MIIM_DATA, 0, 0,
                    escape_title(item->talloc_ctx, bstr0(title)), NULL,
                    talloc_asprintf(item->talloc_ctx, "set audio-device %s",
                                    entry->name));
    }
    if (pos >= 0) {
        CheckMenuRadioItem(item->hmenu, 0, list->num_entries, pos,
                           MF_BYPOSITION);
    }

    talloc_free(tmp);
}

static void dyn_menu_init(void *talloc_ctx) {
    dyn_menus = talloc_zero(talloc_ctx, dyn_list);
}

static void dyn_menu_update(plugin_ctx *ctx) {
    if (dyn_menus == NULL) return;

    for (int i = 0; i < dyn_menus->num_entries; i++) {
        dyn_entry *item = &dyn_menus->entries[i];

        // clear menu
        while (GetMenuItemCount(item->hmenu) > 0)
            RemoveMenu(item->hmenu, 0, MF_BYPOSITION);
        talloc_free_children(item->talloc_ctx);

        item->update(ctx->state, item);

        // update state
        int count = GetMenuItemCount(item->hmenu);
        UINT enable = count > 0 ? MF_ENABLED : MF_GRAYED;
        EnableMenuItem(ctx->hmenu, item->id, MF_BYCOMMAND | enable);
    }
}

static bool is_seprarator(bstr text, bool uosc) {
    return bstr_equals0(text, "-") || (uosc && bstr_startswith0(text, "---"));
}

static void parse_menu(void *talloc_ctx, HMENU hmenu, bstr key, bstr cmd,
                       bstr text, bool uosc) {
    bstr name, rest, comment;

    name = bstr_split(text, ">", &rest);
    name = bstr_split(name, "#", &comment);
    name = bstr_strip(name);
    if (!name.len) return;

    if (!rest.len) {
        if (is_seprarator(name, uosc)) {
            append_seprator(hmenu);
        } else {
            int id = 0;
            if (bstr_eatstart0(&comment, MENU_PREFIX_DYN)) {
                HMENU submenu =
                    append_submenu(hmenu, escape_title(talloc_ctx, name), &id);
                if (id > 0 && comment.len > 0) {
                    bstr keyword = bstr_split(comment, "#", NULL);
                    keyword = bstr_rstrip(keyword);
                    add_dyn_menu(talloc_ctx, submenu, id, keyword);
                }
            } else {
                id = append_menu(hmenu, MIIM_STRING | MIIM_DATA, 0, 0,
                                 format_title(talloc_ctx, name, key), NULL,
                                 bstrdup0(talloc_ctx, cmd));
                if (id > 0 && (!cmd.len || bstr_startswith0(cmd, "#")))
                    EnableMenuItem(hmenu, id, MF_BYCOMMAND | MF_GRAYED);
            }
        }
    } else {
        HMENU submenu =
            append_submenu(hmenu, escape_title(talloc_ctx, name), NULL);
        if (!comment.len) parse_menu(talloc_ctx, submenu, key, cmd, rest, uosc);
    }
}

static bool split_menu(bstr line, bstr *left, bstr *right, bool uosc) {
    if (!line.len) return false;
    if (!bstr_split_tok(line, MENU_PREFIX, left, right)) {
        if (!uosc || !bstr_split_tok(line, MENU_PREFIX_UOSC, left, right))
            return false;
    }
    *left = bstr_strip(*left);
    *right = bstr_strip(*right);
    return right->len > 0;
}

HMENU load_menu(plugin_ctx *ctx) {
    dyn_menu_init(ctx);

    void *tmp = talloc_new(NULL);
    char *path = mp_get_prop_string(tmp, "input-conf");
    if (path == NULL || strlen(path) == 0) path = "~~/input.conf";

    HMENU hmenu = CreatePopupMenu();
    bstr data = bstr0(mp_read_file(tmp, path));

    while (data.len > 0) {
        bstr line = bstr_strip_linebreaks(bstr_getline(data, &data));
        line = bstr_lstrip(line);
        if (!line.len) continue;

        bstr key, cmd, left, right;
        if (bstr_eatstart0(&line, "#")) {
            if (!ctx->conf->uosc) continue;
            key = bstr0(NULL);
            cmd = bstr_strip(line);
        } else {
            key = bstr_split(line, WHITESPACE, &cmd);
            cmd = bstr_strip(cmd);
        }
        if (split_menu(cmd, &left, &right, ctx->conf->uosc))
            parse_menu(ctx, hmenu, key, cmd, right, ctx->conf->uosc);
    }

    talloc_free(tmp);

    return hmenu;
}

void show_menu(plugin_ctx *ctx, POINT *pt) {
    RECT rc;
    GetClientRect(ctx->hwnd, &rc);
    ScreenToClient(ctx->hwnd, pt);
    if (!PtInRect(&rc, *pt)) return;

    dyn_menu_update(ctx);

    ClientToScreen(ctx->hwnd, pt);
    TrackPopupMenuEx(ctx->hmenu, TPM_LEFTALIGN | TPM_LEFTBUTTON, pt->x, pt->y,
                     ctx->hwnd, NULL);
}

void handle_menu(plugin_ctx *ctx, int id) {
    MENUITEMINFOW mii = {0};
    mii.cbSize = sizeof(mii);
    mii.fMask = MIIM_DATA;
    if (!GetMenuItemInfoW(ctx->hmenu, id, FALSE, &mii)) return;

    if (mii.dwItemData) mp_command_async((const char *)mii.dwItemData);
}