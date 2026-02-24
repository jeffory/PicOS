# Global Variables and Permissions

## Global Variables

These variables are automatically set when your app is launched:

| Variable | Type | Description |
|----------|------|-------------|
| `APP_DIR` | string | Absolute path to your app's directory (e.g., `"/apps/hello"`) |
| `APP_NAME` | string | Name of your app as defined in `app.json` |
| `APP_ID` | string | Reverse-DNS app identifier from `app.json` (e.g., `"com.picos.hello"`) |
| `APP_PERMISSIONS` | table | Granted permissions as boolean fields (see [App Permissions](#app-permissions)) |

---

## App Permissions

Apps can request elevated permissions via the `permissions` array in `app.json`:

```json
{
  "permissions": ["root-filesystem"]
}
```

### Available Permissions

| Permission | Description |
|------------|-------------|
| `filesystem` | Default sandbox access: read `/apps/<appname>/`, read/write `/data/<appid>/` |
| `root-filesystem` | Full SD card read/write access (bypasses sandbox) |

### Checking Permissions in Lua

The `APP_PERMISSIONS` global is a table with boolean fields:

```lua
if APP_PERMISSIONS.root_filesystem then
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
