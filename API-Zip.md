# API — ZIP Archives

Extract ZIP archives on the SD card — useful for bundling game assets in a single file.

## picocalc.zip

### Functions

#### `picocalc.zip.list(zip_path)`
List the contents of a ZIP archive.

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
Extract a ZIP archive to a directory.

- **Parameters:**
  - `zip_path` (string): Path to the ZIP file
  - `dest_dir` (string): Destination directory
  - `progress_fn` (function, optional): Progress callback receiving `(done, total)`
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

### Sandbox

Extraction respects the app sandbox: `dest_dir` must be writable by the app — its `/data/<APP_ID>` directory, unless the `root-filesystem` requirement is granted in `app.json`.
