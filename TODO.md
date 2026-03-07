# PicOS TODO

## Lua API gaps — C exists, not wired to Lua

These are fully implemented in C but missing from `lua_bridge.c`. All are quick wins.

### `picocalc.sys`

| Function | Status | Notes |
|---|---|---|
| `sys.reboot()` | **[done]** | Watchdog-based reboot added to `l_sys_lib` |
| `sys.isUSBPowered()` | **[done]** | Stub returning false added to `l_sys_lib`; real VBUS check via GP24 still TODO (see System section) |

### `picocalc.fs`

| Function | Status | Notes |
|---|---|---|
| `fs.size(path)` | **[done]** | `l_fs_size` wrapper added to `l_fs_lib` |
| `fs.listDir(path)` | **[done]** | Returns `{ {name, is_dir, size}, ... }` table via `listdir_cb` callback |
| `fs.appPath(name)` | **[done]** | Returns `/data/<dirname>/<name>`; auto-creates the data directory |

### `picocalc.display`

| Function | Status | Notes |
|---|---|---|
| `display.textWidth(str)` | **[done]** | `l_display_textWidth` wrapper added to `l_display_lib` |

### `picocalc.network`

| Function | Status | Notes |
|---|---|---|
| `http.new(..., usessl=true)` | **[done]** | HTTPS support enabled via patched `altcp_tls_mbedtls_compat.c` for MbedTLS 3.x compatibility. |

---

## `g_api` struct not fully wired in `main.c`

`g_api.fs` is never assigned in `main.c` (left NULL).
The Lua bridge calls C functions directly so Lua is unaffected, but the C app loader
(future) will need this populated.

- [ ] Wire `g_api.fs = &s_fs_impl` once a `picocalc_fs_t` struct is built
- [x] Wire `g_api.audio` — implemented in this commit
- [x] Wire `g_api.wifi` — was already implemented

---

## Audio — implemented

Driver location: `src/drivers/audio.c` (created)
Hardware: PWM on GP26 (left) and GP27 (right) — defined in `hardware.h`

- [x] Create `src/drivers/audio.h` / `audio.c`
- [x] `audio_init()` — configure PWM slices on GP26/GP27
- [x] `audio_play_tone(uint32_t freq_hz, uint32_t duration_ms)` — square wave; 0ms = indefinite
- [x] `audio_stop_tone()`
- [x] `audio_set_volume(uint8_t vol)` — 0–100, logarithmic scale
- [x] Add `picocalc.audio.playTone(freq, duration)`, `stopTone()`, `setVolume(vol)` to Lua bridge
- [x] Wire `g_api.audio` in `main.c`

---

## Sound - implemented

- [x] sound.sampleplayer.new(path)
- [x] sound.sampleplayer.new(sample)
- [x] sound.sampleplayer:copy() - partial
- [x] sound.sampleplayer:play([repeatCount], [rate])
- [x] sound.sampleplayer:playAt(when, [vol], [rightvol], [rate])
- [x] sound.sampleplayer:setVolume(left, [right])
- [x] sound.sampleplayer:getVolume()
- [ ] sound.sampleplayer:setLoopCallback(callback, [arg])
- [ ] sound.sampleplayer:setPlayRange(start, end)
- [ ] sound.sampleplayer:setPaused(flag)
- [x] sound.sampleplayer:isPlaying()
- [x] sound.sampleplayer:stop()
- [ ] sound.sampleplayer:setFinishCallback(func, [arg])
- [x] sound.sampleplayer:setSample(sample)
- [ ] sound.sampleplayer:getSample()
- [ ] sound.sampleplayer:getLength()
- [ ] sound.sampleplayer:setRate(rate)
- [ ] sound.sampleplayer:getRate()
- [ ] sound.sampleplayer:setRateMod(signal)
- [ ] sound.sampleplayer:setOffset(seconds)
- [ ] sound.sampleplayer:getOffset()
 - [x] sound.fileplayer.new([buffersize]) - For music
 - [x] sound.fileplayer.new(path, [buffersize])
 - [x] sound.fileplayer:load(path)
 - [x] sound.fileplayer:play([repeatCount])
 - [x] sound.fileplayer:stop()
 - [x] sound.fileplayer:pause()
 - [x] sound.fileplayer:isPlaying()
 - [x] sound.fileplayer:getLength()
 - [ ] sound.fileplayer:setFinishCallback(func, [arg])
 - [x] sound.fileplayer:didUnderrun()
 - [ ] sound.fileplayer:setStopOnUnderrun(flag)
 - [x] sound.fileplayer:setLoopRange(start, [end, [loopCallback, [arg]]])
 - [ ] sound.fileplayer:setLoopCallback(callback, [arg])
 - [ ] sound.fileplayer:setBufferSize(seconds)
 - [ ] sound.fileplayer:setRate(rate)
 - [ ] sound.fileplayer:getRate()
 - [ ] sound.fileplayer:setRateMod(signal)
 - [x] sound.fileplayer:setVolume(left, [right, [fadeSeconds, [fadeCallback, [arg]]]])
 - [x] sound.fileplayer:getVolume()
 - [x] sound.fileplayer:setOffset(seconds)
 - [x] sound.fileplayer:getOffset()
- [x] sound.sample.new(path)
- [ ] sound.sample.new(seconds, [format])
- [ ] sound.sample:getSubsample(startOffset, endOffset)
- [x] sound.sample:load(path)
- [ ] sound.sample:decompress()
- [x] sound.sample:getSampleRate()
- [ ] sound.sample:getFormat()
- [x] sound.sample:getLength()
- [ ] sound.sample:play([repeatCount], [rate])
- [ ] sound.sample:playAt(when, [vol], [rightvol], [rate])
- [ ] sound.sample:save(filename)
- [ ] sound.signal:setOffset(offset)
- [ ] sound.signal:setScale(scale)
- [ ] sound.signal:getValue()
- [ ] sound.channel.new()
- [ ] sound.channel:remove()
- [ ] sound.channel:addEffect(effect)
- [ ] sound.channel:removeEffect(effect)
- [ ] sound.channel:addSource(source)
- [ ] sound.channel:removeSource(source)
- [ ] sound.channel:setVolume(volume)
- [ ] sound.channel:getVolume()
- [ ] sound.channel:setPan(pan)
- [ ] sound.channel:setPanMod(signal)
- [ ] sound.channel:setVolumeMod(signal)
- [ ] sound.channel:getDryLevelSignal()
- [ ] sound.channel:getWetLevelSignal()
- [ ] sound.playingSources() - Return a list of sources playing
- [x] sound.getCurrentTime() - Returns the current time, in seconds, as measured by the audio device
- [x] sound.resetTime() - Resets the audio output device time counter.

### MP3 Player (using picomp3lib)

- [x] sound.mp3player.new() - Create MP3 player instance
- [x] sound.mp3player:load(path) - Load MP3 file
- [x] sound.mp3player:play([repeatCount]) - Start playback
- [x] sound.mp3player:stop() - Stop playback
- [x] sound.mp3player:pause() - Pause playback
- [x] sound.mp3player:resume() - Resume playback
- [x] sound.mp3player:isPlaying() - Check if playing
- [x] sound.mp3player:getPosition() - Get current position in samples
- [x] sound.mp3player:getLength() - Get total length in samples
- [x] sound.mp3player:setVolume(vol) - Set volume (0-100)
- [x] sound.mp3player:getVolume() - Get current volume
- [x] sound.mp3player:setLoop(flag) - Enable/disable looping

---

## WiFi — implemented

Hardware: CYW43 on Pimoroni Pico Plus 2 W — **shares SPI1 with the LCD**.
Must use `display_spi_lock()` / `display_spi_unlock()` around all CYW43 SPI access.

- [x] Create `src/drivers/wifi.c` / `wifi.h`
- [x] `wifi_init()` — CYW43 init via `cyw43_arch_init()`, SPI mutex enabled in `display_flush()`, auto-connects from config
- [x] `wifi_connect(ssid, password)` — non-blocking; uses `cyw43_arch_wifi_connect_async()` with WPA2_MIXED_PSK
- [x] `wifi_get_status()` — returns `WIFI_STATUS_*` enum (defined in `os.h`)
- [x] `wifi_disconnect()` — calls `cyw43_wifi_leave()`
- [x] `wifi_get_ip()` / `wifi_get_ssid()` — lwip netif for IP
- [x] `wifi_is_available()` — tracks `cyw43_arch_init()` success at runtime
- [x] Expose full `picocalc.wifi.*` table to Lua with `STATUS_*` constants
- [x] Wire `g_api.wifi` in `main.c`
- [x] `pico_cyw43_arch_lwip_poll` added to `CMakeLists.txt` (conditional on WiFi-capable boards)
- [x] `wifi_poll()` called from Lua instruction hook every ~256 opcodes (background polling)

---

## System menu overlay — implemented

The Menu/Sym key (`BTN_MENU`) pauses the running app and shows an OS-level overlay.

- [x] Create `src/os/system_menu.c` / `system_menu.h`
- [x] Intercept `BTN_MENU` via Lua instruction-count hook (fires every 256 opcodes) + `kbd_consume_menu_press()` edge-detect in `keyboard.c`
- [x] Draw a translucent overlay via `display_darken()` + menu panel
- [x] Built-in items: **Brightness** (L/R/Enter adjust), **Battery** (colour-coded %), **WiFi** (stub), **Reboot**, **Exit app**
- [x] `system_menu_add_item()` / `system_menu_clear_items()` for app-registered items
- [x] Wire `g_api.sys.addMenuItem` and `clearMenuItems` in `main.c`
- [x] Expose `picocalc.sys.addMenuItem(label, fn)` and `clearMenuItems()` to Lua
- [x] Menu button also works during `sys.sleep()` (10ms polling loop)
- [ ] Implement the system menu overlay working on the main menu

---

## Display — missing features

- [ ] `display.drawBitmap(x, y, path)` — load a raw RGB565 or BMP file from SD card and blit it
- [ ] Larger/alternative font support — current 6×8 font is readable but small; add an 8×12 option
- [ ] `display.drawCircle(x, y, r, color)` — useful for apps, trivial to add
- [ ] `display.scroll(dy)` — hardware-assisted vertical scroll (ST7365P supports this)

---

## System — miscellaneous

- [ ] **Core 1 background tasks** — `core1_entry()` in `main.c` is an idle spin loop; candidates: audio mixing, WiFi polling, display DMA coordination
- [x] **Shared config** — `src/os/config.h` / `config.c`; reads/writes `/system/config.json`; flat key/value JSON; exposed as `picocalc.config.{get,set,save,load}()`
- [ ] **`sys.isUSBPowered()` implementation** — currently a stub; on RP2350 Pico, VBUS is detectable via GP24
- [x] **SD card hot-swap** — `sdcard_remount()` exists; launcher rescans on app exit; manual "Remount SD" added to system menu.

---

## System — Filesystem handling

- [x] Restrict files opened to either their own app directory or their own data directory
- [x] Add a file browser in similar styling to the system menu

---

## Code quality / housekeeping

- [ ] `g_api.fs.listDir` callback signature mismatch — `picocalc_fs_t.listDir` in `os.h` has a different signature than `sdcard_list_dir()` in `sdcard.h`; reconcile them (Lua bridge calls `sdcard_list_dir()` directly and is unaffected)
- [ ] App sandboxing: `picocalc.sys.exit()` uses a string sentinel (`__picocalc_exit__`) — could be hardened with a Lua registry light userdata instead

## Quality of life

- [x] Remember the position of the selected app/scrolling position when exiting an application
- [ ] Add a loading spinner api that apps can use

## Graphics APIs - For Lua

- [done] Basic Context & State Management (These define how the MCU interacts with the frame buffer.)
  - [x] graphics.clear(color): Wipes the current draw target.
  - [x] graphics.setColor(color) / graphics.setBackgroundColor(color): Sets global draw state.

- [done] Image Lifecycle & Memory (MCUs often need to distinguish between images stored in Flash (ROM) vs. those in RAM.)
  - [x] graphics.image.new: Allocates a buffer in RAM.
  - [x] graphics.image.load(path): Loads from SD card or SPIFFS.
  - [x] graphics.image:getSize(): Crucial for bounds checking.
  - [x] graphics.image:copy(): Clones an image buffer (use sparingly due to RAM).

- [done] Coordinate-Based Drawing (These are the workhorses of your UI.)
  - [x] graphics.image:draw(x, y, [flip, sourceRect]): Standard blitting.
  - [x] graphics.image:drawAnchored(x, y, ax, ay): Simplifies UI alignment (e.g., centering text or icons).
  - [x] graphics.image:drawTiled(rect): Great for backgrounds without using massive assets.

- [done] Spritesheets
  - [x] graphics.spritesheet.new([image])
  - [x] graphics.spritesheet.newGrid(image, cols, rows, frameWidth, frameHeight)
  - [x] spritesheet:addFrame(x, y, width, height)
  - [x] spritesheet:getFrameCount()
  - [x] spritesheet:getFrame(index)
  - [x] spritesheet:getImage()
  - [x] spritesheet:drawFrame(frameIndex, x, y [, flip])

## Optimisations

- [ ] Route direct hardware calls in `lua_bridge.c` through g_api for environment portability (Medium effort)
- [ ] Consider storing framebuffer in native endian, byte-swap only during DMA	
- [ ] Skip wifi_poll() in lua hook when WiFi is disabled

## Sprites

- [x] graphics.sprite.new([image_or_tilemap])
- [ ] graphics.sprite.spriteWithText(text, maxWidth, maxHeight, [backgroundColor, [leadingAdjustment, [truncationString, [alignment, [font]]]]])
- [x] graphics.sprite.update()
- [x] graphics.sprite:setImage(image, [flip, [scale, [yscale]]])
- [x] graphics.sprite:getImage()
- [x] graphics.sprite:add()
- [x] graphics.sprite.addSprite(sprite)
- [x] graphics.sprite:remove()
- [x] graphics.sprite.removeSprite(sprite)
- [x] graphics.sprite:moveTo(x, y)
- [x] graphics.sprite:getPosition()
- [x] graphics.sprite.x
- [x] graphics.sprite.y
- [x] graphics.sprite:moveBy(x, y)
- [x] graphics.sprite:setZIndex(z)
- [x] graphics.sprite:getZIndex()
- [x] graphics.sprite:setVisible(flag)
- [x] graphics.sprite:isVisible()
- [x] graphics.sprite:setCenter(x, y)
- [x] graphics.sprite:getCenter()
- [x] graphics.sprite:getCenterPoint()
- [x] graphics.sprite:setSize(width, height)
- [x] graphics.sprite:getSize()
- [x] graphics.sprite.width
- [x] graphics.sprite.height
- [x] graphics.sprite:setScale(scale, [yScale])
- [x] graphics.sprite:getScale()
- [x] graphics.sprite:setRotation(angle, [scale, [yScale]])
- [x] graphics.sprite:getRotation()
- [x] graphics.sprite:copy()
- [x] graphics.sprite:setUpdatesEnabled(flag)
- [x] graphics.sprite:updatesEnabled()
- [x] graphics.sprite:setTag(tag)
- [x] graphics.sprite:getTag()
- [x] graphics.sprite:setImageDrawMode(mode)
- [x] graphics.sprite:setImageFlip(flip, [flipCollideRect])
- [x] graphics.sprite:getImageFlip()
- [x] graphics.sprite:setIgnoresDrawOffset(flag)
- [x] graphics.sprite:setBounds(upper-left-x, upper-left-y, width, height)
- [x] graphics.sprite.setBounds(rect)
- [x] graphics.sprite:getBounds()
- [x] graphics.sprite:getBoundsRect()
- [x] graphics.sprite:setOpaque(flag)
- [x] graphics.sprite:isOpaque()
- [x] graphics.sprite.setBackgroundDrawingCallback(drawCallback)
- [x] graphics.sprite.redrawBackground()
- [ ] graphics.sprite:setTilemap(tilemap)
- [x] graphics.sprite:setClipRect(x, y, width, height)
- [x] graphics.sprite:setClipRect(rect)
- [x] graphics.sprite:clearClipRect()
- [ ] graphics.sprite.setClipRectsInRange(x, y, width, height, startz, endz)
- [ ] graphics.sprite.setClipRectsInRange(rect, startz, endz)
- [ ] graphics.sprite.clearClipRectsInRange(startz, endz)
- [ ] graphics.sprite:setStencilImage(stencil, [tile])
- [ ] graphics.setStencilPattern({ row1, row2, row3, row4, row5, row6, row7, row8 })
- [ ] graphics.setStencilPattern(pattern)
- [ ] graphics.sprite:setStencilPattern(level, [ditherType])
- [ ] graphics.sprite:clearStencil()
- [x] graphics.sprite.setAlwaysRedraw(flag)
- [x] graphics.sprite.getAlwaysRedraw()
- [x] graphics.sprite:markDirty()
- [x] graphics.sprite.addDirtyRect(x, y, width, height)
- [x] graphics.sprite:setRedrawsOnImageChange(flag)
- [x] graphics.sprite.getAllSprites()
- [x] graphics.sprite.performOnAllSprites(f)
- [x] graphics.sprite.spriteCount()
- [x] graphics.sprite.removeAll()
- [x] graphics.sprite.removeSprites(spriteArray)
- [x] graphics.sprite:draw(x, y, width, height)
- [x] graphics.sprite:update()
- [x] graphics.sprite:setCollideRect(x, y, width, height)
- [x] graphics.sprite:setCollideRect(rect)
- [x] graphics.sprite:getCollideRect()
- [x] graphics.sprite:getCollideBounds()
- [x] graphics.sprite:clearCollideRect()
- [x] graphics.sprite:overlappingSprites()
- [x] graphics.sprite.allOverlappingSprites()
- [ ] graphics.sprite:alphaCollision(anotherSprite)
- [x] graphics.sprite:setCollisionsEnabled(flag)
- [x] graphics.sprite:collisionsEnabled()
- [x] graphics.sprite:setGroups(groups)
- [x] graphics.sprite:setCollidesWithGroups(groups)
- [x] graphics.sprite:setGroupMask(mask)
- [x] graphics.sprite:getGroupMask()
- [x] graphics.sprite:setCollidesWithGroupsMask(mask)
- [x] graphics.sprite:getCollidesWithGroupsMask()
- [x] graphics.sprite:resetGroupMask()
- [x] graphics.sprite:resetCollidesWithGroupsMask()
- [ ] graphics.sprite:moveWithCollisions(goalX, goalY)
- [ ] graphics.sprite:moveWithCollisions(goalPoint)
- [x] graphics.sprite:checkCollisions(x, y)
- [x] graphics.sprite:checkCollisions(point)
- [ ] graphics.sprite:collisionResponse(other)
- [x] graphics.sprite.querySpritesAtPoint(x, y)
- [x] graphics.sprite.querySpritesAtPoint(p)
- [x] graphics.sprite.querySpritesInRect(x, y, width, height)
- [x] graphics.sprite.querySpritesInRect(rect)
- [x] graphics.sprite.querySpritesAlongLine(x1, y1, x2, y2)
- [ ] graphics.sprite.querySpritesAlongLine(lineSegment)
- [x] graphics.sprite.querySpriteInfoAlongLine(x1, y1, x2, y2)
- [ ] graphics.sprite.querySpriteInfoAlongLine(lineSegment)
- [ ] graphics.sprite.addEmptyCollisionSprite(r)
- [ ] graphics.sprite.addEmptyCollisionSprite(x, y, w, h)
- [ ] graphics.sprite.addWallSprites(tilemap, emptyIDs, [xOffset, yOffset])
- [ ] graphics.font.new(path)
- [ ] graphics.font.newFamily(fontPaths)
- [ ] graphics.setFont(font, [variant])
- [ ] graphics.getFont([variant])
- [ ] graphics.setFontFamily(fontFamily)
- [ ] graphics.setFontTracking(pixels)
- [ ] graphics.getFontTracking()
- [ ] graphics.getSystemFont([variant])
- [ ] graphics.font:drawText(text, x, y, [width, height], [leadingAdjustment], [wrapMode], [alignment])
- [ ] graphics.font:drawText(text, rect, [leadingAdjustment], [wrapMode], [alignment])
- [ ] graphics.font:drawTextAligned(text, x, y, alignment, [leadingAdjustment])
- [ ] graphics.font:getHeight()
- [ ] graphics.font:getTextWidth(text)
- [ ] graphics.font:setTracking(pixels)
- [ ] graphics.font:getTracking()
- [ ] graphics.font:setLeading(pixels)
- [ ] graphics.font:getLeading()
- [ ] graphics.font:getGlyph(character)
- [ ] graphics.drawText(text, x, y, [width, height], [fontFamily], [leadingAdjustment], [wrapMode], [alignment])
- [ ] graphics.drawText(text, rect, [fontFamily], [leadingAdjustment], [wrapMode], [alignment])
- [ ] graphics.drawLocalizedText(key, x, y, [width, height], [language], [leadingAdjustment], [wrapMode], [alignment])
- [ ] graphics.drawLocalizedText(key, rect, [language], [leadingAdjustment])
- [ ] graphics.getLocalizedText(key, [language])
- [ ] graphics.getTextSize(str, [fontFamily, [leadingAdjustment]])
- [ ] graphics.drawTextAligned(text, x, y, alignment, [leadingAdjustment])
- [ ] graphics.drawTextInRect(text, x, y, width, height, [leadingAdjustment, [truncationString, [alignment, [font]]]])
- [ ] graphics.drawTextInRect(text, rect, [leadingAdjustment, [truncationString, [alignment, [font]]]])
- [ ] graphics.drawLocalizedTextAligned(text, x, y, alignment, [language, [leadingAdjustment]])
- [ ] graphics.drawLocalizedTextInRect(text, x, y, width, height, [leadingAdjustment, [truncationString, [alignment, [font, [language]]]]])
- [ ] graphics.drawLocalizedTextInRect(text, rect, [leadingAdjustment, [truncationString, [alignment, [font, [language]]]]])
- [ ] graphics.getTextSizeForMaxWidth(text, maxWidth, [leadingAdjustment, [font]]])
- [ ] graphics.imageWithText(text, maxWidth, maxHeight, [backgroundColor, [leadingAdjustment, [truncationString, [alignment, [font]]]]])


- [x] graphics.animation.loop.new([interval], imageTable, [shouldLoop])
- [x] graphics.animation.loop:draw(x, y, [flip])
- [x] graphics.animation.loop:image()
- [x] graphics.animation.loop:isValid()
- [x] graphics.animation.loop:setImageTable(imageTable)
- [ ] Example: Using an animation loop to draw an animated image
- [ ] Example: Creating multiple animation states from one sprite sheet
- [ ] Example: Using an animation loop in a sprite
- [x] graphics.animator.new(duration, startValue, endValue, [easingFunction, [startTimeOffset]])
- [ ] Example: Using an animator to animate movement
- [ ] graphics.animator.new(duration, lineSegment, [easingFunction, [startTimeOffset]])
- [ ] Example: Using an animator to animate along a line
- [ ] graphics.animator.new(duration, arc, [easingFunction, [startTimeOffset]])
- [ ] graphics.animator.new(duration, polygon, [easingFunction, [startTimeOffset]])
- [ ] graphics.animator.new(durations, parts, easingFunctions, [startTimeOffset])
- [ ] Example: Using an animator with parts
- [x] graphics.animator:currentValue()
- [x] graphics.animator:valueAtTime(time)
- [x] graphics.animator:progress()
- [x] graphics.animator:reset([duration])
- [x] graphics.animator:ended()
- [x] graphics.animator.easingAmplitude
- [x] graphics.animator.easingPeriod
- [x] graphics.animator.repeatCount
- [x] graphics.animator.reverses
- [x] graphics.animation.blinker.new([onDuration, [offDuration, [loop, [cycles, [default]]]]])
- [x] graphics.animation.blinker.updateAll()
- [x] graphics.animation.blinker:update()
- [x] graphics.animation.blinker:start([onDuration, [offDuration, [loop, [cycles, [default]]]]])
- [x] graphics.animation.blinker:startLoop()
- [x] graphics.animation.blinker:stop()
- [x] graphics.animation.blinker.stopAll()
- [x] graphics.animation.blinker:remove()

# Pathfinding

- [ ] pathfinder.graph.new([nodeCount, [coordinates]])
- [ ] pathfinder.graph.new2DGrid(width, height, [allowDiagonals, [includedNodes]])
- [ ] pathfinder.graph:addNewNode(id, [x, y, [connectedNodes, weights, addReciprocalConnections]])
- [ ] pathfinder.graph:addNewNodes(count)
- [ ] pathfinder.graph:addNode(node, [connectedNodes, weights, addReciprocalConnections])
- [ ] pathfinder.graph:addNodes(nodes)
- [ ] pathfinder.graph:allNodes()
- [ ] pathfinder.graph:removeNode(node)
- [ ] pathfinder.graph:removeNodeWithXY(x, y)
- [ ] pathfinder.graph:removeNodeWithID(id)
- [ ] pathfinder.graph:nodeWithID(id)
- [ ] pathfinder.graph:nodeWithXY(x, y)
- [ ] pathfinder.graph:addConnections(connections)
- [ ] pathfinder.graph:addConnectionToNodeWithID(fromNodeID, toNodeID, weight, addReciprocalConnection)
- [ ] pathfinder.graph:removeAllConnections()
- [ ] pathfinder.graph:removeAllConnectionsFromNodeWithID(id, [removeIncoming])
- [ ] pathfinder.graph:findPath(startNode, goalNode, [heuristicFunction, [findPathToGoalAdjacentNodes]])
- [ ] pathfinder.graph:findPathWithIDs(startNodeID, goalNodeID, [heuristicFunction, [findPathToGoalAdjacentNodes]])
- [ ] pathfinder.graph:setXYForNodeWithID(id, x, y)
- [ ] pathfinder.node:addConnection(node, weight, addReciprocalConnection)
- [ ] pathfinder.node:addConnections(nodes, weights, addReciprocalConnections)
- [ ] pathfinder.node:addConnectionToNodeWithXY(x, y, weight, addReciprocalConnection)
- [ ] pathfinder.node:connectedNodes()
- [ ] pathfinder.node:removeConnection(node, [removeReciprocal])
- [ ] pathfinder.node:removeAllConnections([removeIncoming])
- [ ] pathfinder.node:setXY(x, y)
- [ ] getPowerStatus()