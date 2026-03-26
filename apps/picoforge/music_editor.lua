-- music_editor.lua — Music editor mode for PicoForge
-- Song sequencer: arrange SFX across 4 channels

local MusicEditor = {}
MusicEditor.__index = MusicEditor

-- Layout constants
local CONTENT_Y = 40
local SCREEN_W  = 320
local FOOTER_Y  = 302

-- Song grid layout
local GRID_X = 8
local GRID_Y = CONTENT_Y + 14
local ROW_H = 11
local VISIBLE_ROWS = 21

-- Column positions within the grid
local COL_NUM = 0       -- entry number
local COL_CH1 = 22      -- channel 1 SFX
local COL_CH2 = 52      -- channel 2
local COL_CH3 = 82      -- channel 3
local COL_CH4 = 112     -- channel 4
local COL_FLAGS = 142    -- flags display
local COL_POSITIONS = {COL_CH1, COL_CH2, COL_CH3, COL_CH4}

-- Channel colors
local CH_COLORS = {0x5EFB, 0x07E0, 0xFD20, 0xFBB5}  -- blue, green, orange, pink

-- Info panel (right side)
local INFO_X = 180
local INFO_Y = CONTENT_Y + 4

function MusicEditor.new(MusicPlayer, MusicData, Synth, sfx_data, audio)
    local music_data = MusicData.new()
    local player = MusicPlayer.new(Synth, sfx_data, music_data)

    return setmetatable({
        MusicPlayer = MusicPlayer,
        MusicData = MusicData,
        Synth = Synth,
        player = player,
        data = music_data,
        sfx_data = sfx_data,
        audio = audio,
        cursor_row = 0,     -- 0-indexed song entry
        cursor_col = 1,     -- 1-4 = channels
        scroll = 0,
        streaming = false,
        project_path = nil,
        -- Number entry buffer for SFX slot
        num_buf = "",
        num_timer = 0,
    }, MusicEditor)
end

function MusicEditor:init(project_path, fs)
    self.project_path = project_path
    local mus_path = project_path .. "/music.mus"
    if fs.exists(mus_path) then
        self.data:load(fs, mus_path)
    end
end

function MusicEditor:save(fs)
    if not self.project_path then return false end
    return self.data:save(fs, self.project_path .. "/music.mus")
end

-- Playback controls
function MusicEditor:play()
    self.player:play(self.cursor_row)
    if not self.streaming then
        self.audio.startStream(self.Synth.SAMPLE_RATE)
        self.streaming = true
    end
end

function MusicEditor:stop()
    self.player:stop()
    if self.streaming then
        self.audio.stopStream()
        self.streaming = false
    end
end

function MusicEditor:update_audio()
    if not self.player.playing then
        if self.streaming then
            self.audio.stopStream()
            self.streaming = false
        end
        return
    end

    local free = self.audio.ringFree()
    if free > 128 then
        local frames = math.min(free, 256)
        local samples, count = self.player:render(frames)
        if samples and count > 0 then
            self.audio.pushSamples(samples)
        end
    end
end

------------------------------------------------------------
-- Input handling
------------------------------------------------------------

function MusicEditor:handle_button(pressed, held, char, BTN)
    local is_ctrl = (held & BTN.CTRL) ~= 0
    local is_fn = (held & BTN.FN) ~= 0

    local function btn(b) return (pressed & b) ~= 0 end

    -- Ctrl shortcuts handled by caller
    if is_ctrl and char then
        return false
    end

    -- Arrow keys
    if btn(BTN.UP) then
        self.cursor_row = self.cursor_row - 1
        if self.cursor_row < 0 then
            self.cursor_row = self.MusicData.MAX_ENTRIES - 1
        end
        self:_ensure_visible()
        return true
    end
    if btn(BTN.DOWN) then
        self.cursor_row = self.cursor_row + 1
        if self.cursor_row >= self.MusicData.MAX_ENTRIES then
            self.cursor_row = 0
        end
        self:_ensure_visible()
        return true
    end
    if btn(BTN.LEFT) then
        self.cursor_col = self.cursor_col - 1
        if self.cursor_col < 1 then self.cursor_col = 4 end
        return true
    end
    if btn(BTN.RIGHT) then
        self.cursor_col = self.cursor_col + 1
        if self.cursor_col > 4 then self.cursor_col = 1 end
        return true
    end

    -- Enter: play/stop
    if btn(BTN.ENTER) then
        if self.player.playing then
            self:stop()
        else
            self:play()
        end
        return true
    end

    -- Backspace: clear current channel
    if btn(BTN.BACKSPACE) then
        self.data:set_sfx(self.cursor_row, self.cursor_col, -1)
        self.num_buf = ""
        return true
    end

    -- Tab: cycle column
    if btn(BTN.TAB) then
        self.cursor_col = (self.cursor_col % 4) + 1
        return true
    end

    -- Character input
    if char and not is_ctrl then
        return self:handle_char_input(char)
    end

    return false
end

function MusicEditor:handle_char_input(char)
    -- Number keys: enter SFX slot number (0-63, two digit entry)
    local n = tonumber(char)
    if n then
        self.num_buf = self.num_buf .. char
        self.num_timer = 30  -- frames until auto-commit

        local val = tonumber(self.num_buf)
        if #self.num_buf >= 2 or val > 6 then
            -- Commit the number
            if val and val >= 0 and val < self.sfx_data.MAX_SLOTS then
                self.data:set_sfx(self.cursor_row, self.cursor_col, val)
            end
            self.num_buf = ""
            self.num_timer = 0
            -- Auto-advance to next row
            self.cursor_row = self.cursor_row + 1
            if self.cursor_row >= self.MusicData.MAX_ENTRIES then
                self.cursor_row = 0
            end
            self:_ensure_visible()
        end
        return true
    end

    local lc = char:lower()

    -- L = toggle loop start
    if lc == "l" then
        self.data:toggle_flag(self.cursor_row, self.MusicData.FLAG_LOOP_START)
        return true
    end

    -- E = toggle loop end
    if lc == "e" then
        self.data:toggle_flag(self.cursor_row, self.MusicData.FLAG_LOOP_END)
        return true
    end

    -- S = toggle stop
    if lc == "s" then
        self.data:toggle_flag(self.cursor_row, self.MusicData.FLAG_STOP)
        return true
    end

    return false
end

-- Called each frame to handle number entry timeout
function MusicEditor:update()
    if self.num_timer > 0 then
        self.num_timer = self.num_timer - 1
        if self.num_timer == 0 and #self.num_buf > 0 then
            local val = tonumber(self.num_buf)
            if val and val >= 0 and val < self.sfx_data.MAX_SLOTS then
                self.data:set_sfx(self.cursor_row, self.cursor_col, val)
            end
            self.num_buf = ""
            self.cursor_row = self.cursor_row + 1
            if self.cursor_row >= self.MusicData.MAX_ENTRIES then
                self.cursor_row = 0
            end
            self:_ensure_visible()
        end
    end
end

function MusicEditor:_ensure_visible()
    if self.cursor_row < self.scroll then
        self.scroll = self.cursor_row
    end
    if self.cursor_row >= self.scroll + VISIBLE_ROWS then
        self.scroll = self.cursor_row - VISIBLE_ROWS + 1
    end
end

------------------------------------------------------------
-- Rendering
------------------------------------------------------------

function MusicEditor:draw(disp)
    disp.fillRect(0, CONTENT_Y, SCREEN_W, FOOTER_Y - CONTENT_Y, 0x0000)

    self:draw_grid(disp)
    self:draw_info(disp)
end

function MusicEditor:draw_grid(disp)
    -- Column headers
    local hy = GRID_Y - 12
    disp.drawText(GRID_X + COL_NUM, hy, "##", 0x4A69)
    disp.drawText(GRID_X + COL_CH1, hy, "CH1", CH_COLORS[1])
    disp.drawText(GRID_X + COL_CH2, hy, "CH2", CH_COLORS[2])
    disp.drawText(GRID_X + COL_CH3, hy, "CH3", CH_COLORS[3])
    disp.drawText(GRID_X + COL_CH4, hy, "CH4", CH_COLORS[4])
    disp.drawText(GRID_X + COL_FLAGS, hy, "FLG", 0x7BEF)

    for i = 0, VISIBLE_ROWS - 1 do
        local row = self.scroll + i
        if row >= self.MusicData.MAX_ENTRIES then break end

        local entry = self.data:get(row)
        local ry = GRID_Y + i * ROW_H
        local is_cursor = (row == self.cursor_row)
        local is_playing = self.player.playing and (row == self.player.current_entry)

        -- Background
        if is_cursor then
            disp.fillRect(GRID_X, ry - 1, 170, ROW_H, 0x2104)
        end
        if is_playing then
            disp.fillRect(GRID_X, ry - 1, 170, ROW_H, 0x0208)
        end

        -- Entry number
        local num_color = (row % 4 == 0) and 0x4A69 or 0x2945
        disp.drawText(GRID_X + COL_NUM, ry,
            string.format("%02d", row), num_color)

        -- Channel SFX assignments
        for ch = 1, 4 do
            local sfx_idx = entry.sfx[ch]
            local col_x = COL_POSITIONS[ch]
            local color = CH_COLORS[ch]

            if sfx_idx >= 0 then
                disp.drawText(GRID_X + col_x, ry,
                    string.format("%02d", sfx_idx), color)
            else
                disp.drawText(GRID_X + col_x, ry, "--", 0x2945)
            end

            -- Cursor highlight on active column
            if is_cursor and ch == self.cursor_col then
                disp.drawRect(GRID_X + col_x - 2, ry - 2, 20, ROW_H + 2, 0xFFFF)
            end
        end

        -- Flags
        local flags_str = ""
        if (entry.flags & self.MusicData.FLAG_LOOP_START) ~= 0 then
            flags_str = flags_str .. "L"
        end
        if (entry.flags & self.MusicData.FLAG_LOOP_END) ~= 0 then
            flags_str = flags_str .. "E"
        end
        if (entry.flags & self.MusicData.FLAG_STOP) ~= 0 then
            flags_str = flags_str .. "S"
        end
        if #flags_str > 0 then
            disp.drawText(GRID_X + COL_FLAGS, ry, flags_str, 0xFFE0)
        end
    end

    -- Number entry indicator
    if #self.num_buf > 0 then
        local cursor_screen_row = self.cursor_row - self.scroll
        if cursor_screen_row >= 0 and cursor_screen_row < VISIBLE_ROWS then
            local col_x = COL_POSITIONS[self.cursor_col]
            local ry = GRID_Y + cursor_screen_row * ROW_H
            disp.fillRect(GRID_X + col_x - 2, ry - 2, 20, ROW_H + 2, 0x4A69)
            disp.drawText(GRID_X + col_x, ry, self.num_buf .. "_", 0xFFFF)
        end
    end
end

function MusicEditor:draw_info(disp)
    local y = INFO_Y

    -- Title
    disp.drawText(INFO_X, y, "Music", 0xFFFF)
    y = y + 16

    -- Playback status
    if self.player.playing then
        disp.drawText(INFO_X, y, "PLAYING", 0x07E0)
        y = y + 12
        disp.drawText(INFO_X, y,
            string.format("Entry: %02d", self.player.current_entry), 0x5EFB)
    else
        disp.drawText(INFO_X, y, "STOPPED", 0x7BEF)
    end
    y = y + 16

    -- Song length
    local song_end = self.data:find_song_end()
    disp.drawText(INFO_X, y, "Length:", 0x7BEF)
    disp.drawText(INFO_X + 48, y, tostring(song_end + 1), 0xFFFF)
    y = y + 16

    -- Current entry detail
    disp.drawText(INFO_X, y, "Entry " .. string.format("%02d", self.cursor_row), 0xD6BA)
    y = y + 14
    local entry = self.data:get(self.cursor_row)
    if entry then
        for ch = 1, 4 do
            local sfx_idx = entry.sfx[ch]
            local label = string.format("Ch%d:", ch)
            disp.drawText(INFO_X, y, label, CH_COLORS[ch])
            if sfx_idx >= 0 then
                disp.drawText(INFO_X + 30, y,
                    string.format("SFX %02d", sfx_idx), 0xFFFF)
            else
                disp.drawText(INFO_X + 30, y, "---", 0x2945)
            end
            y = y + 12
        end
    end
    y = y + 4

    -- Controls help
    disp.drawText(INFO_X, y, "Controls:", 0x4A69)
    y = y + 12
    disp.drawText(INFO_X, y, "0-9: Set SFX", 0x7BEF)
    y = y + 11
    disp.drawText(INFO_X, y, "BS:  Clear", 0x7BEF)
    y = y + 11
    disp.drawText(INFO_X, y, "L:   Loop start", 0x7BEF)
    y = y + 11
    disp.drawText(INFO_X, y, "E:   Loop end", 0x7BEF)
    y = y + 11
    disp.drawText(INFO_X, y, "S:   Stop flag", 0x7BEF)
end

function MusicEditor:get_footer_text()
    local mod = self.data.modified and "[+]" or ""
    return string.format("Music %s  Enter:Play/Stop  ^S:Save", mod)
end

return MusicEditor
