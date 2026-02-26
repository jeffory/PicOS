# File Manager - Missing SDK Features

All items below have now been implemented in the SDK (sdcard driver +
lua_bridge_fs/ui).  This file is kept for reference only.

---

## ✅ Implemented

### `picocalc.fs.delete(path) → ok [, err]`
- Wraps `f_unlink()`.  Returns `true` on success, or `false, "delete failed"`.

### `picocalc.fs.rename(src, dst) → ok [, err]`
- Wraps `f_rename()`.  Sandbox-checked on both src (read) and dst (write).

### `picocalc.fs.copy(src, dst [, progress_fn]) → ok [, err]`
- Native C 4 KB-chunk copy.  Optional `progress_fn(bytes_done, total_bytes)`
  called after each chunk.

### `picocalc.fs.stat(path) → {size, is_dir, year, month, day, hour, min, sec}`
- Single-path metadata lookup via `f_stat()`.  Returns `nil, err` on failure.

### `picocalc.fs.diskInfo() → {free, total}` (values in KB)
- Wraps `f_getfree()`.  Returns `nil, err` if disk info is unavailable.

### `picocalc.fs.glob(path, pattern) → [{name, is_dir, size, year, …}]`
- `*` and `?` wildcards, case-insensitive.

### `listDir` modification-time fields
- `sdcard_entry_t` now carries `fdate`/`ftime`.  Each entry table from
  `picocalc.fs.listDir()` now includes `year`, `month`, `day`, `hour`,
  `min`, `sec` (decoded from FatFS packed fields; absent if timestamp is 0).

### `picocalc.ui.textInput([prompt [, default]]) → string | nil`
- Blocking text-input dialog overlay.  Enter=confirm, Esc=cancel.
  Scrolls horizontally for inputs longer than the visible field width.

### `picocalc.ui.confirm(message) → bool`
- Blocking yes/no dialog overlay.  Enter/Y=yes, Esc/N=no.
  Message is word-wrapped over up to two lines.

---

## Nice-to-have (still outstanding)

### Progress callback for `listDir` / large scans
- Not needed now that native `copy` has a progress callback.

### `picocalc.fs.glob` recursive search
- Current implementation only searches a single directory.
  A future `glob(path, pattern, recursive)` flag would be useful.
