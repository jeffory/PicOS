---@meta
-- PicOS Lua API stubs for LuaLS (lua-language-server).
-- This file is never executed — it exists solely to provide IDE autocomplete,
-- hover documentation, and type checking for PicOS app development.
--
-- Place `.luarc.json` at the project root and set:
--   "Lua.workspace.library": ["sdk/lua"]
--
-- Generated from src/os/lua_bridge_*.c  (PicOS feature/shared-api-native-lua)

-- =============================================================================
-- App globals (injected by launcher before app starts)
-- =============================================================================

---Absolute path to the app's directory on the SD card, e.g. `"/apps/hello"`.
---@type string
APP_DIR = ""

---Human-readable display name from `app.json`.
---@type string
APP_NAME = ""

---Reverse-domain app identifier from `app.json`, e.g. `"com.example.hello"`.
---@type string
APP_ID = ""

---Requirements granted to this app (booleans). Fields: `filesystem`,
---`root_filesystem`, `http`, `audio`, `clipboard`.
---@type { filesystem: boolean, root_filesystem: boolean, http: boolean, audio: boolean, clipboard: boolean }
APP_REQUIREMENTS = {}

-- =============================================================================
-- picocalc  (top-level namespace)
-- =============================================================================

---@class picocalc
picocalc = {}

-- =============================================================================
-- picocalc.display
-- =============================================================================

---@class picocalc.display
---@field BLACK   integer RGB565 black  (0x0000)
---@field WHITE   integer RGB565 white  (0xFFFF)
---@field RED     integer RGB565 red
---@field GREEN   integer RGB565 green
---@field BLUE    integer RGB565 blue
---@field YELLOW  integer RGB565 yellow
---@field CYAN    integer RGB565 cyan
---@field GRAY    integer RGB565 gray
---@field FONT_6X8            integer Built-in 6×8 bitmap font
---@field FONT_8X12           integer Built-in 8×12 bitmap font
---@field FONT_SCIENTIFICA    integer Scientifica bitmap font
---@field FONT_SCIENTIFICA_BOLD integer Scientifica bold bitmap font
picocalc.display = {}

---Clear the display to a solid colour (default: BLACK).
---@param color? integer RGB565 fill colour
function picocalc.display.clear(color) end

---Set a single pixel.
---@param x integer
---@param y integer
---@param color integer RGB565 colour
function picocalc.display.setPixel(x, y, color) end

---Fill a solid rectangle.
---@param x integer
---@param y integer
---@param w integer
---@param h integer
---@param color integer RGB565 colour
function picocalc.display.fillRect(x, y, w, h, color) end

---Draw a rectangle outline.
---@param x integer
---@param y integer
---@param w integer
---@param h integer
---@param color integer RGB565 colour
function picocalc.display.drawRect(x, y, w, h, color) end

---Draw a line between two points.
---@param x0 integer
---@param y0 integer
---@param x1 integer
---@param y1 integer
---@param color integer RGB565 colour
function picocalc.display.drawLine(x0, y0, x1, y1, color) end

---Draw a circle outline.
---@param cx integer Centre x
---@param cy integer Centre y
---@param r integer Radius
---@param color integer RGB565 colour
function picocalc.display.drawCircle(cx, cy, r, color) end

---Fill a solid circle.
---@param cx integer Centre x
---@param cy integer Centre y
---@param r integer Radius
---@param color integer RGB565 colour
function picocalc.display.fillCircle(cx, cy, r, color) end

---Configure the hardware scroll area (for smooth vertical scrolling).
---@param top integer Lines at the top that do not scroll
---@param height integer Height of the scrolling region in lines
---@param bottom integer Lines at the bottom that do not scroll
function picocalc.display.setScrollArea(top, height, bottom) end

---Set the hardware vertical scroll offset within the scroll region.
---@param offset integer Pixel offset
function picocalc.display.setScrollOffset(offset) end

---Draw a text string. Background defaults to BLACK if omitted.
---Returns the pixel width of the rendered text.
---@param x integer
---@param y integer
---@param text string
---@param fg integer RGB565 foreground colour
---@param bg? integer RGB565 background colour
---@return integer width Pixel width of the drawn text
function picocalc.display.drawText(x, y, text, fg, bg) end

---Flush the framebuffer to the LCD (non-blocking DMA). Call once per frame.
function picocalc.display.flush() end

---Returns the display width in pixels (320).
---@return integer
function picocalc.display.getWidth() end

---Returns the display height in pixels (320).
---@return integer
function picocalc.display.getHeight() end

---Set the LCD backlight brightness.
---@param brightness integer 0 (off) – 255 (maximum)
function picocalc.display.setBrightness(brightness) end

---Return the pixel width of a string in the current font, without drawing it.
---@param text string
---@return integer width
function picocalc.display.textWidth(text) end

---Select the active font for subsequent drawText/textWidth calls.
---@param font_id integer One of the FONT_* constants
function picocalc.display.setFont(font_id) end

---Return the currently active font ID.
---@return integer
function picocalc.display.getFont() end

---Return the character cell width of the current font in pixels.
---@return integer
function picocalc.display.getFontWidth() end

---Return the character cell height of the current font in pixels.
---@return integer
function picocalc.display.getFontHeight() end

---Convert 8-bit R/G/B components to a packed RGB565 colour integer.
---@param r integer 0–255
---@param g integer 0–255
---@param b integer 0–255
---@return integer color RGB565
function picocalc.display.rgb(r, g, b) end

-- =============================================================================
-- picocalc.input
-- =============================================================================

---@class picocalc.input
---@field BTN_UP        integer D-pad up
---@field BTN_DOWN      integer D-pad down
---@field BTN_LEFT      integer D-pad left
---@field BTN_RIGHT     integer D-pad right
---@field BTN_ENTER     integer Enter / OK
---@field BTN_ESC       integer Escape / back
---@field BTN_MENU      integer System menu (Sym key)
---@field BTN_F1        integer Function key 1
---@field BTN_F2        integer Function key 2
---@field BTN_F3        integer Function key 3
---@field BTN_F4        integer Function key 4
---@field BTN_F5        integer Function key 5
---@field BTN_F6        integer Function key 6
---@field BTN_F7        integer Function key 7
---@field BTN_F8        integer Function key 8
---@field BTN_F9        integer Function key 9
---@field BTN_BACKSPACE integer Backspace
---@field BTN_TAB       integer Tab
---@field BTN_DEL       integer Delete (Fn+Backspace)
---@field BTN_SHIFT     integer Shift modifier
---@field BTN_CTRL      integer Ctrl modifier
---@field BTN_ALT       integer Alt modifier
---@field BTN_FN        integer Fn / Symbol modifier
picocalc.input = {}

---Poll the keyboard and trigger the system menu if Menu is pressed.
---Call once per frame before reading button state.
function picocalc.input.update() end

---Return a bitmask of all currently held buttons (BTN_* constants).
---@return integer bitmask
function picocalc.input.getButtons() end

---Return a bitmask of buttons pressed *this frame* (edge-detect, not held).
---@return integer bitmask
function picocalc.input.getButtonsPressed() end

---Return a bitmask of buttons released *this frame*.
---@return integer bitmask
function picocalc.input.getButtonsReleased() end

---Return the last ASCII character typed, or `nil` if none this frame.
---Includes full keyboard layout; use for text input.
---@return string|nil
function picocalc.input.getChar() end

---Return the raw hardware key code for the last key event.
---@return integer
function picocalc.input.getRawKey() end

-- =============================================================================
-- picocalc.sys
-- =============================================================================

---@class picocalc.sys
picocalc.sys = {}

---Return milliseconds elapsed since boot.
---@return integer
function picocalc.sys.getTimeMs() end

---Return battery charge (0–100), or -1 if unknown / USB-powered.
---Result is cached for ~5 seconds to avoid slow I²C reads.
---@return integer
function picocalc.sys.getBattery() end

---Log a message to the USB serial port (115200 baud). Prefix: `[APP]`.
---@param message string
function picocalc.sys.log(message) end

---Sleep for `ms` milliseconds. HTTP/TCP callbacks are processed while sleeping.
---@param ms integer
function picocalc.sys.sleep(ms) end

---Exit the app cleanly and return to the launcher. Never returns.
function picocalc.sys.exit() end

---Reboot the device via the watchdog timer. Never returns.
function picocalc.sys.reboot() end

---Returns `false` (stub — USB sense not yet implemented).
---@return boolean
function picocalc.sys.isUSBPowered() end

---Return a table with charging/percentage info.
---@return { charging: boolean, percent: integer }
function picocalc.sys.getPowerStatus() end

---Return current time, optionally NTP-synced.
---@return { synced: boolean, hour: integer, min: integer, sec: integer, epoch: integer }
function picocalc.sys.getClock() end

---Return a snapshot of heap usage.
---@return { psram_free: integer, psram_used: integer, psram_total: integer, sram_free: integer, sram_used: integer }
function picocalc.sys.getMemInfo() end

---Return the OS firmware version string.
---@return string
function picocalc.sys.getVersion() end

---Apply an OTA firmware update from a `.uf2` file.
---@param path string Absolute SD card path to the `.uf2` file
---@return boolean ok
---@return string? error
function picocalc.sys.applyUpdate(path) end

---Register a custom item in the system-menu overlay (max 4 per app).
---@param label string Menu item text
---@param callback fun() Called when the item is selected
function picocalc.sys.addMenuItem(label, callback) end

---Remove all app-registered menu items. Called automatically on app exit.
function picocalc.sys.clearMenuItems() end

---Deliberately trigger a HardFault for crash-handler testing. **Never call in production.**
function picocalc.sys.triggerFault() end

-- =============================================================================
-- picocalc.fs
-- =============================================================================

---@class picocalc.fs
picocalc.fs = {}

---@class PicOSFile : userdata
local PicOSFile = {}

---Open a file on the SD card.
---@param path string Absolute SD card path
---@param mode? string `"r"` (default), `"w"`, `"a"`, `"r+"`, etc.
---@return PicOSFile? handle
---@return string? error
function picocalc.fs.open(path, mode) end

---Read up to `len` bytes from an open file.
---@param file PicOSFile
---@param len integer
---@return string? data `nil` on EOF or error
function picocalc.fs.read(file, len) end

---Write data to an open file. Returns bytes written.
---@param file PicOSFile
---@param data string
---@return integer bytes_written
function picocalc.fs.write(file, data) end

---Close an open file handle.
---@param file PicOSFile
function picocalc.fs.close(file) end

---Seek to an absolute byte offset within an open file.
---@param file PicOSFile
---@param offset integer
---@return boolean ok
function picocalc.fs.seek(file, offset) end

---Return the current byte offset within an open file.
---@param file PicOSFile
---@return integer offset
function picocalc.fs.tell(file) end

---Return `true` if a path exists on the SD card.
---@param path string
---@return boolean
function picocalc.fs.exists(path) end

---Read an entire file into a string.
---@param path string
---@return string? contents `nil` on error
function picocalc.fs.readFile(path) end

---Return the size of a file in bytes, or -1 if the file does not exist.
---@param path string
---@return integer
function picocalc.fs.size(path) end

---@class PicOSDirEntry
---@field name string File or directory name (not full path)
---@field is_dir boolean `true` for directories
---@field size integer File size in bytes (0 for directories)
---@field year? integer
---@field month? integer
---@field day? integer
---@field hour? integer
---@field min? integer
---@field sec? integer

---List directory contents.
---@param path string
---@return PicOSDirEntry[]
function picocalc.fs.listDir(path) end

---Create a directory (and any missing parents).
---@param path string
---@return boolean ok
function picocalc.fs.mkdir(path) end

---Return the writable per-app data path for `name`, i.e. `/data/<APP_ID>/name`.
---@param name string Relative filename
---@return string path
function picocalc.fs.appPath(name) end

---Show a fullscreen file-browser UI and return the selected path, or `nil` if cancelled.
---@param start_path? string Initial directory
---@return string? selected_path
function picocalc.fs.browse(start_path) end

---Delete a file or empty directory.
---@param path string
---@return boolean ok
---@return string? error
function picocalc.fs.delete(path) end

---Rename/move a file or directory.
---@param src string
---@param dst string
---@return boolean ok
---@return string? error
function picocalc.fs.rename(src, dst) end

---Copy a file. Optional progress callback receives `(done: integer, total: integer)`.
---@param src string
---@param dst string
---@param progress_fn? fun(done: integer, total: integer)
---@return boolean ok
---@return string? error
function picocalc.fs.copy(src, dst, progress_fn) end

---@class PicOSStatResult
---@field size integer
---@field is_dir boolean
---@field year? integer
---@field month? integer
---@field day? integer
---@field hour? integer
---@field min? integer
---@field sec? integer

---Return metadata for a path.
---@param path string
---@return PicOSStatResult? info `nil` if path does not exist
---@return string? error
function picocalc.fs.stat(path) end

---Return free and total SD card space in kilobytes.
---@return { free: integer, total: integer }? info
---@return string? error
function picocalc.fs.diskInfo() end

---Return directory entries whose names match a glob pattern (`*` and `?`), case-insensitive.
---@param path string Directory to search
---@param pattern string Glob pattern, e.g. `"*.lua"`
---@return PicOSDirEntry[]
function picocalc.fs.glob(path, pattern) end

-- =============================================================================
-- picocalc.config  (system-wide, /system/config.json)
-- =============================================================================

---@class picocalc.config
picocalc.config = {}

---Read a system config value.
---@param key string
---@return string? value `nil` if key does not exist
function picocalc.config.get(key) end

---Write a system config value (pass `nil` to delete).
---@param key string
---@param value string|nil
function picocalc.config.set(key, value) end

---Persist the config to `/system/config.json`.
---@return boolean ok
function picocalc.config.save() end

---Reload config from `/system/config.json`.
---@return boolean ok
function picocalc.config.load() end

-- =============================================================================
-- picocalc.appconfig  (per-app, /data/<APP_ID>/config.json)
-- =============================================================================

---@class picocalc.appconfig
picocalc.appconfig = {}

---Read a per-app config value, returning `fallback` if the key is absent.
---@param key string
---@param fallback? string
---@return string? value
function picocalc.appconfig.get(key, fallback) end

---Write a per-app config value (in memory only — call `save()` to persist).
---@param key string
---@param value string
function picocalc.appconfig.set(key, value) end

---Persist the per-app config to `/data/<APP_ID>/config.json`.
---@return boolean ok
function picocalc.appconfig.save() end

---Delete the config file and clear all keys from memory.
---@return boolean ok
function picocalc.appconfig.reset() end

-- =============================================================================
-- picocalc.audio  (simple tone generation)
-- =============================================================================

---@class picocalc.audio
picocalc.audio = {}

---Play a tone at the given frequency. If `duration_ms` is 0 or omitted the
---tone plays indefinitely until `stopTone()` is called.
---@param freq integer Frequency in Hz (e.g. 440 for A4)
---@param duration_ms? integer Duration in milliseconds; 0 = indefinite
function picocalc.audio.playTone(freq, duration_ms) end

---Stop the currently playing tone immediately.
function picocalc.audio.stopTone() end

---Set the audio output volume for tone playback.
---@param volume integer 0 (mute) – 100 (maximum)
function picocalc.audio.setVolume(volume) end

-- =============================================================================
-- picocalc.sound  (sample / fileplayer / MP3 player)
-- =============================================================================

---@class picocalc.sound
picocalc.sound = {}

---@class PicOSSample : userdata
---Holds raw PCM audio data loaded from a WAV file.
local PicOSSample = {}

---@class PicOSSamplePlayer : userdata
local PicOSSamplePlayer = {}

---@class PicOSFilePlayer : userdata
local PicOSFilePlayer = {}

---@class PicOSMp3Player : userdata
local PicOSMp3Player = {}

-- ── picocalc.sound top-level constructors ────────────────────────────────────

---Return the audio clock time in milliseconds since the last `resetTime()`.
---@return integer ms
function picocalc.sound.getCurrentTime() end

---Reset the audio clock to zero.
function picocalc.sound.resetTime() end

---Return the number of currently active audio sources.
---@return integer
function picocalc.sound.playingSources() end

---Create a Sample, optionally loading a WAV file immediately.
---@param path_or_duration? string|number WAV file path, or duration in seconds for an empty sample
---@return PicOSSample
function picocalc.sound.sample(path_or_duration) end

---Create a SamplePlayer, optionally pre-loading a sample.
---@param sample_or_path? PicOSSample|string
---@return PicOSSamplePlayer
function picocalc.sound.sampleplayer(sample_or_path) end

---Create a FilePlayer for streaming WAV files from the SD card.
---@param buffer_size? integer Internal streaming buffer size in bytes
---@return PicOSFilePlayer
function picocalc.sound.fileplayer(buffer_size) end

---Create an MP3Player for streaming MP3 files from the SD card.
---@return PicOSMp3Player
function picocalc.sound.mp3player() end

-- ── PicOSSample methods ──────────────────────────────────────────────────────

---Load a WAV file into this sample.
---@param path string
---@return boolean ok
---@return string? error
function PicOSSample:load(path) end

---Return the number of PCM sample frames.
---@return integer
function PicOSSample:getLength() end

---Return the sample rate in Hz (e.g. 44100).
---@return integer
function PicOSSample:getSampleRate() end

---Return format metadata.
---@return { bits: integer, channels: integer, sampleRate: integer }
function PicOSSample:getFormat() end

---Decompress the sample (if compressed). Returns `self` for chaining.
---@return PicOSSample
function PicOSSample:decompress() end

---Return a new Sample containing the sub-range `[start, end]` (sample frames).
---@param start_frame integer
---@param end_frame integer
---@return PicOSSample
function PicOSSample:getSubsample(start_frame, end_frame) end

-- ── PicOSSamplePlayer methods ────────────────────────────────────────────────

---Attach a sample to this player.
---@param sample PicOSSample
---@return boolean ok
function PicOSSamplePlayer:setSample(sample) end

---Return the currently attached sample, or `nil`.
---@return PicOSSample?
function PicOSSamplePlayer:getSample() end

---Start playback. `repeat_count` = number of repetitions (0 = use loop flag).
---@param repeat_count? integer
---@return boolean ok
function PicOSSamplePlayer:play(repeat_count) end

---Stop playback.
function PicOSSamplePlayer:stop() end

---Return `true` while playing.
---@return boolean
function PicOSSamplePlayer:isPlaying() end

---Pause or resume playback.
---@param paused boolean
function PicOSSamplePlayer:setPaused(paused) end

---Return the sample length in frames.
---@return integer
function PicOSSamplePlayer:getLength() end

---Seek to a position in seconds.
---@param seconds number
function PicOSSamplePlayer:setOffset(seconds) end

---Return the current playback position in seconds.
---@return number
function PicOSSamplePlayer:getOffset() end

---Set playback volume.
---@param vol integer 0–255
function PicOSSamplePlayer:setVolume(vol) end

---Return the current volume.
---@return integer
function PicOSSamplePlayer:getVolume() end

---Restrict playback to a sub-range (in sample frames).
---@param start_frame integer
---@param end_frame integer
function PicOSSamplePlayer:setPlayRange(start_frame, end_frame) end

---Set playback rate multiplier (1.0 = normal speed).
---@param rate number
function PicOSSamplePlayer:setRate(rate) end

---Return the current playback rate.
---@return number
function PicOSSamplePlayer:getRate() end

-- ── PicOSFilePlayer methods ──────────────────────────────────────────────────

---Open a WAV file for streaming.
---@param path string
---@return boolean ok
function PicOSFilePlayer:load(path) end

---Start streaming playback.
---@param repeat_count? integer 0 = infinite
---@return boolean ok
function PicOSFilePlayer:play(repeat_count) end

---Stop playback.
function PicOSFilePlayer:stop() end

---Pause playback.
function PicOSFilePlayer:pause() end

---Resume after pause.
function PicOSFilePlayer:resume() end

---@return boolean
function PicOSFilePlayer:isPlaying() end

---Return total file length in seconds.
---@return number
function PicOSFilePlayer:getLength() end

---Return current playback position in seconds.
---@return number
function PicOSFilePlayer:getOffset() end

---Seek to a position in seconds.
---@param seconds number
function PicOSFilePlayer:setOffset(seconds) end

---Set per-channel volumes. `right` defaults to `left` if omitted.
---@param left integer 0–255
---@param right? integer 0–255
function PicOSFilePlayer:setVolume(left, right) end

---Return the current left and right channel volumes.
---@return integer left
---@return integer right
function PicOSFilePlayer:getVolume() end

---Set the loop region (in seconds). Omit both args to loop the whole file.
---@param start_sec? number
---@param end_sec? number
function PicOSFilePlayer:setLoopRange(start_sec, end_sec) end

---Return `true` if a buffer underrun occurred since the last call.
---@return boolean
function PicOSFilePlayer:didUnderrun() end

---Register a callback called when playback finishes.
---@param fn fun()
function PicOSFilePlayer:setFinishCallback(fn) end

---If `true`, stop automatically on buffer underrun rather than filling with silence.
---@param flag boolean
function PicOSFilePlayer:setStopOnUnderrun(flag) end

-- ── PicOSMp3Player methods ───────────────────────────────────────────────────

---Open an MP3 file for streaming.
---@param path string
---@return boolean ok
function PicOSMp3Player:load(path) end

---Start playback.
---@param repeat_count? integer 0 = infinite
---@return boolean ok
function PicOSMp3Player:play(repeat_count) end

---Stop playback.
function PicOSMp3Player:stop() end

---Pause playback.
function PicOSMp3Player:pause() end

---Resume after pause.
function PicOSMp3Player:resume() end

---@return boolean
function PicOSMp3Player:isPlaying() end

---Return current playback position in seconds.
---@return number
function PicOSMp3Player:getPosition() end

---Return total duration in seconds.
---@return number
function PicOSMp3Player:getLength() end

---Return the sample rate of the MP3 stream in Hz.
---@return integer
function PicOSMp3Player:getSampleRate() end

---Set playback volume.
---@param vol integer 0–255
function PicOSMp3Player:setVolume(vol) end

---Return the current volume.
---@return integer
function PicOSMp3Player:getVolume() end

---Enable or disable looping.
---@param loop boolean
function PicOSMp3Player:setLoop(loop) end

-- =============================================================================
-- picocalc.wifi  (low-level WiFi control)
-- =============================================================================

---@class picocalc.wifi
---@field STATUS_DISCONNECTED integer
---@field STATUS_CONNECTING    integer
---@field STATUS_CONNECTED     integer
---@field STATUS_FAILED        integer
picocalc.wifi = {}

---Return `true` if WiFi hardware is present on this device.
---@return boolean
function picocalc.wifi.isAvailable() end

---Initiate a connection to an SSID.
---@param ssid string
---@param password? string
function picocalc.wifi.connect(ssid, password) end

---Disconnect from the current network.
function picocalc.wifi.disconnect() end

---Return the current connection status (STATUS_* constant).
---@return integer
function picocalc.wifi.getStatus() end

---Return the current IP address string, or `nil` if not connected.
---@return string?
function picocalc.wifi.getIP() end

---Return the connected SSID, or `nil` if not connected.
---@return string?
function picocalc.wifi.getSSID() end

-- =============================================================================
-- picocalc.network  (HTTP client)
-- =============================================================================

---@class picocalc.network
---@field kStatusNotConnected integer WiFi not connected (0)
---@field kStatusConnected     integer WiFi connected (1)
---@field kStatusNotAvailable  integer WiFi hardware absent (2)
picocalc.network = {}

---Return the current network status (kStatus* constant).
---@return integer
function picocalc.network.getStatus() end

---Enable or disable WiFi. `callback` is called when the operation completes.
---@param flag boolean
---@param callback? fun()
function picocalc.network.setEnabled(flag, callback) end

---@class picocalc.network.http
picocalc.network.http = {}

---@class PicOSHttpConn : userdata
local PicOSHttpConn = {}

---Create a new HTTP(S) connection object. Does not connect until a request is made.
---@param server string Hostname or IP (no scheme prefix)
---@param port? integer Default: 80 for HTTP, 443 for HTTPS
---@param use_ssl? boolean `true` for HTTPS
---@return PicOSHttpConn? conn
---@return string? error
function picocalc.network.http.new(server, port, use_ssl) end

---Enable or disable HTTP keep-alive for this connection.
---@param flag boolean
function PicOSHttpConn:setKeepAlive(flag) end

---Request a specific byte range (for resumable downloads).
---@param from integer Start byte offset (inclusive)
---@param to integer End byte offset (inclusive)
function PicOSHttpConn:setByteRange(from, to) end

---Set the connection timeout in seconds (default: 10).
---@param seconds number
function PicOSHttpConn:setConnectTimeout(seconds) end

---Set the read timeout in seconds (default: 30).
---@param seconds number
function PicOSHttpConn:setReadTimeout(seconds) end

---Set the internal read buffer size in bytes (max: 32768).
---@param bytes integer
---@return boolean ok
function PicOSHttpConn:setReadBufferSize(bytes) end

---Send an HTTP GET request.
---@param path string URL path, e.g. `"/api/data"`
---@param headers? string Extra request headers (raw HTTP format)
---@return boolean ok
---@return string? error
function PicOSHttpConn:get(path, headers) end

---Send an HTTP POST request.
---@param path string
---@param headers? string Extra headers
---@param body? string Request body
---@return boolean ok
---@return string? error
function PicOSHttpConn:post(path, headers, body) end

---Alias for `post`.
---@param path string
---@param headers? string
---@param body? string
---@return boolean ok
---@return string? error
function PicOSHttpConn:query(path, headers, body) end

---Close the connection.
function PicOSHttpConn:close() end

---Return the last error string, or `nil` if no error.
---@return string?
function PicOSHttpConn:getError() end

---Return download progress. `total` is -1 if Content-Length is unknown.
---@return integer bytes_received
---@return integer total
function PicOSHttpConn:getProgress() end

---Return the number of bytes available to read.
---@return integer
function PicOSHttpConn:getBytesAvailable() end

---Read up to `length` bytes from the response body. Returns `nil` when done.
---@param length? integer Max bytes to read
---@return string?
function PicOSHttpConn:read(length) end

---Return the HTTP response status code (e.g. 200), or `nil` if not yet received.
---@return integer?
function PicOSHttpConn:getResponseStatus() end

---Return all response headers as a key→value table, or `nil` if not yet received.
---@return { [string]: string }?
function PicOSHttpConn:getResponseHeaders() end

---Register a callback fired each time new response data arrives.
---@param fn fun(conn: PicOSHttpConn)
function PicOSHttpConn:setRequestCallback(fn) end

---Register a callback fired once response headers have been parsed.
---@param fn fun(conn: PicOSHttpConn)
function PicOSHttpConn:setHeadersReadCallback(fn) end

---Register a callback fired when the full response body has been received.
---@param fn fun(conn: PicOSHttpConn)
function PicOSHttpConn:setRequestCompleteCallback(fn) end

---Register a callback fired when the connection is closed or fails.
---@param fn fun(conn: PicOSHttpConn)
function PicOSHttpConn:setConnectionClosedCallback(fn) end

-- =============================================================================
-- picocalc.tcp  (raw TCP/TLS client)
-- =============================================================================

---@class picocalc.tcp
picocalc.tcp = {}

---@class PicOSTcpConn : userdata
local PicOSTcpConn = {}

---Create a TCP (or TLS) connection object.
---@param host string Hostname or IP
---@param port? integer Default: 80
---@param use_ssl? boolean `true` for TLS
---@return PicOSTcpConn
function picocalc.tcp.new(host, port, use_ssl) end

---Write data to the connection. Returns bytes written, or -1 on error.
---@param data string
---@return integer
function PicOSTcpConn:write(data) end

---Read up to `max_len` bytes. Returns `nil` if no data is available.
---@param max_len? integer
---@return string?
function PicOSTcpConn:read(max_len) end

---Close the connection.
function PicOSTcpConn:close() end

---Return the number of bytes available to read.
---@return integer
function PicOSTcpConn:available() end

---Return the last error string, or `nil`.
---@return string?
function PicOSTcpConn:getError() end

---Return `true` if the connection is currently established.
---@return boolean
function PicOSTcpConn:isConnected() end

---@param seconds number
function PicOSTcpConn:setConnectTimeout(seconds) end

---@param seconds number
function PicOSTcpConn:setReadTimeout(seconds) end

---Register a callback fired when the connection is established.
---@param fn fun(conn: PicOSTcpConn)
function PicOSTcpConn:setConnectCallback(fn) end

---Register a callback fired when data arrives.
---@param fn fun(conn: PicOSTcpConn)
function PicOSTcpConn:setReadCallback(fn) end

---Register a callback fired when the connection closes.
---@param fn fun(conn: PicOSTcpConn)
function PicOSTcpConn:setCloseCallback(fn) end

---Return a bitmask of pending connection events.
---@return integer
function PicOSTcpConn:getEvents() end

---Block until connected, with an optional timeout. Returns `true` if connected.
---@param timeout_seconds? number
---@return boolean
function PicOSTcpConn:waitConnected(timeout_seconds) end

---Block until data is available, with an optional timeout. Returns `true` if data arrived.
---@param timeout_seconds? number
---@return boolean
function PicOSTcpConn:waitData(timeout_seconds) end

-- =============================================================================
-- picocalc.ui  (modal dialogs and HUD widgets)
-- =============================================================================

---@class picocalc.ui
picocalc.ui = {}

---Draw a status bar at the top of the screen with a title and battery/WiFi/clock indicators.
---@param title string
function picocalc.ui.drawHeader(title) end

---Draw a status bar at the bottom of the screen.
---@param left? string Left-aligned text
---@param right? string Right-aligned text
function picocalc.ui.drawFooter(left, right) end

---Draw a tab bar. Returns the new active tab index and the tab-bar height.
---@param y integer Top y coordinate
---@param tabs string[] Tab label strings
---@param active_index integer Currently active tab (1-based)
---@param prev_key? integer Button constant to switch to previous tab
---@param next_key? integer Button constant to switch to next tab
---@return integer active_index
---@return integer height
function picocalc.ui.drawTabs(y, tabs, active_index, prev_key, next_key) end

---Show a blocking modal text-input dialog. Returns the entered string, or `nil` if cancelled.
---@param prompt? string
---@param default? string
---@return string?
function picocalc.ui.textInput(prompt, default) end

---Show a blocking yes/no confirmation dialog. Returns `true` if the user confirms.
---@param message string
---@return boolean
function picocalc.ui.confirm(message) end

---Draw a spinning progress indicator.
---@param cx integer Centre x
---@param cy integer Centre y
---@param r? integer Radius
---@param frame? integer Animation frame index (auto-advances if omitted)
function picocalc.ui.drawSpinner(cx, cy, r, frame) end

-- =============================================================================
-- picocalc.perf  (performance profiling)
-- =============================================================================

---@class picocalc.perf
picocalc.perf = {}

---Mark the start of a frame for FPS measurement.
function picocalc.perf.beginFrame() end

---Mark the end of a frame. Call after `display.flush()`.
function picocalc.perf.endFrame() end

---Return the rolling-average FPS.
---@return integer
function picocalc.perf.getFPS() end

---Return the last frame time in milliseconds.
---@return integer
function picocalc.perf.getFrameTime() end

---Draw a colour-coded FPS counter at (x, y).
---@param x? integer Default: top-left
---@param y? integer
function picocalc.perf.drawFPS(x, y) end

---Set a target FPS cap (0 = uncapped).
---@param fps integer
function picocalc.perf.setTargetFPS(fps) end

-- =============================================================================
-- picocalc.graphics  (images, sprites, spritesheets, animations, fonts)
-- =============================================================================

---@class picocalc.graphics
picocalc.graphics = {}

---Set the default drawing colour used by `clear()` and filled graphic operations.
---@param color integer RGB565
function picocalc.graphics.setColor(color) end

---Set the background (erase) colour.
---@param color integer RGB565
function picocalc.graphics.setBackgroundColor(color) end

---Set a global transparent colour for image blitting (`nil` to disable).
---@param color integer|nil RGB565
function picocalc.graphics.setTransparentColor(color) end

---Return the current global transparent colour, or `nil`.
---@return integer?
function picocalc.graphics.getTransparentColor() end

---Clear the display using the current background colour.
---@param color? integer RGB565 override
function picocalc.graphics.clear(color) end

-- ── Image ────────────────────────────────────────────────────────────────────

---@class picocalc.graphics.image
picocalc.graphics.image = {}

---@class PicOSImage : userdata
local PicOSImage = {}

---@class PicOSImageStream : userdata
local PicOSImageStream = {}

---Load an image from the SD card (BMP, JPEG, PNG, GIF).
---@param path string
---@return PicOSImage? img
---@return string? error
function picocalc.graphics.image.load(path) end

---Load a sub-region of an image from the SD card.
---@param path string
---@param x integer Source x offset
---@param y integer Source y offset
---@param w integer Region width
---@param h integer Region height
---@return PicOSImage?
function picocalc.graphics.image.loadRegion(path, x, y, w, h) end

---Load and scale an image from the SD card.
---@param path string
---@param w integer Target width
---@param h integer Target height
---@return PicOSImage?
function picocalc.graphics.image.loadScaled(path, w, h) end

---Load an image from a Lua string (in-memory buffer).
---@param data string Raw encoded image bytes
---@param format? string `"bmp"`, `"jpeg"`, `"png"`, `"gif"`
---@return PicOSImage?
function picocalc.graphics.image.loadFromBuffer(data, format) end

---Load an image from a URL asynchronously.
---@param url string
---@param callback fun(img: PicOSImage?, error: string?)
---@return PicOSImage?
function picocalc.graphics.image.loadRemote(url, callback) end

---Create a blank (black) image of the given dimensions.
---@param width integer
---@param height integer
---@return PicOSImage
function picocalc.graphics.image.new(width, height) end

---Return metadata for an image file without fully decoding it.
---@param path string
---@return { width: integer, height: integer, format: string }?
function picocalc.graphics.image.getInfo(path) end

---Create a streaming tile decoder for large images.
---@param path string
---@param tile_w? integer
---@param tile_h? integer
---@return PicOSImageStream?
function picocalc.graphics.image.newStream(path, tile_w, tile_h) end

---Set a placeholder image shown while an async load is pending.
---@param img PicOSImage
function picocalc.graphics.image.setPlaceholder(img) end

---Return a list of supported image format strings (e.g. `{"bmp", "jpeg", ...}`).
---@return string[]
function picocalc.graphics.image.getSupportedFormats() end

-- PicOSImage methods

---Return the dimensions of this image.
---@return integer width
---@return integer height
function PicOSImage:getSize() end

---Return a deep copy of this image.
---@return PicOSImage
function PicOSImage:copy() end

---Draw the image at (x, y). Optional options table can include `flipX` and `flipY`.
---Optional `rect` clips the source region `{x, y, w, h}`.
---@param x integer
---@param y integer
---@param options? { flipX?: boolean, flipY?: boolean }
---@param rect? { x: integer, y: integer, w: integer, h: integer }
function PicOSImage:draw(x, y, options, rect) end

---Draw the image with an anchor point. `ax`, `ay` in [0,1] — (0,0) = top-left, (0.5,0.5) = centre.
---@param x integer
---@param y integer
---@param ax number
---@param ay number
function PicOSImage:drawAnchored(x, y, ax, ay) end

---Tile-fill a rectangle of size `rect_w` × `rect_h` starting at (x, y).
---@param x integer
---@param y integer
---@param rect_w integer
---@param rect_h integer
function PicOSImage:drawTiled(x, y, rect_w, rect_h) end

---Draw the image scaled to `dst_w` × `dst_h` at (x, y) (bilinear).
---@param x integer
---@param y integer
---@param dst_w integer
---@param dst_h integer
function PicOSImage:drawScaled(x, y, dst_w, dst_h) end

---Draw the image scaled to `dst_w` × `dst_h` at (x, y) (nearest-neighbour, fast).
---@param x integer
---@param y integer
---@param dst_w integer
---@param dst_h integer
function PicOSImage:drawScaledNN(x, y, dst_w, dst_h) end

---Set a transparent colour for this image (overrides global setting).
---@param color integer|nil RGB565, or `nil` to clear
function PicOSImage:setTransparentColor(color) end

---Return this image's transparent colour, or `nil`.
---@return integer?
function PicOSImage:getTransparentColor() end

---@param location string `"psram"` or `"sram"`
function PicOSImage:setStorageLocation(location) end

---Return metadata for this image (width, height, format, etc.)
---@return table
function PicOSImage:getMetadata() end

-- PicOSImageStream methods

---Decode and return the next tile. Returns `nil` when complete.
---@return PicOSImage?
function PicOSImageStream:getNextTile() end

---Return `true` when all tiles have been decoded.
---@return boolean
function PicOSImageStream:isComplete() end

-- ── Image cache ───────────────────────────────────────────────────────────────

---@class picocalc.graphics.cache
picocalc.graphics.cache = {}

---Set the maximum PSRAM memory budget for the image cache.
---@param bytes integer
function picocalc.graphics.cache.setMaxMemory(bytes) end

---Pin an image in the cache by path so it is not evicted.
---@param path string
function picocalc.graphics.cache.retain(path) end

---Unpin a previously retained image.
---@param path string
function picocalc.graphics.cache.release(path) end

-- ── Sprite ────────────────────────────────────────────────────────────────────

---@class picocalc.graphics.sprite
picocalc.graphics.sprite = {}

---@class PicOSSprite : userdata
local PicOSSprite = {}

---Create a new Sprite object.
---@return PicOSSprite
function picocalc.graphics.sprite.new() end

---Add a sprite to the global sprite list.
---@param sprite PicOSSprite
function picocalc.graphics.sprite.addSprite(sprite) end

---Remove a sprite from the global sprite list.
---@param sprite PicOSSprite
function picocalc.graphics.sprite.removeSprite(sprite) end

---Update all sprites (calls each sprite's update callback).
function picocalc.graphics.sprite.update() end

---Return all sprites in the global list.
---@return PicOSSprite[]
function picocalc.graphics.sprite.getAllSprites() end

---Return the number of sprites in the global list.
---@return integer
function picocalc.graphics.sprite.spriteCount() end

---Remove all sprites from the global list.
function picocalc.graphics.sprite.removeAll() end

---Remove a list of sprites from the global list.
---@param sprites PicOSSprite[]
function picocalc.graphics.sprite.removeSprites(sprites) end

---Call `fn(sprite)` for every sprite in the global list.
---@param fn fun(sprite: PicOSSprite)
function picocalc.graphics.sprite.performOnAllSprites(fn) end

---Return sprites whose bounds overlap a point.
---@param x integer
---@param y integer
---@return PicOSSprite[]
function picocalc.graphics.sprite.querySpritesAtPoint(x, y) end

---Return sprites whose bounds overlap a rectangle.
---@param x integer
---@param y integer
---@param w integer
---@param h integer
---@return PicOSSprite[]
function picocalc.graphics.sprite.querySpritesInRect(x, y, w, h) end

---Return sprites whose bounds intersect a line segment.
---@param x1 integer
---@param y1 integer
---@param x2 integer
---@param y2 integer
---@return PicOSSprite[]
function picocalc.graphics.sprite.querySpritesAlongLine(x1, y1, x2, y2) end

---Return collision info for sprites along a line segment.
---@param x1 integer
---@param y1 integer
---@param x2 integer
---@param y2 integer
---@return table[]
function picocalc.graphics.sprite.querySpriteInfoAlongLine(x1, y1, x2, y2) end

---Set clip rects for sprites in a z-index range.
---@param z_start integer
---@param z_end integer
---@param clip_rect { x: integer, y: integer, w: integer, h: integer }
function picocalc.graphics.sprite.setClipRectsInRange(z_start, z_end, clip_rect) end

---Clear clip rects for sprites in a z-index range.
---@param z_start integer
---@param z_end integer
function picocalc.graphics.sprite.clearClipRectsInRange(z_start, z_end) end

---Add an invisible collision sprite at a rect (useful for tilemaps).
---@param x integer
---@param y integer
---@param w integer
---@param h integer
---@return PicOSSprite
function picocalc.graphics.sprite.addEmptyCollisionSprite(x, y, w, h) end

-- PicOSSprite methods

---@param image PicOSImage
function PicOSSprite:setImage(image) end

---@return PicOSImage?
function PicOSSprite:getImage() end

---Add this sprite to the global sprite list.
function PicOSSprite:add() end

---Remove this sprite from the global sprite list.
function PicOSSprite:remove() end

---@param x integer
---@param y integer
function PicOSSprite:moveTo(x, y) end

---@param dx integer
---@param dy integer
function PicOSSprite:moveBy(dx, dy) end

---@return integer x
---@return integer y
function PicOSSprite:getPosition() end

---@param z integer
function PicOSSprite:setZIndex(z) end

---@return integer
function PicOSSprite:getZIndex() end

---@param visible boolean
function PicOSSprite:setVisible(visible) end

---@return boolean
function PicOSSprite:isVisible() end

---@param ax number 0–1 (horizontal anchor: 0 = left, 0.5 = centre, 1 = right)
---@param ay number 0–1 (vertical anchor)
function PicOSSprite:setCenter(ax, ay) end

---@return number ax
---@return number ay
function PicOSSprite:getCenter() end

---@return integer cx
---@return integer cy
function PicOSSprite:getCenterPoint() end

---@param w integer
---@param h integer
function PicOSSprite:setSize(w, h) end

---@return integer w
---@return integer h
function PicOSSprite:getSize() end

---@param scale number
function PicOSSprite:setScale(scale) end

---@return number
function PicOSSprite:getScale() end

---Enable nearest-neighbour scaling.
---@param nn boolean
function PicOSSprite:setScaleNN(nn) end

---@param color integer|nil RGB565
function PicOSSprite:setTransparentColor(color) end

---@param degrees number
function PicOSSprite:setRotation(degrees) end

---@return number
function PicOSSprite:getRotation() end

---@return PicOSSprite
function PicOSSprite:copy() end

---Restrict image blitting to a sub-rect of the source image.
---@param x integer
---@param y integer
---@param w integer
---@param h integer
function PicOSSprite:setSourceRect(x, y, w, h) end

---Remove the source-rect restriction.
function PicOSSprite:clearSourceRect() end

---@param enabled boolean
function PicOSSprite:setUpdatesEnabled(enabled) end

---@return boolean
function PicOSSprite:updatesEnabled() end

---@param tag integer
function PicOSSprite:setTag(tag) end

---@return integer
function PicOSSprite:getTag() end

---@param mode integer
function PicOSSprite:setImageDrawMode(mode) end

---@param flipX boolean
---@param flipY boolean
function PicOSSprite:setImageFlip(flipX, flipY) end

---@return boolean flipX
---@return boolean flipY
function PicOSSprite:getImageFlip() end

---@param ignore boolean
function PicOSSprite:setIgnoresDrawOffset(ignore) end

---Set the sprite bounding box.
---@param x integer
---@param y integer
---@param w integer
---@param h integer
function PicOSSprite:setBounds(x, y, w, h) end

---@return integer x
---@return integer y
---@return integer w
---@return integer h
function PicOSSprite:getBounds() end

---@return { x: integer, y: integer, w: integer, h: integer }
function PicOSSprite:getBoundsRect() end

---@param opaque boolean
function PicOSSprite:setOpaque(opaque) end

---@return boolean
function PicOSSprite:isOpaque() end

---@param fn fun(sprite: PicOSSprite, x: integer, y: integer, w: integer, h: integer)
function PicOSSprite:setBackgroundDrawingCallback(fn) end

---Draw this sprite immediately (outside the normal update cycle).
function PicOSSprite:draw() end

---Update this sprite (calls its registered update callback).
function PicOSSprite:update() end

---@param enabled boolean
function PicOSSprite:setCollisionsEnabled(enabled) end

---@return boolean
function PicOSSprite:collisionsEnabled() end

---Set the collision rectangle (relative to the sprite's bounds).
---@param x integer
---@param y integer
---@param w integer
---@param h integer
function PicOSSprite:setCollideRect(x, y, w, h) end

---@return integer x
---@return integer y
---@return integer w
---@return integer h
function PicOSSprite:getCollideRect() end

---@return { x: integer, y: integer, w: integer, h: integer }
function PicOSSprite:getCollideBounds() end

---Clear the collision rectangle (no collision).
function PicOSSprite:clearCollideRect() end

---Return sprites that currently overlap this sprite's collision rect.
---@return PicOSSprite[]
function PicOSSprite:overlappingSprites() end

---Return all sprites that overlap this sprite's collision rect (including off-screen).
---@return PicOSSprite[]
function PicOSSprite:allOverlappingSprites() end

---Clear the stencil mask.
function PicOSSprite:clearStencil() end

---Set a checkerboard stencil pattern.
---@param x integer Pattern phase x
---@param y integer Pattern phase y
function PicOSSprite:setStencilPattern(x, y) end

---Return `true` if this sprite's image collides with `other` based on alpha masks.
---@param other PicOSSprite
---@return boolean
function PicOSSprite:alphaCollision(other) end

-- ── Spritesheet ───────────────────────────────────────────────────────────────

---@class picocalc.graphics.spritesheet
picocalc.graphics.spritesheet = {}

---@class PicOSSpritesheet : userdata
local PicOSSpritesheet = {}

---Create a spritesheet from a manually-built frame list.
---@return PicOSSpritesheet
function picocalc.graphics.spritesheet.new() end

---Create a spritesheet from a uniform grid of equal-sized frames.
---@param image PicOSImage Source image
---@param frame_w integer Frame width in pixels
---@param frame_h integer Frame height in pixels
---@return PicOSSpritesheet
function picocalc.graphics.spritesheet.newGrid(image, frame_w, frame_h) end

---Add a frame to the spritesheet.
---@param image PicOSImage
function PicOSSpritesheet:addFrame(image) end

---Return the number of frames.
---@return integer
function PicOSSpritesheet:getFrameCount() end

---Return the image for frame index `i` (1-based).
---@param i integer
---@return PicOSImage?
function PicOSSpritesheet:getFrame(i) end

---Return the combined source image.
---@return PicOSImage?
function PicOSSpritesheet:getImage() end

---Draw frame `i` at (x, y).
---@param i integer 1-based frame index
---@param x integer
---@param y integer
function PicOSSpritesheet:drawFrame(i, x, y) end

-- ── AnimationLoop ─────────────────────────────────────────────────────────────

---@class picocalc.graphics.animation
picocalc.graphics.animation = {}

---@class picocalc.graphics.animation.loop
picocalc.graphics.animation.loop = {}

---@class PicOSAnimationLoop : userdata
local PicOSAnimationLoop = {}

---Create an animation loop from a spritesheet.
---@param spritesheet PicOSSpritesheet
---@param frame_duration_ms? integer Milliseconds per frame (default: 100)
---@return PicOSAnimationLoop
function picocalc.graphics.animation.loop.new(spritesheet, frame_duration_ms) end

---Draw the current frame at (x, y).
---@param x integer
---@param y integer
function PicOSAnimationLoop:draw(x, y) end

---Advance the animation timer.
function PicOSAnimationLoop:update() end

---Return the current frame image.
---@return PicOSImage?
function PicOSAnimationLoop:image() end

---Return `true` if the animation still has frames (always `true` for loops).
---@return boolean
function PicOSAnimationLoop:isValid() end

---Return the current zero-based frame index.
---@return integer
function PicOSAnimationLoop:getFrameIndex() end

---Replace the image table.
---@param spritesheet PicOSSpritesheet
function PicOSAnimationLoop:setImageTable(spritesheet) end

---Set the milliseconds per frame.
---@param ms integer
function PicOSAnimationLoop:setInterval(ms) end

---Enable or disable looping.
---@param loop boolean
function PicOSAnimationLoop:setLooping(loop) end

---Reset to frame 0.
function PicOSAnimationLoop:reset() end

-- ── Blinker ───────────────────────────────────────────────────────────────────

---@class picocalc.graphics.animation.blinker
picocalc.graphics.animation.blinker = {}

---@class PicOSBlinker : userdata
local PicOSBlinker = {}

---Create a blinker (on/off flash timer).
---@return PicOSBlinker
function picocalc.graphics.animation.blinker.new() end

---Update all blinkers.
function picocalc.graphics.animation.blinker.updateAll() end

---Stop all blinkers.
function picocalc.graphics.animation.blinker.stopAll() end

---@param on_ms integer Milliseconds on
---@param off_ms integer Milliseconds off
---@param count? integer Number of cycles (default: 1)
function PicOSBlinker:start(on_ms, off_ms, count) end

---Start looping indefinitely.
---@param on_ms integer
---@param off_ms integer
function PicOSBlinker:startLoop(on_ms, off_ms) end

---Stop the blinker.
function PicOSBlinker:stop() end

---Remove from the global blinker list.
function PicOSBlinker:remove() end

---@return boolean
function PicOSBlinker:isRunning() end

---Advance the blinker timer by the elapsed time.
function PicOSBlinker:update() end

-- ── Animator ──────────────────────────────────────────────────────────────────

---@class picocalc.graphics.animator
picocalc.graphics.animator = {}

---@class PicOSAnimator : userdata
local PicOSAnimator = {}

---Create an Animator that interpolates a value from `from` to `to` over `duration_ms`.
---@param from number
---@param to number
---@param duration_ms integer
---@param easing_fn? fun(t: number): number
---@return PicOSAnimator
function picocalc.graphics.animator.new(from, to, duration_ms, easing_fn) end

---Return the current interpolated value.
---@return number
function PicOSAnimator:currentValue() end

---Return the interpolated value at a given elapsed time in milliseconds.
---@param ms integer
---@return number
function PicOSAnimator:valueAtTime(ms) end

---Return completion progress in [0, 1].
---@return number
function PicOSAnimator:progress() end

---Reset the animation to the start.
function PicOSAnimator:reset() end

---Return `true` when the animation has finished.
---@return boolean
function PicOSAnimator:ended() end

-- ── Font ──────────────────────────────────────────────────────────────────────

---@class picocalc.graphics.font
picocalc.graphics.font = {}

---@class PicOSFont : userdata
local PicOSFont = {}

---Load a bitmap font from the SD card.
---@param path string
---@return PicOSFont?
function picocalc.graphics.font.new(path) end

---Draw text using this font.
---@param text string
---@param x integer
---@param y integer
---@param color? integer RGB565
function PicOSFont:drawText(text, x, y, color) end

---Return the character height of this font.
---@return integer
function PicOSFont:getHeight() end

---Return the character width (for monospaced fonts).
---@return integer
function PicOSFont:getWidth() end

---Return the pixel width of a string in this font.
---@param text string
---@return integer
function PicOSFont:getTextWidth(text) end

---Return the font's name string.
---@return string
function PicOSFont:getName() end

-- =============================================================================
-- picocalc.video  (MJPEG AVI playback)
-- =============================================================================

---@class picocalc.video
picocalc.video = {}

---@class PicOSVideoPlayer : userdata
local PicOSVideoPlayer = {}

---Create a new video player. Destroy with `player:destroy()` when done.
---@return PicOSVideoPlayer
function picocalc.video.player() end

---Load an MJPEG AVI file. Returns `true` on success.
---@param path string
---@return boolean ok
function PicOSVideoPlayer:load(path) end

---Start playback. Raises clock to 300 MHz and disconnects WiFi for decode performance.
function PicOSVideoPlayer:play() end

---Pause playback.
function PicOSVideoPlayer:pause() end

---Resume after pause.
function PicOSVideoPlayer:resume() end

---Stop playback and restore system clock / reconnect WiFi.
function PicOSVideoPlayer:stop() end

---Advance one frame and decode it to the display. Call once per loop iteration.
---Returns `true` while still playing.
---@return boolean playing
function PicOSVideoPlayer:update() end

---Seek to a specific frame index.
---@param frame integer
function PicOSVideoPlayer:seek(frame) end

---@return boolean
function PicOSVideoPlayer:isPlaying() end

---@return boolean
function PicOSVideoPlayer:isPaused() end

---Return the video frame rate.
---@return number fps
function PicOSVideoPlayer:getFPS() end

---Return the video dimensions.
---@return integer width
---@return integer height
function PicOSVideoPlayer:getSize() end

---Return metadata and playback state.
---@return { width: integer, height: integer, fps: number, frame_count: integer, current_frame: integer, has_audio: boolean }
function PicOSVideoPlayer:getInfo() end

---Return `true` if the loaded AVI file contains an MP3 audio track.
---@return boolean
function PicOSVideoPlayer:hasAudio() end

---Set audio volume.
---@param vol integer 0–255
function PicOSVideoPlayer:setVolume(vol) end

---@return integer
function PicOSVideoPlayer:getVolume() end

---@param muted boolean
function PicOSVideoPlayer:setMuted(muted) end

---@return boolean
function PicOSVideoPlayer:isMuted() end

---Enable/disable looping.
---@param loop boolean
function PicOSVideoPlayer:setLoop(loop) end

---If `true` (default), `update()` calls `display.flush()` automatically after each frame.
---@param af boolean
function PicOSVideoPlayer:setAutoFlush(af) end

---Return the number of frames dropped since last `resetStats()`.
---@return integer
function PicOSVideoPlayer:getDroppedFrames() end

---Reset dropped-frame counter.
function PicOSVideoPlayer:resetStats() end

---Free all resources. Also called by the garbage collector.
function PicOSVideoPlayer:destroy() end

-- =============================================================================
-- picocalc.game  (camera, scene manager, save files)
-- =============================================================================

---@class picocalc.game
picocalc.game = {}

-- ── Camera ────────────────────────────────────────────────────────────────────

---@class picocalc.game.camera
picocalc.game.camera = {}

---@class PicOSCamera : userdata
local PicOSCamera = {}

---Create a new Camera at (0, 0) with zoom 1.0.
---@return PicOSCamera
function picocalc.game.camera.new() end

---@param x integer
---@param y integer
function PicOSCamera:setPosition(x, y) end

---@param dx integer
---@param dy integer
function PicOSCamera:move(dx, dy) end

---@return integer x
---@return integer y
function PicOSCamera:getPosition() end

---@param zoom number
function PicOSCamera:setZoom(zoom) end

---@return number
function PicOSCamera:getZoom() end

---Set a target sprite/object to follow. The camera will smoothly track it.
---@param target any Object with `getPosition()` method
function PicOSCamera:setTarget(target) end

---Clear the follow target.
function PicOSCamera:clearTarget() end

---Constrain the camera to a world-space rectangle.
---@param x integer
---@param y integer
---@param w integer
---@param h integer
function PicOSCamera:setBounds(x, y, w, h) end

---Remove world bounds.
function PicOSCamera:clearBounds() end

---@return integer x
---@return integer y
---@return integer w
---@return integer h
function PicOSCamera:getBounds() end

---Apply a full-screen shake effect for `duration_ms` milliseconds.
---@param amplitude integer Pixels of shake
---@param duration_ms integer
function PicOSCamera:shake(amplitude, duration_ms) end

---Apply a horizontal shake.
---@param amplitude integer
---@param duration_ms integer
function PicOSCamera:shakeX(amplitude, duration_ms) end

---Apply a vertical shake.
---@param amplitude integer
---@param duration_ms integer
function PicOSCamera:shakeY(amplitude, duration_ms) end

---Cancel an active shake.
function PicOSCamera:stopShake() end

---Convert world-space coordinates to screen-space.
---@param wx integer
---@param wy integer
---@return integer sx
---@return integer sy
function PicOSCamera:worldToScreen(wx, wy) end

---Convert screen-space coordinates to world-space.
---@param sx integer
---@param sy integer
---@return integer wx
---@return integer wy
function PicOSCamera:screenToWorld(sx, sy) end

---Update camera position (advances follow target, shake, etc.).
function PicOSCamera:update() end

---Return the current draw offset applied to the display.
---@return integer ox
---@return integer oy
function PicOSCamera:getOffset() end

-- ── Scene manager ─────────────────────────────────────────────────────────────

---@class picocalc.game.scene
picocalc.game.scene = {}

---@class PicOSScene : userdata
local PicOSScene = {}

---Create a new Scene. A scene is a table-like object with `update()`, `draw()`,
---`enter()`, and `exit()` lifecycle methods.
---@return PicOSScene
function picocalc.game.scene.new() end

---Add a scene to the manager (does not make it active).
---@param name string
---@param scene PicOSScene
function picocalc.game.scene.add(name, scene) end

---Remove a named scene.
---@param name string
function picocalc.game.scene.remove(name) end

---Return `true` if a scene with the given name is registered.
---@param name string
---@return boolean
function picocalc.game.scene.has(name) end

---Switch immediately to a named scene (calls `exit()` on current, `enter()` on next).
---@param name string
function picocalc.game.scene.switch(name) end

---Push a scene on the stack without exiting the current one.
---@param name string
function picocalc.game.scene.push(name) end

---Pop the top scene off the stack and return to the previous scene.
function picocalc.game.scene.pop() end

---Return the current scene.
---@return PicOSScene?
function picocalc.game.scene.getCurrent() end

---Call `update()` on the current scene.
function picocalc.game.scene.update() end

---Call `draw()` on the current scene.
function picocalc.game.scene.draw() end

---Return (or create) an object pool associated with a scene.
---@param name string Scene name
---@param factory? fun(): any Factory function for new objects
---@return table pool
function picocalc.game.scene.objectPool(name, factory) end

---Set a global value accessible to all scenes.
---@param key string
---@param value any
function picocalc.game.scene.setGlobal(key, value) end

---Get a global value.
---@param key string
---@return any
function picocalc.game.scene.getGlobal(key) end

---Clear all global values.
function picocalc.game.scene.clearGlobals() end

-- ── Save files ────────────────────────────────────────────────────────────────

---@class picocalc.game.save
picocalc.game.save = {}

---Write a value to the app's save file. `value` must be serialisable (string, number, boolean, table).
---@param key string
---@param value any
function picocalc.game.save.set(key, value) end

---Read a value from the app's save file.
---@param key string
---@return any
function picocalc.game.save.get(key) end

---Return `true` if a save key exists.
---@param key string
---@return boolean
function picocalc.game.save.exists(key) end

---Delete a save key.
---@param key string
function picocalc.game.save.delete(key) end

---Return a list of all save keys.
---@return string[]
function picocalc.game.save.list() end

-- =============================================================================
-- picocalc.terminal  (in-app virtual terminal widget)
-- =============================================================================

---@class picocalc.terminal
picocalc.terminal = {}

---@class PicOSTerminal : userdata
local PicOSTerminal = {}

---Create a terminal widget with `cols` × `rows` characters and `scrollback_lines` of history.
---@param cols integer
---@param rows integer
---@param scrollback_lines? integer
---@return PicOSTerminal
function picocalc.terminal.new(cols, rows, scrollback_lines) end

---Write a UTF-8 string (with ANSI escape codes) to the terminal.
---@param text string
function PicOSTerminal:write(text) end

---Clear all terminal content.
function PicOSTerminal:clear() end

---Move the cursor to (x, y) (0-based, column × row).
---@param x integer
---@param y integer
function PicOSTerminal:setCursor(x, y) end

---Return the current cursor position.
---@return integer x
---@return integer y
function PicOSTerminal:getCursor() end

---Set foreground and background colours.
---@param fg integer RGB565
---@param bg integer RGB565
function PicOSTerminal:setColors(fg, bg) end

---Return the current foreground and background colours.
---@return integer fg
---@return integer bg
function PicOSTerminal:getColors() end

---Scroll the terminal by `lines` lines (positive = down).
---@param lines integer
function PicOSTerminal:scroll(lines) end

---Render the full terminal to the display.
function PicOSTerminal:render() end

---Render only dirty (changed) cells to the display.
function PicOSTerminal:renderDirty() end

---@return integer
function PicOSTerminal:getCols() end

---@return integer
function PicOSTerminal:getRows() end

---@param visible boolean
function PicOSTerminal:setCursorVisible(visible) end

---@param blink boolean
function PicOSTerminal:setCursorBlink(blink) end

---Select the font for this terminal.
---@param font_id integer One of the FONT_* constants
function PicOSTerminal:setFont(font_id) end

---@return integer
function PicOSTerminal:getFont() end

---Mark all cells as dirty (forces a full re-render on next `renderDirty`).
function PicOSTerminal:markAllDirty() end

---Return `true` if all cells are dirty.
---@return boolean
function PicOSTerminal:isFullDirty() end

---Return the first and last dirty row indices.
---@return integer first
---@return integer last
function PicOSTerminal:getDirtyRange() end

---Return the number of lines in the scrollback buffer.
---@return integer
function PicOSTerminal:getScrollbackCount() end

---Return the content of scrollback line `line` as an array of cell values.
---@param line integer
---@return integer[]
function PicOSTerminal:getScrollbackLine(line) end

---Return the current scrollback display offset.
---@return integer
function PicOSTerminal:getScrollbackOffset() end

---Set the scrollback display offset.
---@param offset integer
function PicOSTerminal:setScrollbackOffset(offset) end

---Block until any key is pressed. Returns the key constant.
---@return integer
function PicOSTerminal:waitForAnyKey() end

---Block until a specific key is pressed.
---@param key integer BTN_* constant
function PicOSTerminal:waitForKey(key) end

---Return the next key event without blocking, or `nil` if no key is pending.
---@return integer?
function PicOSTerminal:readKey() end

---Return the next ASCII character without blocking, or `nil`.
---@return string?
function PicOSTerminal:readChar() end

---Block until an ASCII character is typed. Returns the character.
---@return string
function PicOSTerminal:waitForChar() end

---Enable or disable line-number gutter.
---@param enabled boolean
function PicOSTerminal:setLineNumbers(enabled) end

---Set the starting line number displayed in the gutter.
---@param start integer
function PicOSTerminal:setLineNumberStart(start) end

---Set the width of the line-number gutter in characters.
---@param cols integer
function PicOSTerminal:setLineNumberCols(cols) end

---Set line-number gutter colours.
---@param fg integer RGB565
---@param bg integer RGB565
function PicOSTerminal:setLineNumberColors(fg, bg) end

---Return the number of content columns (total columns minus gutter width).
---@return integer
function PicOSTerminal:getContentCols() end

---Enable or disable the scrollbar.
---@param enabled boolean
function PicOSTerminal:setScrollbar(enabled) end

---Set scrollbar colours.
---@param bg integer RGB565 track colour
---@param thumb integer RGB565 thumb colour
function PicOSTerminal:setScrollbarColors(bg, thumb) end

---Set scrollbar width in pixels.
---@param width integer
function PicOSTerminal:setScrollbarWidth(width) end

---Update scrollbar thumb position.
---@param total_lines integer Total document line count
---@param scroll_position integer Current top-visible line
function PicOSTerminal:setScrollInfo(total_lines, scroll_position) end

---Enable or disable visual word-wrap (content is not modified).
---@param enabled boolean
function PicOSTerminal:setWordWrap(enabled) end

---Set the column at which visual word-wrap breaks.
---@param column integer
function PicOSTerminal:setWordWrapColumn(column) end

---Show or hide the wrap-continuation indicator.
---@param enabled boolean
function PicOSTerminal:setWrapIndicator(enabled) end

-- =============================================================================
-- picocalc.crypto  (hashing, symmetric encryption, key exchange)
-- =============================================================================

---@class picocalc.crypto
picocalc.crypto = {}

---@class PicOSAesCtr : userdata
local PicOSAesCtr = {}

---@class PicOSEcdh : userdata
local PicOSEcdh = {}

---Compute SHA-256. Returns a 32-byte binary string.
---@param data string
---@return string hash
function picocalc.crypto.sha256(data) end

---Compute SHA-1. Returns a 20-byte binary string.
---@param data string
---@return string hash
function picocalc.crypto.sha1(data) end

---Compute HMAC-SHA256. Returns a 32-byte binary string.
---@param key string
---@param data string
---@return string mac
function picocalc.crypto.hmacSHA256(key, data) end

---Compute HMAC-SHA1. Returns a 20-byte binary string.
---@param key string
---@param data string
---@return string mac
function picocalc.crypto.hmacSHA1(key, data) end

---Generate `n` cryptographically random bytes (max 4096).
---@param n integer
---@return string bytes
function picocalc.crypto.randomBytes(n) end

---SSH key derivation (RFC 4253 §7.2). Returns `needed_len` derived bytes.
---@param K_mpint string Shared secret as an SSH mpint
---@param H string Exchange hash
---@param session_id string Session ID
---@param letter string Single-character key type (`"A"` – `"F"`)
---@param needed_len integer Number of output bytes required
---@return string derived
function picocalc.crypto.deriveKey(K_mpint, H, session_id, letter, needed_len) end

---Create an AES-CTR context. `key` must be 16 or 32 bytes; `iv` must be 16 bytes.
---@param key string AES key (16 or 32 bytes)
---@param iv string Initial counter/nonce (16 bytes)
---@return PicOSAesCtr
function picocalc.crypto.aes_ctr_new(key, iv) end

---Create an X25519 ECDH key-exchange context.
---@return PicOSEcdh
function picocalc.crypto.ecdh_x25519_new() end

---Create a P-256 ECDH key-exchange context.
---@return PicOSEcdh
function picocalc.crypto.ecdh_p256_new() end

---Verify an RSA signature. Returns `true` if valid.
---@param pubkey string DER-encoded RSA public key
---@param sig string Signature bytes
---@param hash string Hash of the signed data
---@return boolean valid
function picocalc.crypto.rsaVerify(pubkey, sig, hash) end

---Verify an ECDSA P-256 signature. Returns `true` if valid.
---@param pubkey string P-256 public key bytes (65-byte uncompressed)
---@param sig string Signature bytes
---@param hash string Hash of the signed data
---@return boolean valid
function picocalc.crypto.ecdsaP256Verify(pubkey, sig, hash) end

-- PicOSAesCtr methods

---Encrypt or decrypt `input` (XOR stream cipher — same operation in both directions).
---@param input string
---@return string output
function PicOSAesCtr:update(input) end

---Release the AES context. Also called by the GC.
function PicOSAesCtr:free() end

-- PicOSEcdh methods

---Return the public key bytes for this key-exchange context
---(32 bytes for X25519; 65-byte uncompressed point for P-256).
---@return string pubkey
function PicOSEcdh:getPublicKey() end

---Compute the shared secret from the peer's public key.
---Returns `nil, error` on failure.
---@param remote_pubkey string Peer's public key bytes
---@return string? shared_secret
---@return string? error
function PicOSEcdh:computeShared(remote_pubkey) end

---Release the ECDH context. Also called by the GC.
function PicOSEcdh:free() end
