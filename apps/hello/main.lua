-- Hello World app for PicoCalc OS
-- Demonstrates: display drawing, input handling, basic game loop, FPS tracking

local pc = picocalc          -- short alias for convenience
local disp = pc.display
local input = pc.input
local perf = pc.perf         -- performance tracking

-- App state
local frame = 0
local blink = true

-- Colours
local BG   = disp.BLACK
local FG   = disp.WHITE
local DIM  = disp.GRAY
local ACC  = disp.CYAN

-- ── Main loop ─────────────────────────────────────────────────────────────────
-- Your app owns the screen until it returns (exits to launcher).
-- Call input.update() once per frame to read key state.
-- Call pc.sys.sleep() to cap frame rate and yield CPU.

while true do
    perf.beginFrame()  -- Start FPS timing
    
    -- Poll input (reads STM32 FIFO — must be called once per frame)
    input.update()
    local pressed = input.getButtonsPressed()

    -- Esc = exit back to launcher
    if pressed & input.BTN_ESC ~= 0 then
        return
    end

    -- ── Draw frame ──────────────────────────────────────────────────────────
    disp.clear(BG)

    -- Title
    disp.drawText(8, 8, "Hello, PicoCalc!", ACC, BG)
    disp.drawLine(8, 18, 312, 18, DIM)
    
    -- FPS display (built-in, easy!)
    perf.drawFPS()  -- defaults to top-right corner (250, 8)
    -- Or customize: perf.drawFPS(10, 30)
    -- Or manual: local fps = perf.getFPS(); disp.drawText(x, y, "FPS: " .. fps, ...)

    -- Animated blink
    blink = (frame % 60) < 30
    if blink then
        disp.drawText(100, 140, "* Hello World *", FG, BG)
    end

    -- Battery readout
    local bat = pc.sys.getBattery()
    if bat >= 0 then
        disp.drawText(8, 300, "Battery: " .. bat .. "%", DIM, BG)
    end

    -- Time
    local ms = pc.sys.getTimeMs()
    local secs = math.floor(ms / 1000)
    disp.drawText(8, 310, "Uptime: " .. secs .. "s", DIM, BG)

    -- Instructions
    disp.drawText(8, 290, "Press Esc to exit", DIM, BG)

    -- Draw a bouncing box based on frame
    local bx = 8 + math.floor(math.abs(math.sin(frame / 60 * math.pi)) * 260)
    local by = 100
    disp.fillRect(bx, by, 20, 20, ACC)

    disp.flush()

    perf.endFrame()  -- End FPS timing
    frame = frame + 1
    pc.sys.sleep(16)    -- ~60 FPS cap
end
