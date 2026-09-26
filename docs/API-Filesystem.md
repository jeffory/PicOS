# API Filesystem

Filesystem access to the SD card (FAT32).

## picocalc.fs

### Functions

#### `picocalc.fs.open(path [, mode])`
Opens a file on the SD card.

- **Parameters:**
  - `path` (string): Absolute file path (e.g., `"/apps/hello/data.txt"`)
  - `mode` (string, optional): File mode (`"r"`, `"w"`, `"a"`, `"rb"`, `"wb"`, etc.). Defaults to `"r"`.
- **Returns:** a file handle (userdata), or `nil, err` (`"permission denied"`, `"cannot open file"`, `"too many open files"`)

Handles have methods — `f:read(n)`, `f:write(s)`, `f:seek(pos)`, `f:tell()`,
`f:close()` — identical to the `picocalc.fs.*` functions. A handle closes
itself when garbage-collected, and `local f <close> = picocalc.fs.open(...)`
closes it at the end of the scope. Using a closed handle raises "attempt to
use a closed file"; `close` is idempotent. At most 16 files can be open at
once (FatFS), and files still open when the app exits are closed by the OS.

```lua
local f = picocalc.fs.open("/data/save.txt", "w")
if f then
    picocalc.fs.write(f, "Hello")
    picocalc.fs.close(f)
end
```

---

#### `picocalc.fs.read(file, length)`
Reads bytes from an open file.

- **Parameters:**
  - `file` (userdata): File handle from `open()`
  - `length` (number): Number of bytes to read
- **Returns:** (string or nil) Data read, or `nil` on error

Raises on a negative length or a closed handle; the length is clamped to the bytes left in the file.

```lua
local data = picocalc.fs.read(f, 1024)
```

---

#### `picocalc.fs.write(file, data)`
Writes data to an open file.

- **Parameters:**
  - `file` (userdata): File handle from `open()`
  - `data` (string): Data to write
- **Returns:** (number) Number of bytes written

```lua
local n = picocalc.fs.write(f, "content")
```

---

#### `picocalc.fs.close(file)`
Closes an open file.

- **Parameters:**
  - `file` (userdata): File handle from `open()`
- **Returns:** None

---

#### `picocalc.fs.exists(path)`
Checks if a file or directory exists.

- **Parameters:**
  - `path` (string): Absolute path
- **Returns:** (boolean) `true` if exists, `false` otherwise

```lua
if picocalc.fs.exists("/data/save.txt") then
    -- Load saved data
end
```

---

#### `picocalc.fs.readFile(path)`
Reads an entire file into memory in one call.

- **Parameters:**
  - `path` (string): Absolute file path
- **Returns:** (string or nil) File contents, or `nil` when the path is denied, missing or unreadable. A file larger than free memory **raises** a memory error instead of returning `nil`; wrap it in `pcall` when the size is not known to fit.

```lua
local content = picocalc.fs.readFile("/apps/hello/config.txt")
```

---

#### `picocalc.fs.size(path)`
Returns the size of a file in bytes.

- **Parameters:**
  - `path` (string): Absolute file path
- **Returns:** (number) File size in bytes, or `-1` on error (file not found or sandbox violation)

```lua
local bytes = picocalc.fs.size("/data/save.txt")
if bytes >= 0 then
    print("File is " .. bytes .. " bytes")
end
```

---

#### `picocalc.fs.listDir(path)`
Lists the contents of a directory.

- **Parameters:**
  - `path` (string): Absolute directory path
- **Returns:** (table) Array of entries, where each entry is a table with:
  - `name` (string): File or directory name
  - `is_dir` (boolean): `true` if directory, `false` if file
  - `size` (number): File size in bytes (0 for directories)

```lua
local entries = picocalc.fs.listDir("/apps")
for _, e in ipairs(entries) do
    print(e.name, e.is_dir, e.size)
end
```

---

#### `picocalc.fs.mkdir(path)`
Creates a directory at the specified path.

- **Parameters:**
  - `path` (string): Absolute directory path to create
- **Returns:** (boolean) `true` if successful or directory already exists, `false` on error

```lua
-- Create app data directory
local data_dir = "/data/" .. APP_ID
if picocalc.fs.mkdir(data_dir) then
    print("Data directory ready")
end
```

---

#### `picocalc.fs.seek(file, position)`
Seeks to a byte position within an open file.

- **Parameters:**
  - `file` (userdata): File handle from `open()`
  - `position` (number): Byte offset from the beginning of the file
- **Returns:** None

```lua
picocalc.fs.seek(f, 0)  -- Seek to beginning
```

---

#### `picocalc.fs.tell(file)`
Returns the current byte position within an open file.

- **Parameters:**
  - `file` (userdata): File handle from `open()`
- **Returns:** (number) Current byte offset

```lua
local pos = picocalc.fs.tell(f)
```

---

#### `picocalc.fs.appPath(name)`
Returns the path `/data/<appname>/<name>`, automatically creating the app's data directory if it does not exist. This is the recommended way to access per-app persistent storage.

- **Parameters:**
  - `name` (string): Filename within the app's data directory
- **Returns:** (string) Full path (e.g., `"/data/myapp/save.json"`)

```lua
local save_path = picocalc.fs.appPath("save.json")
local f = picocalc.fs.open(save_path, "w")
picocalc.fs.write(f, '{"score": 100}')
picocalc.fs.close(f)
```

---

#### `picocalc.fs.browse([startDir])`
Opens a file-browser overlay panel. The user can navigate directories and select a file.

- **Parameters:**
  - `startDir` (string, optional): Starting directory. Defaults to the app's `/data/<appname>/` directory.
- **Returns:** (string or nil) Selected file path, or `nil` if cancelled

```lua
local selected = picocalc.fs.browse("/apps")
if selected then
    print("Selected: " .. selected)
end
```

---

#### `picocalc.fs.copy(src, dst)`
Copy a file. Subject to filesystem sandbox.

- **Parameters:**
  - `src` (string): Source path
  - `dst` (string): Destination path
- **Returns:** (boolean) `true` on success; `(false, string)` on failure

```lua
local ok, err = picocalc.fs.copy("/data/myapp/save.json", "/data/myapp/save_backup.json")
if not ok then print("Copy failed: " .. err) end
```

---

#### `picocalc.fs.delete(path)`
Delete a file. Subject to filesystem sandbox.

- **Parameters:**
  - `path` (string): File path to delete
- **Returns:** (boolean) `true` on success; `(false, string)` on failure

```lua
local ok, err = picocalc.fs.delete("/data/myapp/old_save.json")
if not ok then print("Delete failed: " .. err) end
```

---

#### `picocalc.fs.deleteRecursive(path)`
Delete a directory and all its contents. Subject to filesystem sandbox.

- **Parameters:**
  - `path` (string): Directory path to delete
- **Returns:** (boolean) `true` on success; `(false, string)` on failure

```lua
local ok, err = picocalc.fs.deleteRecursive("/data/myapp/cache")
if not ok then print("Delete failed: " .. err) end
```

---

#### `picocalc.fs.rename(src, dst)`
Rename or move a file. Both paths subject to sandbox.

- **Parameters:**
  - `src` (string): Current path
  - `dst` (string): New path
- **Returns:** (boolean) `true` on success; `(false, string)` on failure

```lua
local ok, err = picocalc.fs.rename("/data/myapp/temp.txt", "/data/myapp/final.txt")
if not ok then print("Rename failed: " .. err) end
```

---

#### `picocalc.fs.stat(path)`
Get file or directory information. Subject to sandbox.

- **Parameters:**
  - `path` (string): File or directory path
- **Returns:** (table) `{size = number, is_dir = boolean}`, or `(nil, string)` on failure

```lua
local info, err = picocalc.fs.stat("/data/myapp/save.json")
if info then
    print("Size: " .. info.size .. ", is_dir: " .. tostring(info.is_dir))
end
```

---

#### `picocalc.fs.diskInfo()`
Get SD card disk space information.

- **Parameters:** None
- **Returns:** (table) `{free = number, total = number}` (values in KB), or `(nil, string)` on failure

```lua
local info, err = picocalc.fs.diskInfo()
if info then
    print("Free: " .. info.free .. " KB / Total: " .. info.total .. " KB")
end
```

---

#### `picocalc.fs.glob(path, pattern)`
List files in a directory matching a glob pattern. Subject to sandbox.

- **Parameters:**
  - `path` (string): Directory to search
  - `pattern` (string): Glob pattern (e.g. `"*.lua"`)
- **Returns:** (table) Array of matching filenames

```lua
local lua_files = picocalc.fs.glob("/apps/myapp", "*.lua")
for _, name in ipairs(lua_files) do
    print(name)
end
```

---

#### `picocalc.fs.ensureReady()`
Ensure the SD card is mounted and ready.

- **Parameters:** None
- **Returns:** (boolean) `true` if SD card is ready

```lua
if picocalc.fs.ensureReady() then
    -- Safe to perform file operations
end
```

---

#### `picocalc.fs.setSlowMode(enabled)`
Enable or disable slow SD card mode. Slow mode reduces SPI clock speed for compatibility with some SD cards.

- **Parameters:**
  - `enabled` (boolean): `true` to enable slow mode, `false` to disable
- **Returns:** None

```lua
picocalc.fs.setSlowMode(true)  -- Use slower SPI clock for compatibility
```
