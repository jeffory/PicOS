# API Audio and Sound

Audio output, including simple tones and full sample/file playback.

## picocalc.audio

Simple tone generation via PWM. Useful for beeps, alerts, and simple sound effects.

### Functions

#### `picocalc.audio.playTone(freq [, duration])`
Plays a tone at the specified frequency.

- **Parameters:**
  - `freq` (number): Frequency in Hz (e.g., `440` for concert A)
  - `duration` (number, optional): Duration in milliseconds. If `0` or omitted, the tone plays indefinitely until `stopTone()` is called.
- **Returns:** None

```lua
picocalc.audio.playTone(440, 200)   -- beep for 200ms
picocalc.audio.playTone(880)        -- start continuous tone
```

---

#### `picocalc.audio.stopTone()`
Stops any currently playing tone immediately.

- **Parameters:** None
- **Returns:** None

```lua
picocalc.audio.stopTone()
```

---

#### `picocalc.audio.setVolume(volume)`
Sets the audio output volume.

- **Parameters:**
  - `volume` (number): Volume level (0–255, where 0 is muted and 255 is maximum)
- **Returns:** None

```lua
picocalc.audio.setVolume(128)  -- 50% volume
```

---

## picocalc.sound

Full audio playback system supporting WAV samples and MP3 files. Provides three player types:
- **SamplePlayer** — plays a pre-loaded WAV sample from memory
- **FilePlayer** — streams a WAV file from the SD card
- **MP3Player** — streams an MP3 file from the SD card

### Top-Level Functions

#### `picocalc.sound.getCurrentTime()`
Returns the current audio clock time in milliseconds since the last `resetTime()` call.

- **Returns:** (number) Milliseconds

---

#### `picocalc.sound.resetTime()`
Resets the audio clock to zero.

- **Returns:** None

---

### Sample

A `Sample` holds raw PCM audio data loaded from a WAV file.

#### `picocalc.sound.sample([path])`
Creates a new Sample object, optionally loading a WAV file immediately.

- **Parameters:**
  - `path` (string, optional): Absolute path to a WAV file
- **Returns:** (userdata) Sample object, or `nil, errstr` on failure

```lua
local s = picocalc.sound.sample("/apps/myapp/beep.wav")
```

---

#### `sample:load(path)`
Loads a WAV file into the sample.

- **Parameters:**
  - `path` (string): Absolute path to a WAV file
- **Returns:** `true` on success, or `nil, errstr` on failure

---

#### `sample:getLength()`
Returns the number of PCM samples (frames).

- **Returns:** (number)

---

#### `sample:getSampleRate()`
Returns the sample rate in Hz (e.g., `44100`).

- **Returns:** (number)

---

### SamplePlayer

Plays a `Sample` from memory. Supports looping and volume control.

#### `picocalc.sound.sampleplayer([sample_or_path])`
Creates a SamplePlayer, optionally pre-loading a sample.

- **Parameters:**
  - `sample_or_path` (userdata or string, optional): A `Sample` object or a WAV file path
- **Returns:** (userdata) SamplePlayer object, or `nil, errstr` on failure

```lua
local player = picocalc.sound.sampleplayer("/apps/myapp/beep.wav")
player:play()
```

---

#### `player:setSample(sample)`
Sets the sample to play.

- **Parameters:**
  - `sample` (userdata): A `Sample` object
- **Returns:** `true` on success, or `nil, errstr`

---

#### `player:play([repeat])`
Starts playback.

- **Parameters:**
  - `repeat` (number, optional): Number of times to repeat. `0` loops indefinitely.
- **Returns:** `true` if started

---

#### `player:stop()`
Stops playback.

---

#### `player:isPlaying()`
- **Returns:** (boolean)

---

#### `player:setVolume(vol)` / `player:getVolume()`
Volume range 0–255.

---

### FilePlayer

Streams a WAV file from the SD card without loading it fully into memory.

#### `picocalc.sound.fileplayer([bufferSize])`
Creates a FilePlayer.

- **Parameters:**
  - `bufferSize` (number, optional): Internal streaming buffer size in bytes
- **Returns:** (userdata) FilePlayer object, or `nil, errstr` on failure

---

#### `player:load(path)`
Opens a WAV file for streaming.

- **Parameters:**
  - `path` (string): Absolute path to a WAV file
- **Returns:** `true` on success, or `nil, errstr`

---

#### `player:play([repeat])` / `player:stop()` / `player:pause()` / `player:isPlaying()`
Standard playback controls. `repeat` works the same as SamplePlayer.

---

#### `player:getLength()` / `player:getOffset()` / `player:setOffset(seconds)`
Returns or seeks to a position in seconds.

---

#### `player:setVolume(left [, right])` / `player:getVolume()`
Sets left/right channel volumes (0–255). Returns both channels.

---

#### `player:setLoopRange([start [, end]])`
Sets the loop region in seconds. Omit both to loop the whole file.

---

### MP3Player

Streams an MP3 file from the SD card.

#### `picocalc.sound.mp3player()`
Creates an MP3Player.

- **Returns:** (userdata) MP3Player object, or `nil, errstr` on failure

```lua
local mp3 = picocalc.sound.mp3player()
mp3:load("/apps/myapp/music.mp3")
mp3:play()
```

---

#### `player:load(path)`
Opens an MP3 file for streaming.

- **Returns:** `true` on success, or `nil, errstr`

---

#### `player:play([repeat])` / `player:stop()` / `player:pause()` / `player:resume()` / `player:isPlaying()`
Standard playback controls.

---

#### `player:getPosition()` / `player:getLength()`
Returns current playback position or total duration in seconds.

---

#### `player:setVolume(vol)` / `player:getVolume()`
Volume range 0–255.

---

#### `player:setLoop(loop)`
- **Parameters:**
  - `loop` (boolean): `true` to loop continuously
