-- Block.exe
-- by PicOS

-- Game state
local game_state = "playing" -- "playing", "gameover"
local score = 0
local level = 1
local lines_cleared = 0
local pc = picocalc -- shorthand for easier access
-- pc.perf.setTargetFPS(40)

-- Playfield dimensions
local FIELD_WIDTH = 10
local FIELD_HEIGHT = 20
local BLOCK_SIZE = 14

-- Center the playfield on the 320x320 screen
local PLAYFIELD_X = math.floor((320 - (FIELD_WIDTH * BLOCK_SIZE)) / 2)
local PLAYFIELD_Y = math.floor((320 - (FIELD_HEIGHT * BLOCK_SIZE)) / 2)

-- 80s Neon Colors
local C = {
    BG = pc.display.rgb(10, 0, 20),
    GRID = pc.display.rgb(40, 20, 80),
    TEXT = pc.display.rgb(220, 220, 255),
    GAMEOVER = pc.display.rgb(255, 50, 50),
    -- Piece colors
    CYAN = pc.display.rgb(0, 255, 255),
    BLUE = pc.display.rgb(0, 100, 255),
    ORANGE = pc.display.rgb(255, 150, 0),
    YELLOW = pc.display.rgb(255, 255, 0),
    GREEN = pc.display.rgb(50, 255, 50),
    PURPLE = pc.display.rgb(150, 50, 255),
    PINK = pc.display.rgb(255, 50, 150),
    WHITE = pc.display.rgb(255, 255, 255)
}

-- Tetromino shapes and colors
-- Coordinates are relative to top-left of a 4x4 grid
local TETROMINOES = {
    -- I
    { rotations = { {{0,1},{1,1},{2,1},{3,1}}, {{1,0},{1,1},{1,2},{1,3}} }, color = C.CYAN },
    -- J
    { rotations = { {{0,0},{0,1},{1,1},{2,1}}, {{1,0},{2,0},{1,1},{1,2}}, {{0,1},{1,1},{2,1},{2,2}}, {{1,0},{1,1},{0,2},{1,2}} }, color = C.BLUE },
    -- L
    { rotations = { {{2,0},{0,1},{1,1},{2,1}}, {{1,0},{1,1},{1,2},{2,2}}, {{0,1},{1,1},{2,1},{0,2}}, {{0,0},{1,0},{1,1},{1,2}} }, color = C.ORANGE },
    -- O
    { rotations = { {{1,1},{2,1},{1,2},{2,2}} }, color = C.YELLOW },
    -- S
    { rotations = { {{1,0},{2,0},{0,1},{1,1}}, {{1,0},{1,1},{2,1},{2,2}} }, color = C.GREEN },
    -- T
    { rotations = { {{1,0},{0,1},{1,1},{2,1}}, {{1,0},{1,1},{2,1},{1,2}}, {{0,1},{1,1},{2,1},{1,2}}, {{1,0},{0,1},{1,1},{1,2}} }, color = C.PURPLE },
    -- Z
    { rotations = { {{0,0},{1,0},{1,1},{2,1}}, {{2,0},{1,1},{2,1},{1,2}} }, color = C.PINK }
}

-- Game variables
local playfield = {}
local current_piece = {}
local next_piece_idx = 0
local gravity_timer = 0
local gravity_speed = 500 -- ms per step down

-- Particle System
local particles = {}

-- Input handling
local last_input_time = 0
local input_delay = 120 -- ms between moves

-- Initialize a new game
function init_game()
    game_state = "playing"
    score = 0
    level = 1
    lines_cleared = 0
    gravity_speed = 500

    -- Create empty playfield grid (must initialize rows so playfield[y][x] never hits a nil row)
    playfield = {}
    for y = 1, FIELD_HEIGHT do
        playfield[y] = {}
    end
    pc.graphics.drawGrid(PLAYFIELD_X, PLAYFIELD_Y, BLOCK_SIZE, BLOCK_SIZE, FIELD_WIDTH, FIELD_HEIGHT, C.GRID)

    next_piece_idx = math.random(1, #TETROMINOES)
    spawn_new_piece()
end

-- Select next piece and spawn it at the top
function spawn_new_piece()
    current_piece = {
        shape_idx = next_piece_idx,
        rotation = 1,
        x = math.floor(FIELD_WIDTH / 2) - 1,
        y = 0
    }
    next_piece_idx = math.random(1, #TETROMINOES)

    -- Game over check
    if not is_valid_position(current_piece) then
        game_state = "gameover"
    end
end

-- Check if a piece is in a valid position (not colliding)
function is_valid_position(piece)
    local shape = TETROMINOES[piece.shape_idx]
    local rotation_data = shape.rotations[piece.rotation]

    for _, block in ipairs(rotation_data) do
        local px = piece.x + block[1]
        local py = piece.y + block[2]

        -- Check boundaries
        if px < 0 or px >= FIELD_WIDTH or py < 0 or py >= FIELD_HEIGHT then
            return false
        end

        -- Check collision with existing blocks on playfield
        if playfield[py + 1] and playfield[py + 1][px + 1] then
            return false
        end
    end
    return true
end

-- Lock the current piece into the playfield
function lock_piece()
    local shape = TETROMINOES[current_piece.shape_idx]
    local rotation_data = shape.rotations[current_piece.rotation]

    for _, block in ipairs(rotation_data) do
        local px = current_piece.x + block[1]
        local py = current_piece.y + block[2]
        if py >= 0 then
            playfield[py + 1][px + 1] = shape.color
        end
    end

    clear_full_lines()
    spawn_new_piece()
end

-- Check for and clear completed lines

-- Particle System Functions
-- particles is a flat array: {x, y, vx, vy, life_ms, color, x, y, ...}
-- 6 values per particle. Managed by pc.graphics.updateDrawParticles() in C.
local function create_line_clear_particles(row_y, num_lines)
    -- Intensity scales with the square of the number of lines for a bigger "punch"
    local num_particles = 20 * num_lines * num_lines
    local center_y_px = PLAYFIELD_Y + (row_y - 1) * BLOCK_SIZE
    local speed = 100 + num_lines * 60
    local colors = {C.PINK, C.CYAN, C.WHITE}
    local base = #particles

    for i = 1, num_particles do
        local b = base + (i - 1) * 6
        particles[b + 1] = PLAYFIELD_X + BLOCK_SIZE / 2 + math.random(0, (FIELD_WIDTH - 1) * BLOCK_SIZE)
        particles[b + 2] = center_y_px + (math.random() - 0.5) * num_lines * BLOCK_SIZE
        particles[b + 3] = (math.random() - 0.5) * speed
        particles[b + 4] = (math.random() - 0.5) * speed
        particles[b + 5] = math.random(500, 1000)
        particles[b + 6] = colors[math.random(1, 3)]
    end
end

function clear_full_lines()
    local cleared_rows = {}
    -- First pass: find all full lines
    for y = 1, FIELD_HEIGHT do
        local is_full = true
        for x = 1, FIELD_WIDTH do
            if not playfield[y][x] then
                is_full = false
                break
            end
        end
        if is_full then
            table.insert(cleared_rows, y)
        end
    end

    local lines_cleared_this_turn = #cleared_rows
    if lines_cleared_this_turn > 0 then
        -- Create particle explosion centered on the cleared lines
        local total_y = 0
        for _, y_row in ipairs(cleared_rows) do
            total_y = total_y + y_row
        end
        local avg_y_row = total_y / lines_cleared_this_turn
        create_line_clear_particles(avg_y_row, lines_cleared_this_turn)

        -- Second pass: remove the lines (from bottom to top to preserve indices)
        for i = lines_cleared_this_turn, 1, -1 do
            local y_to_remove = cleared_rows[i]
            table.remove(playfield, y_to_remove)
        end

        -- Add new empty lines at the top
        for i = 1, lines_cleared_this_turn do
            local new_row = {}
            for j = 1, FIELD_WIDTH do new_row[j] = nil end
            table.insert(playfield, 1, new_row)
        end

        -- Update score, level, etc.
        local points = {40, 100, 300, 1200}
        score = score + (points[lines_cleared_this_turn] or 1200) * level
        lines_cleared = lines_cleared + lines_cleared_this_turn
        level = math.floor(lines_cleared / 10) + 1
        gravity_speed = 500 - (level - 1) * 40
        if gravity_speed < 50 then gravity_speed = 50 end
    end
end

-- Handle player input
function handle_input()
    local now = pc.sys.getTimeMs()
    if now - last_input_time < input_delay then return end

    local buttons = pc.input.getButtons()
    local moved = false

    if buttons & pc.input.BTN_LEFT ~= 0 then
        current_piece.x = current_piece.x - 1
        if not is_valid_position(current_piece) then
            current_piece.x = current_piece.x + 1
        end
        moved = true
    elseif buttons & pc.input.BTN_RIGHT ~= 0 then
        current_piece.x = current_piece.x + 1
        if not is_valid_position(current_piece) then
            current_piece.x = current_piece.x - 1
        end
        moved = true
    end

    if buttons & pc.input.BTN_DOWN ~= 0 then
        current_piece.y = current_piece.y + 1
        if not is_valid_position(current_piece) then
            current_piece.y = current_piece.y - 1
        else
            score = score + 1 -- Small bonus for soft dropping
        end
        moved = true
    end

    local pressed = pc.input.getButtonsPressed()
    if pressed & pc.input.BTN_UP ~= 0 then -- Rotate
        current_piece.rotation = current_piece.rotation + 1
        if current_piece.rotation > #TETROMINOES[current_piece.shape_idx].rotations then
            current_piece.rotation = 1
        end
        if not is_valid_position(current_piece) then -- Basic wall kick
            current_piece.x = current_piece.x - 1
            if not is_valid_position(current_piece) then
                current_piece.x = current_piece.x + 2
                if not is_valid_position(current_piece) then
                    current_piece.x = current_piece.x - 1 -- revert
                    current_piece.rotation = current_piece.rotation - 1
                    if current_piece.rotation < 1 then
                        current_piece.rotation = #TETROMINOES[current_piece.shape_idx].rotations
                    end
                end
            end
        end
        moved = true
    end

    if pressed & pc.input.BTN_ENTER ~= 0 then -- Hard drop
        while is_valid_position(current_piece) do
            current_piece.y = current_piece.y + 1
            score = score + 2 -- Small bonus for hard dropping
        end
        current_piece.y = current_piece.y - 1
        lock_piece()
        moved = true
    end

    if moved then last_input_time = now end
end

-- Update game logic (gravity)
function update_game(delta_ms)
    if game_state ~= "playing" then return end

    gravity_timer = gravity_timer + delta_ms
    if gravity_timer >= gravity_speed then
        gravity_timer = 0
        current_piece.y = current_piece.y + 1
        if not is_valid_position(current_piece) then
            current_piece.y = current_piece.y - 1
            lock_piece()
        end
    end
end

-- Drawing functions
function draw_block(grid_x, grid_y, color, offset_x, offset_y)
    local px = (offset_x or PLAYFIELD_X) + (grid_x - 1) * BLOCK_SIZE
    local py = (offset_y or PLAYFIELD_Y) + (grid_y - 1) * BLOCK_SIZE
    pc.graphics.fillBorderedRect(px, py, BLOCK_SIZE, BLOCK_SIZE, color, C.GRID)
end

function draw_playfield()
    -- Draw the grid background (single C call instead of 200 drawRect calls)
    pc.graphics.drawGrid(PLAYFIELD_X, PLAYFIELD_Y, BLOCK_SIZE, BLOCK_SIZE, FIELD_WIDTH, FIELD_HEIGHT, C.GRID)
    -- Draw the locked blocks
    for y = 1, FIELD_HEIGHT do
        for x = 1, FIELD_WIDTH do
            if playfield[y][x] then
                draw_block(x, y, playfield[y][x])
            end
        end
    end
end

function draw_piece(piece)
    local shape = TETROMINOES[piece.shape_idx]
    local rotation_data = shape.rotations[piece.rotation]
    for _, block in ipairs(rotation_data) do
        draw_block(piece.x + block[1] + 1, piece.y + block[2] + 1, shape.color)
    end
end

function draw_ui()
    local ui_x = PLAYFIELD_X + FIELD_WIDTH * BLOCK_SIZE + 15
    pc.display.drawText(ui_x, PLAYFIELD_Y + 10, "SCORE", C.TEXT, C.BG)
    pc.display.drawText(ui_x, PLAYFIELD_Y + 20, string.format("%06d", score), C.TEXT, C.BG)

    pc.display.drawText(ui_x, PLAYFIELD_Y + 50, "LEVEL", C.TEXT, C.BG)
    pc.display.drawText(ui_x, PLAYFIELD_Y + 60, string.format("%02d", level), C.TEXT, C.BG)

    pc.display.drawText(ui_x, PLAYFIELD_Y + 90, "LINES", C.TEXT, C.BG)
    pc.display.drawText(ui_x, PLAYFIELD_Y + 100, string.format("%03d", lines_cleared), C.TEXT, C.BG)

    pc.display.drawText(ui_x, PLAYFIELD_Y + 130, "NEXT", C.TEXT, C.BG)
    local next_shape = TETROMINOES[next_piece_idx]
    local next_rotation = next_shape.rotations[1]
    for _, block in ipairs(next_rotation) do
        draw_block(block[1], block[2] + 1, next_shape.color, ui_x, PLAYFIELD_Y + 140)
    end
end

function draw_gameover()
    local text = "GAME OVER"
    local width = pc.display.textWidth(text)
    local x = math.floor((320 - width) / 2)
    local y = math.floor(320 / 2) - 20
    pc.display.drawText(x, y, text, C.GAMEOVER, C.BG)

    local restart_text = "Press ENTER to restart"
    width = pc.display.textWidth(restart_text)
    x = math.floor((320 - width) / 2)
    pc.display.drawText(x, y + 20, restart_text, C.TEXT, C.BG)
end

-- Music state
local music_player = pc.sound.mp3player()
local music_enabled = true

-- Initialize music
local function init_music()
    local success, err = music_player:load(APP_DIR .. "/background01.mp3")
    if success then
        music_player:setLoop(true)
        if music_enabled then
            music_player:play()
        end
    else
        pc.sys.log("Failed to load music: " .. tostring(err))
    end
end

-- Refresh menu items to update labels
local function refresh_menu()
    pc.sys.clearMenuItems()
    pc.sys.addMenuItem("New Game", function()
        init_game()
    end)
    
    local music_label = music_enabled and "Disable Music" or "Enable Music"
    pc.sys.addMenuItem(music_label, function()
        music_enabled = not music_enabled
        if music_enabled then
            music_player:resume()
        else
            music_player:pause()
        end
        refresh_menu()
    end)
end

-- Main game loop
init_game()
init_music()
refresh_menu()
local last_frame_time = pc.sys.getTimeMs()

while true do
    pc.perf.beginFrame()
    pc.input.update()

    local now = pc.sys.getTimeMs()
    local delta = now - last_frame_time
    last_frame_time = now

    -- Update logic
    if game_state == "playing" then
        handle_input()
        update_game(delta)
    elseif game_state == "gameover" then
        local pressed = pc.input.getButtonsPressed()
        if pressed & pc.input.BTN_ENTER ~= 0 then
            init_game()
        end
    end

    -- Drawing
    pc.display.clear(C.BG)
    draw_playfield()
    if game_state == "playing" then
        draw_piece(current_piece)
    end
    -- Update positions + draw all particles + compact dead ones — single C call
    pc.graphics.updateDrawParticles(particles, delta / 1000)
    draw_ui()

    if game_state == "gameover" then
        draw_gameover()
    end

    pc.perf.drawFPS() 
    pc.display.flush()
    pc.perf.endFrame()

    -- Exit condition
    if pc.input.getButtonsPressed() & pc.input.BTN_ESC ~= 0 then
        pc.sys.exit()
    end
end
