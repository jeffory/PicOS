# Global Variables and Permissions

## Global Variables

These variables are automatically set when your app is launched:

| Variable | Type | Description |
|----------|------|-------------|
| `APP_DIR` | string | Absolute path to your app's directory (e.g., `"/apps/hello"`) |
| `APP_NAME` | string | Name of your app as defined in `app.json` |
| `APP_ID` | string | Reverse-DNS app identifier from `app.json` (e.g., `"com.picos.hello"`) |
| `APP_REQUIREMENTS` | table | Granted requirements as boolean fields (see [App Requirements](#app-requirements)) |

---

## App Requirements

Apps can request elevated requirements via the `requirements` array in `app.json`:

```json
{
  "requirements": ["root-filesystem"]
}
```

### Available Requirements

| Requirement | Description |
|------------|-------------|
| `filesystem` | Default sandbox access: read `/apps/<appname>/`, read/write `/data/<appid>/` |
| `root-filesystem` | Full SD card read/write access (bypasses sandbox) |

### Checking Requirements in Lua

The `APP_REQUIREMENTS` global is a table with boolean fields:

```lua
if APP_REQUIREMENTS.root_filesystem then
    -- App has full filesystem access
    local entries = picocalc.fs.listDir("/system")
else
    -- Restricted to sandbox
    local path = picocalc.fs.appPath("save.json")
end
```

### Default Sandbox

Without `root-filesystem`, the filesystem API restricts access to:
- **Read**: `/apps/<appname>/` (your app's directory)
- **Read/Write**: `/data/<appid>/` (auto-created app data directory)

Use `picocalc.fs.appPath("filename")` to get paths within your app's data directory.
