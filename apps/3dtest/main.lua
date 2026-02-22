-- 3D Spinning Icosahedron Demo
-- Tests display performance with real-time 3D graphics

local pc = picocalc
local disp = picocalc.display
local gfx = picocalc.graphics
local input = picocalc.input
local perf  = picocalc.perf

-- Screen centre
local cx, cy = 160, 160

-- Regular icosahedron: 12 vertices, circumradius ~50.
-- Built from three mutually perpendicular golden rectangles.
-- φ = (1+√5)/2 ≈ 1.618; scale so circumradius = 50:
--   short half = 26.3,  long half = 42.5  (= φ × 26.3)
local vertices = {
     0,    26.3,  42.5,   -- 1
     0,   -26.3,  42.5,   -- 2
     0,    26.3, -42.5,   -- 3
     0,   -26.3, -42.5,   -- 4
    26.3,  42.5,   0,     -- 5
   -26.3,  42.5,   0,     -- 6
    26.3, -42.5,   0,     -- 7
   -26.3, -42.5,   0,     -- 8
    42.5,   0,    26.3,   -- 9
   -42.5,   0,    26.3,   -- 10
    42.5,   0,   -26.3,   -- 11
   -42.5,   0,   -26.3,   -- 12
}

-- 30 edges (each pair listed once, lower index first)
local edges = {
    1,2,  1,5,  1,6,  1,9,  1,10,   -- vertex 1 fan
    2,7,  2,8,  2,9,  2,10,          -- vertex 2 new
    3,4,  3,5,  3,6,  3,11, 3,12,   -- vertex 3 fan
    4,7,  4,8,  4,11, 4,12,          -- vertex 4 new
    5,6,  5,9,  5,11,                -- vertex 5 new
    6,10, 6,12,                       -- vertex 6 new
    7,8,  7,9,  7,11,                -- vertex 7 new
    8,10, 8,12,                       -- vertex 8 new
    9,11,                             -- vertex 9 new
    10,12,                            -- vertex 10 new
}

-- Rotation angles (radians)
local angleX = 0
local angleY = 0
local angleZ = 0

-- Auto-rotate speeds (radians per frame)
local rotSpeed = 0.02

-- Main loop
local mode = 1  -- 1=auto rotate, 2=manual control
local showHelp = true
local helpTimer = 0

while true do
    perf.beginFrame()
    input.update()

    -- Input handling
    local pressed = input.getButtonsPressed()
    if pressed & input.BTN_ESC ~= 0 then
        return  -- Exit to launcher
    end

    if pressed & input.BTN_ENTER ~= 0 then
        mode = (mode == 1) and 2 or 1
        showHelp = true
        helpTimer = 0
    end

    -- Update rotation
    if mode == 1 then
        angleX = angleX + rotSpeed
        angleY = angleY + rotSpeed * 0.7
        angleZ = angleZ + rotSpeed * 0.5
    else
        local buttons = input.getButtons()
        if buttons & input.BTN_UP    ~= 0 then angleX = angleX + 0.05 end
        if buttons & input.BTN_DOWN  ~= 0 then angleX = angleX - 0.05 end
        if buttons & input.BTN_LEFT  ~= 0 then angleY = angleY - 0.05 end
        if buttons & input.BTN_RIGHT ~= 0 then angleY = angleY + 0.05 end
        if buttons & input.BTN_F1    ~= 0 then angleZ = angleZ - 0.05 end
        if buttons & input.BTN_F2    ~= 0 then angleZ = angleZ + 0.05 end
    end

    -- Clear screen
    disp.clear(disp.BLACK)

    -- Transform, project, and draw all edges + vertex dots in one C call.
    -- Replaces: 24 Lua function calls, 24 table allocs, 15 bridge round-trips.
    gfx.draw3DWireframe(
        vertices, edges,
        angleX, angleY, angleZ,
        cx, cy, 200,
        disp.CYAN, disp.YELLOW, 3)

    -- Draw title and mode
    local modeText = mode == 1 and "AUTO" or "MANUAL"
    pc.ui.drawHeader("3D Icosahedron - " .. modeText)

    -- Show help for 3 seconds
    if showHelp then
        helpTimer = helpTimer + 1
        if helpTimer < 180 then
            local y = 230
            if mode == 1 then
                disp.drawText(4, y, "ENTER: Manual mode", disp.GRAY, disp.BLACK)
            else
                disp.drawText(4, y,      "D-Pad: Rotate X/Y", disp.GRAY, disp.BLACK)
                disp.drawText(4, y + 10, "F1/F2: Rotate Z",   disp.GRAY, disp.BLACK)
                disp.drawText(4, y + 20, "ENTER: Auto mode",  disp.GRAY, disp.BLACK)
            end
        else
            showHelp = false
        end
    end

    pc.ui.drawFooter("ENTER: Toggle Mode", "ESC: Exit")

    -- FPS counter (top right)
    perf.drawFPS(250, 4)

    -- Flush to display
    disp.flush()

    perf.endFrame()
end
