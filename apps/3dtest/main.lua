-- 3D Spinning Hexagon Demo
-- Tests display performance with real-time 3D graphics

local disp = picocalc.display
local input = picocalc.input
local sys = picocalc.sys
local perf = picocalc.perf

-- Screen center
local cx, cy = 160, 160

-- 3D hexagon vertices (in 3D space)
local vertices = {
    {x = 50, y = 0, z = 0},
    {x = 25, y = 43.3, z = 0},
    {x = -25, y = 43.3, z = 0},
    {x = -50, y = 0, z = 0},
    {x = -25, y = -43.3, z = 0},
    {x = 25, y = -43.3, z = 0},
}

-- Edges connecting vertices (hexagon outline + center lines)
local edges = {
    {1, 2}, {2, 3}, {3, 4}, {4, 5}, {5, 6}, {6, 1},  -- outer hexagon
    {1, 4}, {2, 5}, {3, 6},  -- center cross lines
}

-- Rotation angles
local angleX = 0
local angleY = 0
local angleZ = 0

-- Auto-rotate speeds (radians per frame)
local rotSpeed = 0.02

-- 3D rotation matrix functions
local function rotateX(v, angle)
    local c, s = math.cos(angle), math.sin(angle)
    return {
        x = v.x,
        y = v.y * c - v.z * s,
        z = v.y * s + v.z * c
    }
end

local function rotateY(v, angle)
    local c, s = math.cos(angle), math.sin(angle)
    return {
        x = v.x * c + v.z * s,
        y = v.y,
        z = -v.x * s + v.z * c
    }
end

local function rotateZ(v, angle)
    local c, s = math.cos(angle), math.sin(angle)
    return {
        x = v.x * c - v.y * s,
        y = v.x * s + v.y * c,
        z = v.z
    }
end

-- 3D to 2D projection (simple perspective)
local function project(v)
    local scale = 200 / (200 + v.z)  -- perspective scale
    return {
        x = math.floor(cx + v.x * scale),
        y = math.floor(cy + v.y * scale)
    }
end

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
        -- Auto-rotate on all axes
        angleX = angleX + rotSpeed
        angleY = angleY + rotSpeed * 0.7
        angleZ = angleZ + rotSpeed * 0.5
    else
        -- Manual control with D-pad
        local buttons = input.getButtons()
        if buttons & input.BTN_UP ~= 0 then angleX = angleX + 0.05 end
        if buttons & input.BTN_DOWN ~= 0 then angleX = angleX - 0.05 end
        if buttons & input.BTN_LEFT ~= 0 then angleY = angleY - 0.05 end
        if buttons & input.BTN_RIGHT ~= 0 then angleY = angleY + 0.05 end
        if buttons & input.BTN_F1 ~= 0 then angleZ = angleZ - 0.05 end
        if buttons & input.BTN_F2 ~= 0 then angleZ = angleZ + 0.05 end
    end
    
    -- Transform and project vertices
    local projected = {}
    for i, v in ipairs(vertices) do
        -- Apply rotations in order: X, Y, Z
        local rotated = rotateX(v, angleX)
        rotated = rotateY(rotated, angleY)
        rotated = rotateZ(rotated, angleZ)
        projected[i] = project(rotated)
    end
    
    -- Clear screen
    disp.clear(disp.BLACK)
    
    -- Draw edges
    for _, edge in ipairs(edges) do
        local v1 = projected[edge[1]]
        local v2 = projected[edge[2]]
        disp.drawLine(v1.x, v1.y, v2.x, v2.y, disp.CYAN)
    end
    
    -- Draw vertices as small filled circles (3x3 squares)
    for _, v in ipairs(projected) do
        disp.fillRect(v.x - 1, v.y - 1, 3, 3, disp.YELLOW)
    end
    
    -- Draw title and mode
    local modeText = mode == 1 and "AUTO" or "MANUAL"
    pc.ui.drawHeader("3D Hexagon Test - " .. modeText)
    
    -- Show help for 3 seconds
    if showHelp then
        helpTimer = helpTimer + 1
        if helpTimer < 180 then  -- ~3 seconds at 60fps
            local y = 230
            if mode == 1 then
                disp.drawText(4, y, "ENTER: Manual mode", disp.GRAY, disp.BLACK)
            else
                disp.drawText(4, y, "D-Pad: Rotate X/Y", disp.GRAY, disp.BLACK)
                disp.drawText(4, y + 10, "F1/F2: Rotate Z", disp.GRAY, disp.BLACK)
                disp.drawText(4, y + 20, "ENTER: Auto mode", disp.GRAY, disp.BLACK)
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
    sys.sleep(20)  -- Target ~50 FPS (1000ms / 50 = 20ms per frame)
end
