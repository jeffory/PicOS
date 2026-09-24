#include "app_identity.h"

#include "app_manifest.h"
#include "umm_malloc.h"

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
    if (s_identity) {
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
