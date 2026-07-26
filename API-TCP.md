# API — TCP Sockets

Raw non-blocking TCP/TLS client sockets. Connections are handled cross-core by the Core 1 network stack. A static pool supports up to 4 simultaneous connections.

## picocalc.tcp

Callbacks fire from the main loop — they are pumped by the firmware's debug hook. Call `picocalc.input.update()` or `picocalc.sys.sleep()` regularly in your main loop so they can run.

### Functions

#### `picocalc.tcp.new(host [, port] [, use_ssl])`
Open a new TCP connection to a host.

- **Parameters:**
  - `host` (string): Hostname or IP address
  - `port` (number, optional): Port number. Default 80.
  - `use_ssl` (boolean, optional): `true` for a TLS connection. Default `false`.
- **Returns:** (userdata) PicOSTcpConn connection object

```lua
local conn = picocalc.tcp.new("example.com", 443, true)
```

---

### Connection Methods

Objects returned by `picocalc.tcp.new()`.

#### `conn:write(data)`
Write data to the connection.

- **Parameters:**
  - `data` (string): Data to send
- **Returns:** (number) Number of bytes written, or -1 on error

```lua
conn:write("GET / HTTP/1.0\r\n\r\n")
```

---

#### `conn:read([max_len])`
Read available data from the connection.

- **Parameters:**
  - `max_len` (number, optional): Maximum number of bytes to read
- **Returns:** (string or nil) Data read, or `nil` if no data is available

```lua
local chunk = conn:read(1024)
```

---

#### `conn:close()`
Close the connection.

- **Parameters:** None
- **Returns:** None

```lua
conn:close()
```

---

#### `conn:available()`
Get the number of bytes available to read.

- **Parameters:** None
- **Returns:** (number) Bytes available

```lua
if conn:available() > 0 then
    local data = conn:read()
end
```

---

#### `conn:getError()`
Get the last connection error.

- **Parameters:** None
- **Returns:** (string or nil) Error string, or `nil` if there is no error

---

#### `conn:isConnected()`
Check whether the connection is established.

- **Parameters:** None
- **Returns:** (boolean) `true` if connected

---

#### `conn:setConnectTimeout(seconds)`
Set the connection timeout.

- **Parameters:**
  - `seconds` (number): Timeout in seconds
- **Returns:** None

```lua
conn:setConnectTimeout(10)
```

---

#### `conn:setReadTimeout(seconds)`
Set the read timeout.

- **Parameters:**
  - `seconds` (number): Timeout in seconds
- **Returns:** None

---

#### `conn:setConnectCallback(fn)`
Set a callback fired when the connection is established (or fails). The callback receives the connection object.

- **Parameters:**
  - `fn` (function): `function(conn) ... end`
- **Returns:** None

```lua
conn:setConnectCallback(function(c)
    picocalc.sys.log("connected")
end)
```

---

#### `conn:setReadCallback(fn)`
Set a callback fired when data arrives. The callback receives the connection object.

- **Parameters:**
  - `fn` (function): `function(conn) ... end`
- **Returns:** None

```lua
conn:setReadCallback(function(c)
    local data = c:read()
    if data then picocalc.sys.log("got " .. #data .. " bytes") end
end)
```

---

#### `conn:setCloseCallback(fn)`
Set a callback fired when the connection closes. The callback receives the connection object.

- **Parameters:**
  - `fn` (function): `function(conn) ... end`
- **Returns:** None

---

#### `conn:getEvents()`
Get pending connection events.

- **Parameters:** None
- **Returns:** (number) Event bitmask

---

#### `conn:waitConnected([timeout_seconds])`
Block until the connection is established.

- **Parameters:**
  - `timeout_seconds` (number, optional): Maximum time to wait
- **Returns:** (boolean) `true` if connected, `false` on timeout or failure

```lua
if conn:waitConnected(5) then
    conn:write("hello")
end
```

---

#### `conn:waitData([timeout_seconds])`
Block until data is available to read.

- **Parameters:**
  - `timeout_seconds` (number, optional): Maximum time to wait
- **Returns:** (boolean) `true` if data arrived, `false` on timeout

```lua
if conn:waitData(2) then
    local data = conn:read()
end
```
