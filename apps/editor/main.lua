-- Text Editor for PicoCalc OS
-- Nano-like text editor with full editing capabilities
-- Large files (> LARGE_FILE_THRESHOLD bytes) are opened read-only using a
-- seek-based pager that keeps only the visible window in memory.

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
local STATUS_ROWS  = 0
local COMMAND_ROWS = 0
local TEXT_ROWS    = math.floor((SCREEN_H - 28 - 18) / CHAR_H)  -- Account for 28px header, 18px footer
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

-- ── Large-file pager constants ────────────────────────────────────────────────
-- Files larger than this are opened read-only with seek-based rendering.
-- Keeps memory use bounded regardless of file size.
local LARGE_FILE_THRESHOLD = 32768  -- 32 KB
local INDEX_STRIDE         = 64     -- record a byte offset every N lines

-- ── Unicode Sanitization ──────────────────────────────────────────────────────
local function sanitize_text(s)
    if not s then return nil end
    -- Replace common unicode sequences with ASCII equivalents
    -- Smart quotes
    s = s:gsub("\226\128\152", "'")      -- ‘
    s = s:gsub("\226\128\153", "'")      -- ’
    s = s:gsub("\226\128\156", '"')      -- “
    s = s:gsub("\226\128\157", '"')      -- ”
    -- Dashes
    s = s:gsub("\226\128\147", "-")      -- – (en dash)
    s = s:gsub("\226\128\148", "-")      -- — (em dash)
    -- Other common ones
    s = s:gsub("\194\160", " ")          -- non-breaking space
    s = s:gsub("\226\128\166", "...")    -- … (ellipsis)
    -- Unicode line separators (replace with space to avoid breaking line-based display)
    s = s:gsub("\226\128\168", " ")      -- Line separator
    s = s:gsub("\226\128\169", " ")      -- Paragraph separator
    return s
end

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

-- ── Large-file pager state ────────────────────────────────────────────────────
local large_file        = false  -- true when pager mode is active
local large_total_lines = 0      -- total line count (scanned at open)
local line_index        = {}     -- line_index[k] = byte offset of line (k-1)*INDEX_STRIDE+1
local view_cache        = {}     -- lines for scroll_y..scroll_y+TEXT_ROWS-1
local view_cache_scroll = -1     -- scroll_y when view_cache was last built

-- ── Large-file helpers ────────────────────────────────────────────────────────

-- Total lines in the current buffer (works for both modes).
local function total_lines()
    return large_file and large_total_lines or #lines
end

-- Return line content by 1-indexed line number.
-- In large-file mode, line must be within the current view window.
local function get_line(idx)
    if large_file then
        return view_cache[idx - scroll_y] or ""
    end
    return lines[idx] or ""
end

-- Rebuild view_cache for current scroll_y.  Opens file, seeks to the nearest
-- index entry, skips any lines before scroll_y+1, then collects TEXT_ROWS lines.
local function update_view_cache()
    if not large_file then return end
    if view_cache_scroll == scroll_y then return end

    local top_line = scroll_y + 1

    -- Find the largest index bucket k such that it covers a line <= top_line.
    -- line_index[k] is the offset of line (k-1)*INDEX_STRIDE + 1.
    local base_k = math.floor((top_line - 1) / INDEX_STRIDE) + 1
    if base_k > #line_index then base_k = #line_index end

    local base_line   = (base_k - 1) * INDEX_STRIDE + 1
    local base_offset = line_index[base_k] or 0
    local skip        = top_line - base_line  -- lines to skip before collecting

    local handle = fs.open(filename, "r")
    if not handle then return end
    fs.seek(handle, base_offset)

    local new_cache   = {}
    local parts       = {}
    local lines_skipped = 0
    local collecting  = (skip == 0)

    while #new_cache < TEXT_ROWS do
        local chunk = fs.read(handle, 512)
        if not chunk then
            -- EOF: commit any partial last line
            if collecting then
                new_cache[#new_cache + 1] = sanitize_text(table.concat(parts))
            end
            break
        end

        local pos = 1
        while pos <= #chunk do
            local nl = chunk:find('\n', pos, true)
            if collecting then
                if nl then
                    parts[#parts + 1] = chunk:sub(pos, nl - 1)
                    new_cache[#new_cache + 1] = sanitize_text(table.concat(parts))
                    parts = {}
                    if #new_cache >= TEXT_ROWS then break end
                    pos = nl + 1
                else
                    parts[#parts + 1] = chunk:sub(pos)
                    pos = #chunk + 1
                end
            else
                if nl then
                    lines_skipped = lines_skipped + 1
                    if lines_skipped >= skip then collecting = true end
                    pos = nl + 1
                else
                    pos = #chunk + 1
                end
            end
        end

        if #new_cache >= TEXT_ROWS then break end
    end

    fs.close(handle)
    view_cache       = new_cache
    view_cache_scroll = scroll_y
end

-- ── Inline filename prompt ─────────────────────────────────────────────────────
-- Collects a typed string in the command bar; returns the string or nil on Esc.
-- Deliberately avoids calling other local functions so it can be defined early.

local function prompt_filename(prompt_text)
    local result = ""
    while true do
        local y_base = SCREEN_H - 18 - COMMAND_ROWS * CHAR_H - CHAR_H * 3
        disp.fillRect(0, y_base, SCREEN_W, CHAR_H * 3, CMD_BG)
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

-- Reset all large-file state; call before loading any file.
local function reset_large_file_state()
    large_file        = false
    large_total_lines = 0
    line_index        = {}
    view_cache        = {}
    view_cache_scroll = -1
end

-- Open a large file: scan it to build the sparse line index, then enter pager
-- mode.  The file content is never loaded into memory wholesale.
local function load_large_file(path)
    -- Show progress — scanning may take a few seconds for multi-MB files
    disp.clear(BG)
    disp.drawText(2, SCREEN_H // 2 - 4, "Scanning: " .. path, TEXT_FG, BG)
    disp.flush()

    line_index = {0}  -- line 1 always starts at byte offset 0

    local handle = fs.open(path, "r")
    if not handle then
        message      = "Error: Cannot open " .. path
        message_time = sys.getTimeMs()
        return false
    end

    local line_count  = 0   -- number of \n seen so far
    local byte_offset = 0   -- file offset of first byte of current chunk

    while true do
        local chunk = fs.read(handle, 512)
        if not chunk then break end

        sys.sleep(0)  -- Yield to OS to keep keyboard/networking alive

        local pos = 1
        while pos <= #chunk do
            local nl = chunk:find('\n', pos, true)
            if not nl then break end

            line_count = line_count + 1
            -- The line starting after this newline is at file byte (byte_offset + nl).
            -- Record it in the index every INDEX_STRIDE lines.
            if line_count % INDEX_STRIDE == 0 then
                line_index[#line_index + 1] = byte_offset + nl
            end
            pos = nl + 1
        end

        byte_offset = byte_offset + #chunk
    end

    fs.close(handle)

    large_total_lines = line_count + 1  -- +1: last line may have no trailing \n
    large_file        = true
    lines             = {""}            -- not used in pager mode but must be valid
    cursor_x = 1; cursor_y = 1; scroll_y = 0; scroll_x = 0
    filename = path
    modified = false
    view_cache        = {}
    view_cache_scroll = -1

    message      = string.format("Read-only  %d lines", large_total_lines)
    message_time = sys.getTimeMs()
    return true
end

local function load_file(path)
    reset_large_file_state()

    if not fs.exists(path) then
        lines = {""}
        cursor_x = 1; cursor_y = 1; scroll_y = 0; scroll_x = 0
        filename = path
        modified = false
        message = "[New File]"
        message_time = sys.getTimeMs()
        return true
    end

    -- Route large files to the pager
    local size = fs.size(path)
    if size and size > LARGE_FILE_THRESHOLD then
        return load_large_file(path)
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

        sys.sleep(0)  -- Yield to OS to keep keyboard/networking alive

        local start = 1
        for i = 1, #chunk do
            if chunk:sub(i, i) == '\n' then
                table.insert(lines, sanitize_text(chunk:sub(start, i - 1)))
                start = i + 1
            end
        end
        if start <= #chunk then
            local rest = sanitize_text(chunk:sub(start))
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
    if large_file then
        message = "Read-only: cannot save large file"
        message_time = sys.getTimeMs()
        return false
    end

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
    local tl = total_lines()
    if cursor_y < 1  then cursor_y = 1  end
    if cursor_y > tl then cursor_y = tl end

    if large_file then
        -- Read-only pager: bounded scrolling
        local line = get_line(cursor_y)
        local line_len = line and #line or 0
        if cursor_x < 1 then cursor_x = 1 end
        if cursor_x > line_len + 1 then cursor_x = line_len + 1 end
        
        if cursor_y - 1 < scroll_y then
            scroll_y = cursor_y - 1
        end
        if cursor_y - 1 >= scroll_y + TEXT_ROWS then
            scroll_y = cursor_y - TEXT_ROWS
        end
        if scroll_y < 0 then scroll_y = 0 end
        
        -- Adjust horizontal scroll to keep cursor visible
        if not word_wrap then
            local visible_cols = (tl > TEXT_ROWS) and COLS_WITH_SCROLLBAR or COLS_FULL
            if show_line_numbers then
                visible_cols = visible_cols - LINE_NUM_WIDTH
            end
            if cursor_x - 1 < scroll_x then
                scroll_x = math.max(0, cursor_x - 1)
            elseif cursor_x - 1 >= scroll_x + visible_cols then
                scroll_x = cursor_x - visible_cols
            end
        else
            scroll_x = 0
        end
        return
    end

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
        local visible_cols = (tl > TEXT_ROWS) and COLS_WITH_SCROLLBAR or COLS_FULL
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
        if not large_file then
            local line_len = #lines[cursor_y]
            if cursor_x > line_len + 1 then cursor_x = line_len + 1 end
        end
    elseif dx ~= 0 then
        cursor_x = cursor_x + dx
        
        local current_len
        if large_file then
            local ln = get_line(cursor_y)
            current_len = ln and #ln or 0
        else
            current_len = #lines[cursor_y]
        end
        
        local total = total_lines()
        
        if cursor_x < 1 and cursor_y > 1 then
            cursor_y = cursor_y - 1
            if large_file then
                update_view_cache() -- Need to trigger a cache update before getting the line
                local ln = get_line(cursor_y)
                cursor_x = (ln and #ln or 0) + 1
            else
                cursor_x = #lines[cursor_y] + 1
            end
            scroll_x = 0
        elseif cursor_x > current_len + 1 and cursor_y < total then
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
    local fn = filename or "[Untitled]"
    local suffix
    if large_file then
        suffix = " [R/O]"
    elseif modified then
        suffix = " [+]"
    else
        suffix = ""
    end
    pc.ui.drawHeader(fn .. suffix)
end

local function draw_text_area()
    local y_offset = 28
    local tl = total_lines()
    local visible_cols = (tl > TEXT_ROWS) and COLS_WITH_SCROLLBAR or COLS_FULL
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

        while screen_row < TEXT_ROWS and line_idx <= tl do
            local line = get_line(line_idx)
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
                if line_idx == cursor_y and
                        cursor_x >= start_col and cursor_x <= end_col + 1 then
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
            if line_idx <= tl then
                local line = get_line(line_idx)
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
    local tl = total_lines()
    if tl <= TEXT_ROWS then return end

    local y_offset   = 28
    local scrollbar_h = TEXT_ROWS * CHAR_H
    disp.fillRect(SCROLLBAR_X, y_offset, SCROLLBAR_WIDTH, scrollbar_h, SCROLLBAR_C)

    local visible_ratio = TEXT_ROWS / tl
    local thumb_h       = math.max(8, math.floor(scrollbar_h * visible_ratio))
    local scroll_ratio  = scroll_y / (tl - TEXT_ROWS)
    local thumb_y       = y_offset + math.floor((scrollbar_h - thumb_h) * scroll_ratio)
    disp.fillRect(SCROLLBAR_X, thumb_y, SCROLLBAR_WIDTH, thumb_h, SCROLLBAR_THUMB)
end

local function draw_command_bar()
    local pos = string.format("L%d/%d", cursor_y, total_lines())

    if message ~= "" and sys.getTimeMs() - message_time < 3000 then
        pc.ui.drawFooter(message, pos)
    elseif large_file then
        pc.ui.drawFooter("Read-only  F1:Help", pos)
    else
        pc.ui.drawFooter("F1: Help", pos)
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
    if large_file then
        help_line("Large file (read-only pager)", help_title)
        line_y = line_y + 2
        help_line("Navigation:")
        help_line("  Arrows  Scroll / move")
        help_line("  Ctrl+Up/Dn Page Up/Dn")
        help_line("  Tab     Page up")
        help_line("  Ctrl+O  Open another file")
        help_line("  Ctrl+Q  Quit")
        line_y = line_y + 2
        help_line("View Options:")
        help_line("  F2      Toggle word wrap " .. (word_wrap and "[ON]" or "[OFF]"))
        help_line("  F3      Toggle line numbers " .. (show_line_numbers and "[ON]" or "[OFF]"))
    else
        help_line("File Operations:")
        help_line("  Ctrl+S  Save file")
        help_line("  Ctrl+Q  Quit")
        help_line("  Ctrl+O  Open file")
        help_line("  Ctrl+N  New file")
        line_y = line_y + 2
        help_line("Editing:")
        help_line("  Ctrl+K  Delete current line")
        help_line("  Arrows  Move cursor")
        help_line("  Ctrl+Up/Dn Page Up/Dn")
        help_line("  Enter   New line")
        help_line("  Bksp    Delete char")
        line_y = line_y + 2
        help_line("View Options:")
        help_line("  F2      Toggle word wrap " .. (word_wrap and "[ON]" or "[OFF]"))
        help_line("  F3      Toggle line numbers " .. (show_line_numbers and "[ON]" or "[OFF]"))
    end
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
        -- Refresh pager view window when scroll position changes
        update_view_cache()

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
                save_file()  -- no-op with message for large files

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

            -- Ctrl+N = New empty buffer (disabled in large-file mode)
            elseif lower == 'n' then
                if large_file then
                    reset_large_file_state()
                    lines = {""}
                    cursor_x = 1; cursor_y = 1; scroll_y = 0; scroll_x = 0
                    filename = nil; modified = false
                    message = "[New File]  Ctrl+S to save"
                    message_time = sys.getTimeMs()
                elseif modified then
                    message = "Save first (Ctrl+S)"
                    message_time = sys.getTimeMs()
                else
                    lines = {""}
                    cursor_x = 1; cursor_y = 1; scroll_y = 0; scroll_x = 0
                    filename = nil; modified = false
                    message = "[New File]  Ctrl+S to save"
                    message_time = sys.getTimeMs()
                end

            -- Ctrl+K = Delete current line (small-file mode only)
            elseif lower == 'k' and not large_file then
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

        if (pressed & input.BTN_UP    ~= 0) or (do_repeat and held & input.BTN_UP    ~= 0) then
            if held & input.BTN_CTRL ~= 0 then
                move_cursor(0, -TEXT_ROWS)
            else
                move_cursor(0, -1)
            end
        end
        if (pressed & input.BTN_DOWN  ~= 0) or (do_repeat and held & input.BTN_DOWN  ~= 0) then
            if held & input.BTN_CTRL ~= 0 then
                move_cursor(0, TEXT_ROWS)
            else
                move_cursor(0, 1)
            end
        end
        if (pressed & input.BTN_LEFT  ~= 0) or (do_repeat and held & input.BTN_LEFT  ~= 0) then move_cursor(-1, 0) end
        if (pressed & input.BTN_RIGHT ~= 0) or (do_repeat and held & input.BTN_RIGHT ~= 0) then move_cursor( 1, 0) end

        -- Fn = Home (go to start of line)
        if pressed & input.BTN_FN ~= 0 then
            cursor_x = 1
            scroll_x = 0
        end

        -- Tab = Page Up
        if pressed & input.BTN_TAB ~= 0 then move_cursor(0, -TEXT_ROWS) end

        -- ── Editing (small-file mode only) ────────────────────────────────────

        if not large_file then
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
