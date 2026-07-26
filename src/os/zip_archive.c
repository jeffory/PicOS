#include "zip_archive.h"

#include <stdio.h>
#include <string.h>

// Redirect miniz allocations to PSRAM (umm_malloc), not the tiny SRAM heap
#define MZ_MALLOC(x)     umm_malloc(x)
#define MZ_FREE(x)       umm_free(x)
#define MZ_REALLOC(p, x) umm_realloc(p, x)
#include "miniz.h"
#include "../drivers/sdcard.h"
#include "umm_malloc.h"

bool zip_archive_extract(const char *zip_path, const char *dest_dir) {
    int zip_len = 0;
    char *zip_data = sdcard_read_file(zip_path, &zip_len);
    if (!zip_data) return false;

    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_mem(&zip, zip_data, (size_t)zip_len, 0)) {
        umm_free(zip_data);
        return false;
    }

    bool ok = true;
    int n = (int)mz_zip_reader_get_num_files(&zip);
    for (int i = 0; i < n && ok; i++) {
        if (mz_zip_reader_is_file_a_directory(&zip, (mz_uint)i)) continue;
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, (mz_uint)i, &st)) { ok = false; break; }

        char path[256];
        snprintf(path, sizeof(path), "%s/%s", dest_dir, st.m_filename);

        size_t uncomp = 0;
        void *data = mz_zip_reader_extract_to_heap(&zip, (mz_uint)i, &uncomp, 0);
        if (!data) { ok = false; break; }

        sdcard_mkdir(dest_dir);  // ensure dest exists
        sdfile_t f = sdcard_fopen(path, "w");
        if (f) {
            sdcard_fwrite(f, data, (int)uncomp);
            sdcard_fclose(f);
        } else { ok = false; }
        mz_free(data);
    }

    mz_zip_reader_end(&zip);
    umm_free(zip_data);
    return ok;
}

int zip_archive_list(const char *zip_path) {
    int zip_len = 0;
    char *zip_data = sdcard_read_file(zip_path, &zip_len);
    if (!zip_data) return -1;

    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_mem(&zip, zip_data, (size_t)zip_len, 0)) {
        umm_free(zip_data);
        return -1;
    }
    int n = (int)mz_zip_reader_get_num_files(&zip);
    mz_zip_reader_end(&zip);
    umm_free(zip_data);
    return n;
}
