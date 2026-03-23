# API Network and WiFi

WiFi connectivity and HTTP client.

## picocalc.wifi

WiFi connectivity functions (Pico 2W only).

### Functions

#### `picocalc.wifi.isAvailable()`
Checks if WiFi hardware is present.

- **Parameters:** None
- **Returns:** (boolean) `true` if WiFi is available (Pico 2W), `false` otherwise

```lua
if picocalc.wifi.isAvailable() then
    picocalc.wifi.connect("MySSID", "password")
end
```

---

#### `picocalc.wifi.connect(ssid [, password])`
Connects to a WiFi network (non-blocking).

- **Parameters:**
  - `ssid` (string): Network SSID
  - `password` (string, optional): Network password (omit for open networks)
- **Returns:** None

```lua
picocalc.wifi.connect("MyWiFi", "password123")
```

---

#### `picocalc.wifi.disconnect()`
Disconnects from the current WiFi network.

- **Parameters:** None
- **Returns:** None

---

#### `picocalc.wifi.getStatus()`
Returns the current WiFi connection status.

- **Parameters:** None
- **Returns:** (number) Status code (see constants below)

```lua
if picocalc.wifi.getStatus() == picocalc.wifi.STATUS_CONNECTED then
    print("Connected! IP: " .. picocalc.wifi.getIP())
end
```

---

#### `picocalc.wifi.getIP()`
Returns the current IP address as a string.

- **Parameters:** None
- **Returns:** (string or nil) IP address (e.g., `"192.168.1.100"`), or `nil` if not connected

---

#### `picocalc.wifi.getSSID()`
Returns the SSID of the current connection.

- **Parameters:** None
- **Returns:** (string or nil) SSID, or `nil` if not connected

---

### WiFi Status Constants

| Constant | Description |
|----------|-------------|
| `picocalc.wifi.STATUS_DISCONNECTED` | Not connected |
| `picocalc.wifi.STATUS_CONNECTING` | Connection in progress |
| `picocalc.wifi.STATUS_CONNECTED` | Connected successfully |
| `picocalc.wifi.STATUS_FAILED` | Connection failed |

---

## picocalc.network

HTTP client for making network requests. Requires WiFi to be connected first. Up to **8 simultaneous connections** are supported. HTTPS (SSL/TLS) is supported via mbedTLS.

HTTP connections are objects with method syntax (`conn:get(...)`, `conn:read()`, etc.). Callbacks are fired automatically — you do not need to poll manually.

### picocalc.network functions

#### `picocalc.network.getStatus()`
Returns the current network status.

- **Parameters:** None
- **Returns:** (number) One of the `kStatus*` constants

```lua
if picocalc.network.getStatus() == picocalc.network.kStatusConnected then
    -- Safe to make HTTP requests
end
```

---

#### `picocalc.network.setEnabled(flag [, callback])`
Enables or disables WiFi. The optional callback is called once the operation completes.

- **Parameters:**
  - `flag` (boolean): `true` to enable, `false` to disable
  - `callback` (function, optional): Called when done. Receives `true` if connected, `false` otherwise.
- **Returns:** None

```lua
picocalc.network.setEnabled(true, function(connected)
    if connected then
        print("WiFi enabled, IP: " .. picocalc.wifi.getIP())
    end
end)
```

---

### Network Status Constants

| Constant | Value | Description |
|----------|-------|-------------|
| `picocalc.network.kStatusNotConnected` | 0 | WiFi present but not connected |
| `picocalc.network.kStatusConnected` | 1 | Connected and ready |
| `picocalc.network.kStatusNotAvailable` | 2 | No WiFi hardware, or connection failed |

---

### picocalc.network.http — HTTP Connection Objects

#### `picocalc.network.http.new(server [, port [, usessl]])`
Creates a new HTTP/HTTPS connection object. Does not connect immediately — call `conn:get()` or `conn:post()` to start a request.

- **Parameters:**
  - `server` (string): Hostname (e.g., `"api.example.com"`)
  - `port` (number, optional): TCP port. Defaults to `80` for HTTP, `443` for HTTPS.
  - `usessl` (boolean, optional): `true` to use HTTPS/TLS. Defaults to `false`.
- **Returns:** (userdata) Connection object, or `nil, errstr` if the connection pool is full

```lua
-- Plain HTTP
local conn = picocalc.network.http.new("api.example.com")

-- HTTPS (port 443 is the default when usessl=true)
local conn = picocalc.network.http.new("api.example.com", 443, true)

if not conn then
    print("Pool full")
end
```

---

### HTTP Connection Methods

All methods are called on the connection object with colon syntax.

---

#### `conn:get(path [, headers])`
Issues an HTTP GET request.

- **Parameters:**
  - `path` (string): URL path (e.g., `"/api/data"`)
  - `headers` (string or table, optional): Extra headers. Can be a raw `"Key: Value
"` string, a flat array of `"Key: Value"` strings, or a `{key=value}` table.
- **Returns:** (boolean) `true` if request started, or `false, errstr` on immediate failure

```lua
conn:get("/api/data")
```

---

#### `conn:post(path [, headers], data)`
Issues an HTTP POST request.

- **Parameters:**
  - `path` (string): URL path
  - `headers` (string or table, optional): Extra headers (omit to pass data as third arg)
  - `data` (string): Request body
- **Returns:** (boolean) `true` if request started, or `false, errstr` on immediate failure

```lua
conn:post("/submit", '{"value":42}')
-- or with headers:
conn:post("/submit", {["Content-Type"] = "application/json"}, '{"value":42}')
```

---

#### `conn:close()`
Closes the connection and frees the connection pool slot. The object is unusable afterwards.

- **Returns:** None

---

#### `conn:setKeepAlive(flag)`
Enables HTTP keep-alive (`Connection: keep-alive`). Must be called before `get`/`post`.

- **Parameters:**
  - `flag` (boolean)
- **Returns:** None

---

#### `conn:setByteRange(from, to)`
Adds a `Range: bytes=from-to` header to the next request (for partial content / resumable downloads). Must be called before `get`/`post`.

- **Parameters:**
  - `from` (number): Start byte (inclusive)
  - `to` (number): End byte (inclusive)
- **Returns:** None

---

#### `conn:setConnectTimeout(seconds)`
Sets the TCP connection timeout. Default is 10 seconds.

- **Parameters:**
  - `seconds` (number): Timeout in seconds (fractions allowed)
- **Returns:** None

---

#### `conn:setReadTimeout(seconds)`
Sets the timeout waiting for response data after connecting. Default is 30 seconds.

- **Parameters:**
  - `seconds` (number): Timeout in seconds (fractions allowed)
- **Returns:** None

---

#### `conn:setReadBufferSize(bytes)`
Resizes the receive ring buffer. Must be called before `get`/`post`. Capped at 32768 bytes. Defaults to 4096 bytes.

- **Parameters:**
  - `bytes` (number): Buffer size in bytes
- **Returns:** None

---

#### `conn:getError()`
Returns the last error string, if any.

- **Returns:** (string or nil) Error description, or `nil` if no error

```lua
local err = conn:getError()
if err then print("HTTP error: " .. err) end
```

---

#### `conn:getProgress()`
Returns download progress.

- **Returns:** (number, number) `bytesReceived, totalBytes` — `totalBytes` is `-1` if unknown (no `Content-Length` header or chunked encoding)

```lua
local received, total = conn:getProgress()
if total > 0 then
    print(string.format("%d%%", received * 100 // total))
end
```

---

#### `conn:getBytesAvailable()`
Returns the number of bytes currently available to read from the receive buffer.

- **Returns:** (number) Bytes available

---

#### `conn:read([length])`
Reads data from the receive buffer.

- **Parameters:**
  - `length` (number, optional): Maximum bytes to read. Defaults to all available. Capped at 65536 bytes per call.
- **Returns:** (string or nil) Data, or `nil` if nothing is available yet

```lua
local chunk = conn:read()
if chunk then buffer = buffer .. chunk end
```

---

#### `conn:getResponseStatus()`
Returns the HTTP response status code.

- **Returns:** (number or nil) e.g. `200`, `404` — `nil` if headers not yet received

---

#### `conn:getResponseHeaders()`
Returns the response headers as a key-value table (lowercase keys).

- **Returns:** (table or nil) `{["content-type"]="text/json", ...}` — `nil` if headers not yet received

```lua
local hdrs = conn:getResponseHeaders()
if hdrs then
    print("Content-Type: " .. (hdrs["content-type"] or "unknown"))
end
```

---

### Callbacks

Callbacks are fired automatically from within the app's execution loop — no polling needed.

#### `conn:setRequestCallback(fn)`
Called each time new body data arrives. `fn` receives no arguments; use `conn:read()` to consume data.

#### `conn:setHeadersReadCallback(fn)`
Called once when all response headers have been received. Use `conn:getResponseStatus()` and `conn:getResponseHeaders()` inside this callback.

#### `conn:setRequestCompleteCallback(fn)`
Called when the response body is fully received.

#### `conn:setConnectionClosedCallback(fn)`
Called when the connection closes, either cleanly or due to an error. Check `conn:getError()` to distinguish.

---

### HTTP Example

```lua
-- Check WiFi is up
if picocalc.network.getStatus() ~= picocalc.network.kStatusConnected then
    print("Not connected")
    return
end

local conn = picocalc.network.http.new("httpbin.org")
local body = ""

conn:setHeadersReadCallback(function()
    print("Status: " .. conn:getResponseStatus())
end)

conn:setRequestCallback(function()
    local chunk = conn:read()
    if chunk then body = body .. chunk end
end)

conn:setRequestCompleteCallback(function()
    print("Done. Body length: " .. #body)
    conn:close()
end)

conn:setConnectionClosedCallback(function()
    local err = conn:getError()
    if err then print("Error: " .. err) end
end)

conn:get("/get")

-- Keep looping until the request completes
local done = false
conn:setRequestCompleteCallback(function()
    print("Done. Body length: " .. #body)
    done = true
end)

while not done do
    picocalc.input.update()
    if picocalc.input.getButtonsPressed() & picocalc.input.BTN_ESC ~= 0 then
        conn:close()
        return
    end
    picocalc.sys.sleep(16)
end
conn:close()
```
