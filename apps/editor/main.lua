-- Text Editor for PicoCalc OS
-- Nano-like text editor with full editing capabilities

local pc    = picocalc
local disp  = pc.display
local input = pc.input
local fs    = pc.fs
local sys   = pc.sys

-- ── Display constants ─────────────────────────────────────────────────────────
local SCREEN_W = disp.getWidth()
local SCREEN_H = disp.getHeight()
local CHAR_W = 6
local CHAR_H = 8
local COLS = math.floor(SCREEN_W / CHAR_W)  -- ~53 chars
local ROWS = math.floor(SCREEN_H / CHAR_H)  -- 40 lines

-- Layout
local STATUS_ROWS  = 1
local COMMAND_ROWS = 1
local TEXT_ROWS    = ROWS - STATUS_ROWS - COMMAND_ROWS  -- 38 lines
local LINE_NUM_WIDTH = 5  -- "1234 " = 5 chars for line numbers
local COLS_FULL    = COLS
local COLS_WITH_SCROLLBAR = COLS_FULL - 1  -- Reserve space for scrollbar

-- Colors
local BG               = disp.BLACK
local TEXT_FG          = disp.WHITE
local STATUS_BG        = disp.rgb(0, 60, 120)
local STATUS_FG        = disp.WHITE
local CMD_BG           = disp.rgb(40, 40, 40)
local CMD_FG           = disp.YELLOW
local CURSOR_C         = disp.CYAN
local MODIFIED         = disp.RED
local SCROLLBAR_C      = disp.rgb(80, 80, 80)
local SCROLLBAR_THUMB  = disp.rgb(150, 150, 150)

-- Scrollbar
local SCROLLBAR_WIDTH = 4
local SCROLLBAR_X     = SCREEN_W - SCROLLBAR_WIDTH

-- ── State ─────────────────────────────────────────────────────────────────────
local lines        = {""}   -- text buffer (array of strings, 1-indexed)
local cursor_x     = 1      -- column (1-indexed)
local cursor_y     = 1      -- line   (1-indexed)
local scroll_y     = 0      -- top visible line (0-indexed)
local scroll_x     = 0      -- horizontal scroll offset (0-indexed)
local filename     = nil    -- current file path (nil = untitled)
local modified     = false  -- dirty flag
local message      = ""
local message_time = 0

-- Editor options
local word_wrap       = false  -- Word wrap mode (F2 to toggle)
local show_line_numbers = false  -- Show line numbers (F3 to toggle)
local show_help       = false  -- Show help overlay (F1 to toggle)

-- Key repeat
local repeat_timer = 0
local repeat_delay = 15
local repeat_rate  = 3
local last_key     = 0

-- ── Inline filename prompt ─────────────────────────────────────────────────────
-- Collects a typed string in the command bar; returns the string or nil on Esc.
-- Deliberately avoids calling other local functions so it can be defined early.

local function prompt_filename(prompt_text)
    local result = ""
    while true do
        local y_base = (STATUS_ROWS + TEXT_ROWS) * CHAR_H
        disp.fillRect(0, y_base, SCREEN_W, COMMAND_ROWS * CHAR_H, CMD_BG)
        disp.drawText(2, y_base,              prompt_text,             CMD_FG,  CMD_BG)
        disp.drawText(2, y_base + CHAR_H,     result .. "_",           TEXT_FG, CMD_BG)
        disp.drawText(2, y_base + CHAR_H * 2, "Enter:confirm  Esc:cancel", CMD_FG, CMD_BG)
        disp.flush()

        input.update()
        local pressed = input.getButtonsPressed()
        local char    = input.getChar()

        if pressed & input.BTN_ENTER ~= 0 then
            if #result > 0 then return result end
        end
        if pressed & input.BTN_ESC ~= 0 then
            return nil
        end
        if (pressed & input.BTN_BACKSPACE ~= 0) and #result > 0 then
            result = result:sub(1, #result - 1)
        end
        if char and char ~= "" and char ~= "\n" then
            local byte = string.byte(char)
            if byte >= 32 and byte <= 126 then
                result = result .. char
            end
        end

        sys.sleep(16)
    end
end

-- ── File operations ───────────────────────────────────────────────────────────

local function load_file(path)
    if not fs.exists(path) then
        lines = {""}
        cursor_x = 1; cursor_y = 1; scroll_y = 0; scroll_x = 0
        filename = path
        modified = false
        message = "[New File]"
        message_time = sys.getTimeMs()
        return true
    end

    local handle = fs.open(path, "r")
    if not handle then
        message = "Error: Cannot open file"
        message_time = sys.getTimeMs()
        return false
    end

    lines = {}
    while true do
        local chunk = fs.read(handle, 512)
        if not chunk or #chunk == 0 then break end

        local start = 1
        for i = 1, #chunk do
            if chunk:sub(i, i) == '\n' then
                table.insert(lines, chunk:sub(start, i - 1))
                start = i + 1
            end
        end
        if start <= #chunk then
            local rest = chunk:sub(start)
            if #lines == 0 then
                table.insert(lines, rest)
            else
                lines[#lines] = lines[#lines] .. rest
            end
        end
    end

    fs.close(handle)
    if #lines == 0 then lines = {""} end

    cursor_x = 1; cursor_y = 1; scroll_y = 0; scroll_x = 0
    filename = path
    modified = false
    message = "Loaded " .. path
    message_time = sys.getTimeMs()
    return true
end

local function save_file()
    if not filename then
        -- Untitled buffer — ask for a filename, then save into the app data dir.
        local name = prompt_filename("Save As (filename):")
        if not name then
            message = "Save cancelled."
            message_time = sys.getTimeMs()
            return false
        end
        filename = fs.appPath(name)
    end

    local handle = fs.open(filename, "w")
    if not handle then
        message = "Error: Cannot write " .. filename
        message_time = sys.getTimeMs()
        return false
    end

    for i = 1, #lines do
        local line = lines[i]
        if i < #lines then line = line .. '\n' end
        fs.write(handle, line)
    end

    fs.close(handle)
    modified = false
    message = "Saved " .. filename
    message_time = sys.getTimeMs()
    return true
end

-- ── Input helpers ─────────────────────────────────────────────────────────────

local function clamp_cursor()
    if cursor_y < 1       then cursor_y = 1 end
    if cursor_y > #lines  then cursor_y = #lines end

    local line_len = #lines[cursor_y]
    if cursor_x < 1            then cursor_x = 1 end
    if cursor_x > line_len + 1 then cursor_x = line_len + 1 end

    -- Simpler scrolling for word wrap mode
    if word_wrap then
        -- Just ensure cursor line is visible (rough approximation)
        if cursor_y - 1 < scroll_y then
            scroll_y = cursor_y - 1
        end
        if cursor_y - 1 >= scroll_y + TEXT_ROWS then
            scroll_y = cursor_y - TEXT_ROWS
        end
        if scroll_y < 0 then scroll_y = 0 end
        scroll_x = 0  -- No horizontal scroll in wrap mode
    else
        -- Original scrolling with horizontal support
        if cursor_y - 1 < scroll_y then
            scroll_y = cursor_y - 1
        end
        if cursor_y - 1 >= scroll_y + TEXT_ROWS then
            scroll_y = cursor_y - TEXT_ROWS
        end
        if scroll_y < 0 then scroll_y = 0 end
        
        -- Adjust horizontal scroll to keep cursor visible
        local visible_cols = (#lines > TEXT_ROWS) and COLS_WITH_SCROLLBAR or COLS_FULL
        if show_line_numbers then
            visible_cols = visible_cols - LINE_NUM_WIDTH
        end
        if cursor_x - 1 < scroll_x then
            scroll_x = math.max(0, cursor_x - 1)
        elseif cursor_x - 1 >= scroll_x + visible_cols then
            scroll_x = cursor_x - visible_cols
        end
    end
end

local function move_cursor(dx, dy)
    if dy ~= 0 then
        cursor_y = cursor_y + dy
        scroll_x = 0  -- Reset horizontal scroll when changing lines
        clamp_cursor()
        local line_len = #lines[cursor_y]
        if cursor_x > line_len + 1 then cursor_x = line_len + 1 end
    elseif dx ~= 0 then
        cursor_x = cursor_x + dx
        if cursor_x < 1 and cursor_y > 1 then
            cursor_y = cursor_y - 1
            cursor_x = #lines[cursor_y] + 1
            scroll_x = 0
        elseif cursor_x > #lines[cursor_y] + 1 and cursor_y < #lines then
            cursor_y = cursor_y + 1
            cursor_x = 1
            scroll_x = 0
        end
        clamp_cursor()
    end
end

local function insert_char(ch)
    local line   = lines[cursor_y]
    local before = line:sub(1, cursor_x - 1)
    local after  = line:sub(cursor_x)
    lines[cursor_y] = before .. ch .. after
    cursor_x = cursor_x + 1
    modified = true
end

local function delete_char()
    local line = lines[cursor_y]
    if cursor_x <= #line then
        lines[cursor_y] = line:sub(1, cursor_x - 1) .. line:sub(cursor_x + 1)
        modified = true
    elseif cursor_y < #lines then
        lines[cursor_y] = line .. lines[cursor_y + 1]
        table.remove(lines, cursor_y + 1)
        modified = true
    end
end

local function backspace()
    if cursor_x > 1 then
        local line = lines[cursor_y]
        lines[cursor_y] = line:sub(1, cursor_x - 2) .. line:sub(cursor_x)
        cursor_x = cursor_x - 1
        modified = true
    elseif cursor_y > 1 then
        local prev_len = #lines[cursor_y - 1]
        lines[cursor_y - 1] = lines[cursor_y - 1] .. lines[cursor_y]
        table.remove(lines, cursor_y)
        cursor_y = cursor_y - 1
        cursor_x = prev_len + 1
        modified = true
        clamp_cursor()
    end
end

local function insert_newline()
    local line   = lines[cursor_y]
    local before = line:sub(1, cursor_x - 1)
    local after  = line:sub(cursor_x)
    lines[cursor_y] = before
    table.insert(lines, cursor_y + 1, after)
    cursor_y = cursor_y + 1
    cursor_x = 1
    scroll_x = 0
    modified = true
    clamp_cursor()
end

-- ── Drawing ───────────────────────────────────────────────────────────────────

local function draw_status_bar()
    disp.fillRect(0, 0, SCREEN_W, CHAR_H, STATUS_BG)

    local fn         = filename or "[Untitled]"
    local mod_marker = modified and " [+]" or ""
    disp.drawText(2, 0, fn .. mod_marker, STATUS_FG, STATUS_BG)

    local pos   = string.format("L%d C%d", cursor_y, cursor_x)
    local pos_x = SCREEN_W - (#pos * CHAR_W) - 2
    disp.drawText(pos_x, 0, pos, STATUS_FG, STATUS_BG)
end

local function draw_text_area()
    local y_offset = STATUS_ROWS * CHAR_H
    local visible_cols = (#lines > TEXT_ROWS) and COLS_WITH_SCROLLBAR or COLS_FULL
    local text_x = 0  -- X offset for text (after line numbers if enabled)
    
    -- Adjust for line numbers
    if show_line_numbers then
        visible_cols = visible_cols - LINE_NUM_WIDTH
        text_x = LINE_NUM_WIDTH * CHAR_W
    end
    
    if word_wrap then
        -- Word wrap mode: lines can span multiple screen rows
        local screen_row = 0
        local line_idx = scroll_y + 1
        
        while screen_row < TEXT_ROWS and line_idx <= #lines do
            local line = lines[line_idx]
            local y = y_offset + screen_row * CHAR_H
            
            -- Draw line number on first row of each wrapped line
            if show_line_numbers then
                local line_num = string.format("%4d ", line_idx)
                disp.drawText(0, y, line_num, disp.rgb(100, 100, 100), BG)
            end
            
            -- Calculate how many rows this line needs when wrapped
            local line_len = #line
            local rows_needed = math.max(1, math.ceil(line_len / visible_cols))
            
            -- Draw each wrapped segment
            for row_offset = 0, rows_needed - 1 do
                if screen_row >= TEXT_ROWS then break end
                
                local start_col = row_offset * visible_cols + 1
                local end_col = math.min(start_col + visible_cols - 1, line_len)
                local segment = line:sub(start_col, end_col)
                
                local seg_y = y_offset + screen_row * CHAR_H
                disp.drawText(text_x, seg_y, segment, TEXT_FG, BG)
                
                -- Draw cursor if on this line and in this segment
                if line_idx == cursor_y and cursor_x >= start_col and cursor_x <= end_col + 1 then
                    local cx = text_x + (cursor_x - start_col) * CHAR_W
                    disp.fillRect(cx, seg_y, CHAR_W, CHAR_H, CURSOR_C)
                    if cursor_x <= line_len then
                        disp.drawText(cx, seg_y, line:sub(cursor_x, cursor_x), BG, CURSOR_C)
                    end
                end
                
                screen_row = screen_row + 1
            end
            
            line_idx = line_idx + 1
        end
    else
        -- Horizontal scroll mode (original behavior)
        for i = 1, TEXT_ROWS do
            local line_idx = scroll_y + i
            if line_idx <= #lines then
                local line = lines[line_idx]
                local y = y_offset + (i - 1) * CHAR_H
                
                -- Draw line number
                if show_line_numbers then
                    local line_num = string.format("%4d ", line_idx)
                    disp.drawText(0, y, line_num, disp.rgb(100, 100, 100), BG)
                end
                
                -- Apply horizontal scroll for current line
                local view_start = (line_idx == cursor_y) and scroll_x or 0
                local view_end = view_start + visible_cols
                local visible_line = line:sub(view_start + 1, view_end)
                
                disp.drawText(text_x, y, visible_line, TEXT_FG, BG)

                if line_idx == cursor_y then
                    local cx = text_x + (cursor_x - 1 - scroll_x) * CHAR_W
                    if cx >= text_x and cx < text_x + visible_cols * CHAR_W then
                        disp.fillRect(cx, y, CHAR_W, CHAR_H, CURSOR_C)
                        if cursor_x <= #line then
                            disp.drawText(cx, y, line:sub(cursor_x, cursor_x), BG, CURSOR_C)
                        end
                    end
                end
            end
        end
    end
end

local function draw_scrollbar()
    if #lines <= TEXT_ROWS then return end

    local y_offset   = STATUS_ROWS * CHAR_H
    local scrollbar_h = TEXT_ROWS * CHAR_H
    disp.fillRect(SCROLLBAR_X, y_offset, SCROLLBAR_WIDTH, scrollbar_h, SCROLLBAR_C)

    local visible_ratio = TEXT_ROWS / #lines
    local thumb_h       = math.max(8, math.floor(scrollbar_h * visible_ratio))
    local scroll_ratio  = scroll_y / (#lines - TEXT_ROWS)
    local thumb_y       = y_offset + math.floor((scrollbar_h - thumb_h) * scroll_ratio)
    disp.fillRect(SCROLLBAR_X, thumb_y, SCROLLBAR_WIDTH, thumb_h, SCROLLBAR_THUMB)
end

local function draw_command_bar()
    local y_base = (STATUS_ROWS + TEXT_ROWS) * CHAR_H
    disp.fillRect(0, y_base, SCREEN_W, COMMAND_ROWS * CHAR_H, CMD_BG)

    -- Show message if recent, otherwise show help hint
    if message ~= "" and sys.getTimeMs() - message_time < 3000 then
        disp.drawText(2, y_base, message, MODIFIED, CMD_BG)
    else
        disp.drawText(2, y_base, "F1: Help", CMD_FG, CMD_BG)
    end
end

local function draw_help_overlay()
    -- Semi-transparent background box
    local box_x = 10
    local box_y = 40
    local box_w = SCREEN_W - 20
    local box_h = SCREEN_H - 80
    local help_bg = disp.rgb(20, 20, 40)
    local help_fg = disp.WHITE
    local help_title = disp.YELLOW
    
    disp.fillRect(box_x, box_y, box_w, box_h, help_bg)
    disp.drawRect(box_x, box_y, box_w, box_h, help_title)
    
    local line_y = box_y + 4
    local line_height = CHAR_H
    
    local function help_line(text, color)
        color = color or help_fg
        disp.drawText(box_x + 4, line_y, text, color, help_bg)
        line_y = line_y + line_height
    end
    
    help_line("EDITOR HELP", help_title)
    line_y = line_y + 2
    help_line("File Operations:")
    help_line("  Ctrl+S  Save file")
    help_line("  Ctrl+Q  Quit")
    help_line("  Ctrl+O  Open file")
    help_line("  Ctrl+N  New file")
    line_y = line_y + 2
    help_line("Editing:")
    help_line("  Ctrl+K  Delete current line")
    help_line("  Arrows  Move cursor")
    help_line("  Enter   New line")
    help_line("  Bksp    Delete char")
    line_y = line_y + 2
    help_line("View Options:")
    help_line("  F2      Toggle word wrap " .. (word_wrap and "[ON]" or "[OFF]"))
    help_line("  F3      Toggle line numbers " .. (show_line_numbers and "[ON]" or "[OFF]"))
    line_y = line_y + 2
    help_line("Press F1 or Esc to close", help_title)
end

-- ── Main loop ─────────────────────────────────────────────────────────────────

local function main()
    -- On startup, browse for a file to open.
    -- Esc from the browser starts a new empty buffer.
    local startup_file = fs.browse()
    if startup_file then
        load_file(startup_file)
    else
        lines = {""}
        cursor_x = 1; cursor_y = 1; scroll_y = 0; scroll_x = 0
        filename = nil; modified = false
        message = "[New File]  Ctrl+S to save"
        message_time = sys.getTimeMs()
    end

    -- System menu items
    sys.addMenuItem("Save & Quit", function()
        if save_file() then sys.exit() end
    end)
    sys.addMenuItem("Quit without saving", function()
        sys.exit()
    end)

    while true do
        input.update()
        local pressed = input.getButtonsPressed()
        local held    = input.getButtons()
        local char    = input.getChar()

        -- Key repeat
        if held ~= 0 and held == last_key then
            repeat_timer = repeat_timer + 1
        else
            repeat_timer = 0
            last_key = held
        end
        local do_repeat = repeat_timer > repeat_delay and
                          (repeat_timer - repeat_delay) % repeat_rate == 0

        -- ── Command keys ──────────────────────────────────────────────────────

        -- F1 = Toggle help overlay
        if pressed & input.BTN_F1 ~= 0 then
            show_help = not show_help
        end
        
        -- F2 = Toggle word wrap
        if pressed & input.BTN_F2 ~= 0 then
            word_wrap = not word_wrap
            scroll_x = 0  -- Reset horizontal scroll when toggling
            message = "Word wrap " .. (word_wrap and "enabled" or "disabled")
            message_time = sys.getTimeMs()
        end
        
        -- F3 = Toggle line numbers
        if pressed & input.BTN_F3 ~= 0 then
            show_line_numbers = not show_line_numbers
            message = "Line numbers " .. (show_line_numbers and "enabled" or "disabled")
            message_time = sys.getTimeMs()
        end
        
        -- Esc = quit (with unsaved-changes guard) or close help
        if pressed & input.BTN_ESC ~= 0 then
            if show_help then
                show_help = false
            elseif modified then
                message = "Unsaved changes — save first (Ctrl+S)"
                message_time = sys.getTimeMs()
            else
                return
            end
        end

        -- Ctrl+<letter> shortcuts
        if held & input.BTN_CTRL ~= 0 and char then
            local lower = char:lower()

            -- Ctrl+S = Save
            if lower == 's' then
                save_file()

            -- Ctrl+Q = Quit
            elseif lower == 'q' then
                if modified then
                    message = "Unsaved changes — Ctrl+S to save"
                    message_time = sys.getTimeMs()
                else
                    return
                end

            -- Ctrl+O = Open (browse for a file)
            elseif lower == 'o' then
                if modified then
                    message = "Save first (Ctrl+S)"
                    message_time = sys.getTimeMs()
                else
                    local path = fs.browse()
                    if path then load_file(path) end
                end

            -- Ctrl+N = New empty buffer
            elseif lower == 'n' then
                if modified then
                    message = "Save first (Ctrl+S)"
                    message_time = sys.getTimeMs()
                else
                    lines = {""}
                    cursor_x = 1; cursor_y = 1; scroll_y = 0; scroll_x = 0
                    filename = nil; modified = false
                    message = "[New File]  Ctrl+S to save"
                    message_time = sys.getTimeMs()
                end

            -- Ctrl+K = Delete current line
            elseif lower == 'k' then
                if cursor_y <= #lines then
                    table.remove(lines, cursor_y)
                    if #lines == 0 then lines = {""} end
                    if cursor_y > #lines then cursor_y = #lines end
                    cursor_x = 1
                    modified = true
                    clamp_cursor()
                end
            end
        end

        -- ── Navigation ────────────────────────────────────────────────────────

        if (pressed & input.BTN_UP    ~= 0) or (do_repeat and held & input.BTN_UP    ~= 0) then move_cursor(0, -1) end
        if (pressed & input.BTN_DOWN  ~= 0) or (do_repeat and held & input.BTN_DOWN  ~= 0) then move_cursor(0,  1) end
        if (pressed & input.BTN_LEFT  ~= 0) or (do_repeat and held & input.BTN_LEFT  ~= 0) then move_cursor(-1, 0) end
        if (pressed & input.BTN_RIGHT ~= 0) or (do_repeat and held & input.BTN_RIGHT ~= 0) then move_cursor( 1, 0) end

        -- Fn = Home (go to start of line)
        if pressed & input.BTN_FN ~= 0 then
            cursor_x = 1
            scroll_x = 0
        end

        -- Tab = Page Up
        if pressed & input.BTN_TAB ~= 0 then move_cursor(0, -TEXT_ROWS) end

        -- ── Editing ───────────────────────────────────────────────────────────

        if pressed & input.BTN_ENTER ~= 0 then
            insert_newline()
        end

        if (pressed & input.BTN_BACKSPACE ~= 0) or (do_repeat and held & input.BTN_BACKSPACE ~= 0) then
            backspace()
        end

        if pressed & input.BTN_DEL ~= 0 then
            delete_char()
        end

        -- Insert printable characters (skip when Ctrl is held)
        if char and char ~= "" and char ~= "\n" and (held & input.BTN_CTRL == 0) then
            local byte = string.byte(char)
            if byte >= 32 and byte <= 126 then
                insert_char(char)
            end
        end

        clamp_cursor()

        -- ── Draw ──────────────────────────────────────────────────────────────

        disp.clear(BG)
        draw_status_bar()
        draw_text_area()
        draw_scrollbar()
        draw_command_bar()
        
        -- Draw help overlay on top if active
        if show_help then
            draw_help_overlay()
        end
        
        disp.flush()
    end
end

-- ── Entry point ───────────────────────────────────────────────────────────────

main()
