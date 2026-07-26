# API — Video Playback

PicOS supports MJPEG video playback from AVI files stored on the SD card. Video is decoded on Core 0 using JPEGDEC and displayed via the standard display framebuffer. Audio tracks (MP3) are decoded on Core 1 via a shared ring buffer.

---

## Requirements

Add `"video"` (no explicit requirement needed) or `"audio"` to your `app.json` if your video has an audio track:

```json
{
  "requirements": ["audio"]
}
```

---

## Lua API

### Creating a player

```lua
local player = picocalc.video.player()
```

#### `picocalc.video.player()`
Creates a new video player instance.

- **Returns:** (userdata) Video player object, or `nil, errstr` on failure

### Loading and playing

```lua
player:load("/apps/myvideo/clip.avi")  -- path on SD card
player:play()
player:stop()
-- no destroy(): the player is freed automatically by Lua garbage collection
```

### Playback control

```lua
player:setLoop(true)         -- loop continuously (default: false)
player:seek(frame_number)    -- jump to a specific frame index
```

#### `player:pause()`
Pauses video playback. Audio is also paused.

#### `player:resume()`
Resumes video playback after a pause.

---

### Performance and stats

#### `player:getFPS()`
Returns the actual playback frame rate.

- **Returns:** (number) Frames per second

```lua
local fps = player:getFPS()
picocalc.sys.log("FPS: " .. string.format("%.1f", fps))
```

#### `player:getSize()`
Returns the video frame dimensions.

- **Returns:** (number, number) Width and height in pixels

```lua
local w, h = player:getSize()
```

#### `player:getDroppedFrames()`
Returns the number of frames dropped during playback due to decode not keeping up with the frame rate.

- **Returns:** (number) Number of dropped frames

#### `player:resetStats()`
Resets playback statistics (dropped frame counter).

#### `player:setAutoFlush(enabled)`
Controls whether the player automatically calls `display.flush()` after each decoded frame. When disabled, the app must call `display.flush()` manually, allowing HUD overlays to be drawn between decode and flush.

- **Parameters:**
  - `enabled` (boolean): `true` for automatic flush (default), `false` for manual flush

```lua
player:setAutoFlush(false)  -- manual flush for HUD overlay
```

---

### Audio control

```lua
player:hasAudio()            -- true if the AVI file contains an MP3 audio track
player:setMuted(true)        -- mute audio (video still plays)
player:isMuted()             -- returns current mute state
player:setVolume(vol)        -- 0–255
player:getVolume()           -- returns current volume
```

### State

```lua
player:isPlaying()           -- true while playing
player:isPaused()            -- true while paused
local info = player:getInfo()
-- info.width, info.height   — frame dimensions
-- info.frames               — total frames in file
-- info.current_frame        — current playback position
-- info.dropped_frames       — frames dropped during playback
-- info.has_audio            — whether an audio track was found
```

### Game loop integration

Call `player:update()` each frame to advance playback:

```lua
local player = picocalc.video.player()
player:load("/apps/demo/video.avi")
player:play()

while true do
    player:update()
    -- draw HUD or overlays here
    picocalc.display.flush()

    picocalc.input.update()
    local pressed = picocalc.input.getButtonsPressed()
    if pressed & picocalc.input.BTN_ESC ~= 0 then break end
end

player:stop()
```

---

## Video format

PicOS plays **MJPEG AVI** files only. Each video frame is an independent JPEG; the player does not support inter-frame codecs (H.264, VP9, etc.).

| Property | Value |
|----------|-------|
| Container | AVI (RIFF) |
| Video codec | MJPEG (Motion JPEG) |
| Audio codec | MP3 (MPEG-1 Audio Layer III) |
| Resolution | Up to 320 × 320 (native display size) |
| Frame rate | 25 fps recommended |
| Audio sample rate | 22050 Hz or 44100 Hz |
| Audio channels | Mono or stereo |

---

## Converting video with `video_converter.sh`

The `tools/video_converter.sh` script converts any video file (or YouTube URL) to the correct AVI format using `ffmpeg`.

### Prerequisites

```bash
# Install ffmpeg (required)
sudo apt install ffmpeg          # Debian/Ubuntu
brew install ffmpeg              # macOS
sudo dnf install ffmpeg          # Fedora

# Install yt-dlp (only needed for URL downloads)
pip install yt-dlp
```

### Usage

```bash
tools/video_converter.sh [options] <file_or_url>
```

Output is written to `~/Videos/PicOS/` (the directory must exist).

### Options

| Flag | Description | Default |
|------|-------------|---------|
| `--crop` | Scale to 320×320, cropping to fill (square crop) | off (letterbox) |
| `--quality N` | MJPEG quality, 1 (best) – 31 (worst) | `8` |
| `--mute` | Strip the audio track entirely | off (include audio) |

### Examples

```bash
# Basic conversion — letterbox to 320×height, include audio
tools/video_converter.sh myclip.mp4

# Square crop for portrait/square content
tools/video_converter.sh --crop myclip.mp4

# High quality, muted (video-only)
tools/video_converter.sh --quality 3 --mute myclip.mp4

# Download and convert directly from YouTube
tools/video_converter.sh "https://www.youtube.com/watch?v=..."

# Download, crop, strip audio
tools/video_converter.sh --crop --mute "https://youtu.be/..."
```

### What the converter does

**Without `--crop`** (letterbox):
```
ffmpeg -i <input>
  -vf "fps=25,scale=320:-1"
  -vcodec mjpeg -q:v <quality> -huffman optimal
  -acodec libmp3lame -ab 128k -ar 22050
  <output>.avi
```

**With `--crop`** (square fill):
```
ffmpeg -i <input>
  -vf "fps=25,scale=320:320:force_original_aspect_ratio=increase,crop=320:320"
  -vcodec mjpeg -q:v <quality> -huffman optimal
  -acodec libmp3lame -ab 128k -ar 22050
  <output>.avi
```

**With `--mute`**: replaces the `-acodec libmp3lame ...` line with `-an` (no audio).

---

## Audio implementation notes

Audio is decoded using the **MP3 fed mode** in `mp3_player.c`. Rather than reading from an SD file directly, Core 0 (video player) pre-indexes audio chunks from the AVI file and feeds compressed MP3 data into a 64 KB ring buffer in QMI PSRAM. Core 1 reads from this ring and decodes in the normal MP3 DMA path. This avoids PIO PSRAM bus contention (PIO1 SPI is not safe across cores) and lets video and audio share the existing MP3 playback pipeline.

Audio playback is automatically stopped and the ring freed when `player:stop()` is called or the player is garbage-collected.

---

## Clock speed during playback

When `player:play()` is called, the system clock is raised from 200 MHz to **300 MHz** to fit MJPEG decode within the 40 ms frame budget. The clock is restored when `player:stop()` is called.

Because the CYW43 WiFi PIO divider cannot be updated at runtime, **WiFi is disconnected** before the overclock and reconnected (using saved credentials) after restore. Apps that require persistent WiFi connections should avoid video playback, or mute the audio and skip the overclock by using a custom playback loop.

---

## See also

- [Crash Logging and Watchdog](Crash-Logging-and-Watchdog.md) — fault recovery and reboot behavior
- [API — Audio and Sound](API-Audio-and-Sound.md) — MP3 player, sound samples, tone generation
- [API — Display and Graphics](API-Display-and-Graphics.md) — display primitives used alongside video
