# API Filesystem

Filesystem access to the SD card (FAT32).

## picocalc.fs

### Functions

#### `picocalc.fs.open(path [, mode])`
Opens a file on the SD card.

- **Parameters:**
  - `path` (string): Absolute file path (e.g., `"/apps/hello/data.txt"`)
  - `mode` (string, optional): File mode (`"r"`, `"w"`, `"a"`, `"rb"`, `"wb"`, etc.). Defaults to `"r"`.
- **Returns:** (userdata or nil) File handle, or `nil` on error

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
- **Returns:** (string or nil) File contents, or `nil` on error

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
