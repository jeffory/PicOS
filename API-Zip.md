# API Zip

Read and extract ZIP archives on the SD card. Useful for shipping an app's assets as a single file: transfer one archive instead of hundreds of loose files, then extract it once, or open it and read entries straight out of it at runtime.

## picocalc.zip

### Functions

#### `picocalc.zip.list(zip_path)`
List the file entries of a ZIP archive (directory entries are skipped).

- **Parameters:**
  - `zip_path` (string): Path to the ZIP file
- **Returns:** (table or nil, string) Array of `{name, size, compressed_size}` entries, or `nil, errorString` on failure

```lua
local entries, err = picocalc.zip.list("/apps/mygame/assets.zip")
if entries then
    for _, e in ipairs(entries) do
        picocalc.sys.log(e.name .. " (" .. e.size .. " bytes)")
    end
end
```

---

#### `picocalc.zip.extract(zip_path, dest_dir [, progress_fn])`
Extract every file in a ZIP archive to a directory. Parent directories are created as needed. Entries with unsafe names are skipped rather than failing the whole extraction (see [Limits and name validation](#limits-and-name-validation)).

- **Parameters:**
  - `zip_path` (string): Path to the ZIP file
  - `dest_dir` (string): Destination directory
  - `progress_fn` (function, optional): Progress callback receiving `(done, total)` after each extracted file
- **Returns:** (boolean, string) `true` on success, or `false, errorString` on failure

```lua
local ok, err = picocalc.zip.extract("/apps/mygame/assets.zip", "/data/com.example.mygame/assets",
    function(done, total)
        picocalc.sys.log("extracting " .. done .. "/" .. total)
    end)
if not ok then
    picocalc.sys.log("extract failed: " .. err)
end
```

---

#### `picocalc.zip.open(path)`
Open an archive for random access without extracting it. Entries can then be listed, tested, read into memory, or streamed to files through the returned archive object.

At most **4 archives** may be open at once per app. An archive is closed by `:close()`, by the garbage collector, when it leaves a to-be-closed scope (`local ar <close> = ...`), and automatically when the app exits.

- **Parameters:**
  - `path` (string): Path to the ZIP file
- **Returns:** (userdata or nil, string) Archive object, or `nil, errorString` on failure

```lua
local ar, err = picocalc.zip.open(APP_DIR .. "/assets.zip")
if not ar then error(err) end
```

---

### Archive Methods

Objects returned by `picocalc.zip.open()`. Calling any method on a closed archive raises an error, except `:close()`, which is a no-op when already closed.

#### `ar:list()`
List the archive's file entries (directory entries are skipped). Same result shape as `picocalc.zip.list`.

- **Parameters:** None
- **Returns:** (table) Array of `{name, size, compressed_size}` entries

```lua
for _, e in ipairs(ar:list()) do
    picocalc.sys.log(e.name)
end
```

---

#### `ar:exists(name)`
Check whether an entry with this exact name exists. Names include any directory prefix inside the archive (for example `"images/hero.png"`).

- **Parameters:**
  - `name` (string): Entry name
- **Returns:** (boolean) `true` if the entry exists

```lua
if ar:exists("images/hero.png") then ... end
```

---

#### `ar:size(name)`
Get an entry's uncompressed size.

- **Parameters:**
  - `name` (string): Entry name
- **Returns:** (number or nil) Uncompressed size in bytes, or `nil` if the entry does not exist

```lua
local bytes = ar:size("music/theme.mod")
```

---

#### `ar:read(name [, max_len])`
Decompress a whole entry into a Lua string, without touching the SD card's filesystem. Fails if the entry's uncompressed size exceeds `max_len` (when given) or the 4 MB in-memory cap.

- **Parameters:**
  - `name` (string): Entry name
  - `max_len` (number, optional): Reject entries that decompress to more than this many bytes
- **Returns:** (string or nil, string) Entry contents, or `nil, errorString` on failure

```lua
local data, err = ar:read("levels/level1.json")
if data then
    local level = picocalc.json.decode(data)
end
```

---

#### `ar:extract(name, dest_path)`
Stream one entry to a file on the SD card, using constant memory regardless of entry size. The entry name only selects the data; the destination path is entirely caller-chosen (subject to the write sandbox), so no entry-name validation applies here.

- **Parameters:**
  - `name` (string): Entry name
  - `dest_path` (string): Destination file path (parent directories are created as needed)
- **Returns:** (boolean, string) `true` on success, or `false, errorString` on failure

```lua
local ok, err = ar:extract("music/theme.mod", "/data/com.example.mygame/theme.mod")
```

---

#### `ar:extractAll(dest_dir [, progress_fn])`
Extract every file entry into a directory. Same behaviour as `picocalc.zip.extract`, reusing the already-open handle.

- **Parameters:**
  - `dest_dir` (string): Destination directory
  - `progress_fn` (function, optional): Progress callback receiving `(done, total)` after each extracted file
- **Returns:** (boolean, string) `true` on success, or `false, errorString` on failure

```lua
local ok, err = ar:extractAll("/data/com.example.mygame/assets")
```

---

#### `ar:close()`
Close the archive and release its SD file handle. Closing an already-closed archive is a no-op. Also called automatically by the garbage collector and on `<close>` scope exit.

- **Parameters:** None
- **Returns:** None

```lua
ar:close()
```

---

### Example

An asset bundle: `assets.zip` sits next to `main.lua` and holds the app's images. Open the archive once at app start, decode images on demand straight from it (no extraction to SD), and close it at exit.

```lua
local image = picocalc.graphics.image

-- Open once at app start
local ar = assert(picocalc.zip.open(APP_DIR .. "/assets.zip"))

-- Read entries on demand; loadFromBuffer decodes from memory
local function loadSprite(name)
    local data, err = ar:read("images/" .. name)
    if not data then return nil, err end
    return image.loadFromBuffer(data)
end

local hero = assert(loadSprite("hero.png"))

-- ... main loop ...

-- Release the SD file handle at exit
ar:close()
```

Keep the archive open for the app's lifetime instead of re-opening it per read: `open` pays the central-directory walk once, and each later read seeks straight to its entry.

---

### Sandbox

All paths respect the app sandbox:

- `picocalc.zip.open`, `picocalc.zip.list` and the `zip_path` of `picocalc.zip.extract` need **read** access to the archive.
- The destinations of `picocalc.zip.extract`, `ar:extract` and `ar:extractAll` need **write** access.

By default an app can read `/apps/<dirname>` and read/write `/data/<APP_ID>`; the `root-filesystem` requirement in `app.json` lifts this to the whole SD card.

---

### Limits and name validation

Hard caps, enforced by the shared ZIP engine:

| Limit | Value |
|-------|-------|
| Entries per archive | 8192 |
| Total uncompressed size | 256 MB |
| Single in-memory read (`ar:read`) | 4 MB |
| Entry name length | 255 characters |

`picocalc.zip.extract` and `ar:extractAll` check the entry-count and total-uncompressed-size caps up front and fail fast, before writing anything (zip-bomb protection).

Entry names are strictly validated before an entry name may become a filesystem path. Rejected: leading `/`, any backslash or `:`, control bytes, and any path component that is exactly `.` or `..` (a name like `foo..bar` is fine). During `extractAll`, entries with invalid names are skipped and counted; they do not fail the extraction. `ar:extract(name, dest)` skips this check because the entry name never becomes a path there.

---

### Performance notes

- The reader is seek-based: the archive is streamed from the SD card, so opening costs one pass over the central directory, not the archive size, and the archive is never copied to the heap.
- Extraction (`extract`, `ar:extract`, `ar:extractAll`) streams with constant memory, whatever the entry size.
- The SD bus is shared with Core 1 audio (MP3 and file player). Pace big `ar:read` calls: decompressing a multi-megabyte entry holds the SD bus long enough to starve audio decode, so do large reads at load screens, not mid-gameplay.

---

### Native API (C)

`g_api.zip` (`picocalc_zip_t` in `src/os/os.h`). `extract(zip_path, dest_dir)` and `list(zip_path)` exist since Phase 2. API version 5 (`g_api.version >= 5`) appends read-in-place archive handles: `open`, `close`, `numEntries`, `locate`, `statIndex`, `read` and `extractEntry`, working on an opaque `pczip_t` handle. Check `api->version >= 5` before calling them; on older firmware the struct ends at `list`.

Reads are caller-allocated: `statIndex` first to size the buffer, then `read` decompresses into it. `read` returns the number of bytes written, or -1 on any error, including "entry larger than `buf_cap`".

```c
pczip_t z = api->zip->open("/apps/mygame/assets.zip");   // NULL on error
if (!z) return;

int idx = api->zip->locate(z, "images/hero.png");        // -1 if absent
pczip_stat_t st;
if (idx >= 0 && api->zip->statIndex(z, idx, &st)) {
    void *buf = umm_malloc(st.size);                     // caller allocates
    if (buf) {
        int got = api->zip->read(z, idx, buf, st.size);  // bytes written, -1 on error
        if (got >= 0) {
            // ... use buf[0..got) ...
        }
        umm_free(buf);
    }
}

// Or stream an entry to the SD card with constant memory:
api->zip->extractEntry(z, idx, "/data/com.example.mygame/hero.png");

api->zip->close(z);
```

The same limits apply as in Lua: at most 4 open handles per app (force-closed when the app exits), 8192 entries, 256 MB total uncompressed. `pczip_stat_t` carries `name`, `size` (uncompressed), `comp_size` and `is_dir`; `numEntries` counts files plus directories, so skip `is_dir` entries when iterating file content.
