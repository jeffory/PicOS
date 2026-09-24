#include "app_identity.h"

#include "app_manifest.h"
#include "appconfig.h"
#include "umm_malloc.h"
#include "../drivers/sdcard.h"

#include <stdio.h>
#include <string.h>

static app_identity_t *s_identity = NULL;

// Copy at most n-1 bytes (the display name is deliberately truncated).
static void copy_trunc(char *dst, size_t n, const char *src) {
    size_t len = strnlen(src, n - 1);
    memcpy(dst, src, len);
    dst[len] = '\0';
}

bool app_identity_begin(const app_entry_t *app) {
    app_identity_end();  // never inherit a previous app's identity
    if (!app || !app_manifest_id_valid(app->id))
        return false;
    app_identity_t *me = (app_identity_t *)umm_malloc(sizeof(*me));
    if (!me)
        return false;
    memset(me, 0, sizeof(*me));
    copy_trunc(me->id, sizeof(me->id), app->id);
    copy_trunc(me->dir, sizeof(me->dir), app->path);
    snprintf(me->data_dir, sizeof(me->data_dir), "/data/%s", me->id);
    copy_trunc(me->name, sizeof(me->name), app->name);
    copy_trunc(me->requirements, sizeof(me->requirements), app->requirements);
    me->is_native = (app->type == APP_TYPE_NATIVE);
    me->root_fs = app->has_root_filesystem;
    me->http = app->has_http;
    me->audio = app->has_audio;
    s_identity = me;
    return true;
}

void app_identity_end(void) {
    appconfig_unbind();
    if (s_identity) {
        // Normally empty by now (the runner closes leftovers after the app
        // is torn down); never let a file outlive its app's identity.
        int n = app_files_close_all();
        if (n)
            printf("[APP] closed %d file(s) left open by '%s'\n", n,
                   s_identity->name);
        umm_free(s_identity);
        s_identity = NULL;
    }
}

const app_identity_t *app_identity_current(void) {
    return s_identity;
}

bool app_identity_has_requirement(const char *name) {
    return s_identity && app_requirements_has(s_identity->requirements, name);
}

// ── Per-app open files ───────────────────────────────────────────────────────

bool app_files_track(void *f) {
    if (!s_identity || !f || s_identity->n_open_files >= APP_FILES_MAX)
        return false;
    s_identity->open_files[s_identity->n_open_files++] = f;
    return true;
}

bool app_files_untrack(void *f) {
    if (!s_identity || !f)
        return false;
    for (int i = 0; i < s_identity->n_open_files; i++) {
        if (s_identity->open_files[i] == f) {
            s_identity->open_files[i] =
                s_identity->open_files[--s_identity->n_open_files];
            s_identity->open_files[s_identity->n_open_files] = NULL;
            return true;
        }
    }
    return false;
}

int app_files_close_all(void) {
    if (!s_identity)
        return 0;
    int n = s_identity->n_open_files;
    // Detach the list first: sdcard_fclose must never see a handle twice.
    s_identity->n_open_files = 0;
    for (int i = 0; i < n; i++) {
        sdcard_fclose((sdfile_t)s_identity->open_files[i]);
        s_identity->open_files[i] = NULL;
    }
    return n;
}

int app_files_open_count(void) {
    return s_identity ? s_identity->n_open_files : 0;
}
