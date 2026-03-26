-- sfx_editor.lua — SFX editor mode for PicoForge
-- Tracker-style grid with waveform preview and playback

local SfxEditor = {}
SfxEditor.__index = SfxEditor

-- Layout constants (320x320, header=28, tabs=12, footer=18)
local CONTENT_Y = 40
local SCREEN_W  = 320
local FOOTER_Y  = 302

-- Tracker grid layout
local GRID_X = 8
local GRID_Y = CONTENT_Y + 4
local ROW_H = 11           -- font height
local VISIBLE_ROWS = 22
local COL_ROW_NUM = 0      -- row number column
local COL_NOTE = 20        -- note name (e.g. "C-4")
local COL_VOL = 44         -- volume digit
local COL_WAVE = 56        -- waveform abbreviation
local COL_FX = 80          -- effect abbreviation

-- Slot selector (right side)
local SLOT_X = 200
local SLOT_Y = CONTENT_Y + 4
local SLOT_CELL = 10
local SLOT_COLS = 8
local SLOT_VISIBLE_ROWS = 8

-- Controls area (right side, below slots)
local CTRL_X = 200
local CTRL_Y = SLOT_Y + SLOT_VISIBLE_ROWS * 11 + 8

-- Waveform preview (bottom right)
local PREVIEW_X = 200
local PREVIEW_Y = FOOTER_Y - 50
local PREVIEW_W = 112
local PREVIEW_H = 40

-- Cursor columns in the tracker
local CURSOR_COLS = {"note", "vol", "wave", "fx"}

-- Keyboard note entry mapping (two octave rows like Pico-8)
-- Lower row: Z-M = C-4 to B-4
-- Upper row: Q-P = C-5 to B-5 (approx)
local KEY_NOTE_MAP = {
    z = 0, s = 1, x = 2, d = 3, c = 4,     -- C, C#, D, D#, E
    v = 5, g = 6, b = 7, h = 8, n = 9,      -- F, F#, G, G#, A
    j = 10, m = 11,                           -- A#, B
    q = 12, ["2"] = 13, w = 14, ["3"] = 15, e = 16,  -- C+1, C#+1, ...
    r = 17, ["5"] = 18, t = 19, ["6"] = 20, y = 21,
    ["7"] = 22, u = 23,
}

function SfxEditor.new(Synth, SfxData, audio)
    return setmetatable({
        Synth = Synth,
        SfxData = SfxData,
        synth = Synth.new(),
        data = SfxData.new(),
        audio = audio,
        cursor_row = 0,     -- 0-indexed row in tracker
        cursor_col = 1,     -- 1-indexed into CURSOR_COLS
        scroll = 0,         -- scroll offset for tracker
        octave = 4,         -- current octave for note entry
        project_path = nil,
        streaming = false,  -- whether audio stream is active
    }, SfxEditor)
end

-- Initialize with project path
function SfxEditor:init(project_path, fs)
    self.project_path = project_path
    local sfx_path = project_path .. "/sounds.sfx"
    if fs.exists(sfx_path) then
        self.data:load(fs, sfx_path)
    end
end

-- Save SFX data
function SfxEditor:save(fs)
    if not self.project_path then return false end
    return self.data:save(fs, self.project_path .. "/sounds.sfx")
end

-- Play the current SFX
function SfxEditor:play_sfx()
    local sfx = self.data:current()
    if not sfx then return end
    self.synth:play(sfx)
    if not self.streaming then
        self.audio.startStream(self.Synth.SAMPLE_RATE)
        self.streaming = true
    end
end

-- Stop playback
function SfxEditor:stop_sfx()
    self.synth:stop()
    if self.streaming then
        self.audio.stopStream()
        self.streaming = false
    end
end

-- Update audio: call each frame to push samples
function SfxEditor:update_audio()
    if not self.synth.playing then
        if self.streaming then
            self.audio.stopStream()
            self.streaming = false
        end
        return
    end

    local free = self.audio.ringFree()
    if free > 128 then
        local frames = math.min(free, 256)
        local samples, count = self.synth:render(frames)
        if samples and count > 0 then
            self.audio.pushSamples(samples)
        end
    end
end

------------------------------------------------------------
-- Input handling
------------------------------------------------------------

function SfxEditor:handle_button(pressed, held, char, BTN)
    local is_ctrl = (held & BTN.CTRL) ~= 0
    local is_shift = (held & BTN.SHIFT) ~= 0
    local is_fn = (held & BTN.FN) ~= 0

    local function btn(b) return (pressed & b) ~= 0 end

    -- Ctrl shortcuts
    if is_ctrl and char then
        local lc = char:lower()
        if lc == "s" then
            return true  -- save handled by caller
        end
        return false
    end

    -- FN + arrows: navigate SFX slots
    if is_fn then
        if btn(BTN.LEFT) then self:nav_slot(-1); return true end
        if btn(BTN.RIGHT) then self:nav_slot(1); return true end
        if btn(BTN.UP) then self:nav_slot(-SLOT_COLS); return true end
        if btn(BTN.DOWN) then self:nav_slot(SLOT_COLS); return true end
    end

    -- Arrow keys: move cursor in tracker
    if btn(BTN.UP) then
        self.cursor_row = self.cursor_row - 1
        if self.cursor_row < 0 then
            self.cursor_row = self.data:current().note_count - 1
        end
        self:_ensure_visible()
        return true
    end
    if btn(BTN.DOWN) then
        self.cursor_row = self.cursor_row + 1
        if self.cursor_row >= self.data:current().note_count then
            self.cursor_row = 0
        end
        self:_ensure_visible()
        return true
    end
    if btn(BTN.LEFT) then
        self.cursor_col = self.cursor_col - 1
        if self.cursor_col < 1 then self.cursor_col = #CURSOR_COLS end
        return true
    end
    if btn(BTN.RIGHT) then
        self.cursor_col = self.cursor_col + 1
        if self.cursor_col > #CURSOR_COLS then self.cursor_col = 1 end
        return true
    end

    -- Enter: play/stop
    if btn(BTN.ENTER) then
        if self.synth.playing then
            self:stop_sfx()
        else
            self:play_sfx()
        end
        return true
    end

    -- Backspace/Delete: clear current cell
    if btn(BTN.BACKSPACE) then
        self:clear_cell()
        return true
    end

    -- Tab: cycle column
    if btn(BTN.TAB) then
        if is_shift then
            self.cursor_col = self.cursor_col - 1
            if self.cursor_col < 1 then self.cursor_col = #CURSOR_COLS end
        else
            self.cursor_col = self.cursor_col + 1
            if self.cursor_col > #CURSOR_COLS then self.cursor_col = 1 end
        end
        return true
    end

    -- Character input for editing
    if char and not is_ctrl then
        return self:handle_char_input(char, is_shift)
    end

    return false
end

function SfxEditor:handle_char_input(char, is_shift)
    local col_name = CURSOR_COLS[self.cursor_col]
    local lc = char:lower()

    -- Note column: piano keyboard mapping
    if col_name == "note" then
        local semi = KEY_NOTE_MAP[lc]
        if semi then
            local pitch = self.octave * 12 + semi
            if pitch > 95 then pitch = 95 end
            self.data:set_note(self.cursor_row + 1, pitch, nil, nil, nil)
            -- Auto-advance
            self.cursor_row = self.cursor_row + 1
            if self.cursor_row >= self.data:current().note_count then
                self.cursor_row = 0
            end
            self:_ensure_visible()
            return true
        end

        -- +/- change octave
        if char == "+" or char == "=" then
            self.octave = math.min(self.octave + 1, 7)
            return true
        end
        if char == "-" then
            self.octave = math.max(self.octave - 1, 0)
            return true
        end
    end

    -- Volume column: 0-7
    if col_name == "vol" then
        local n = tonumber(char)
        if n and n >= 0 and n <= 7 then
            self.data:set_note(self.cursor_row + 1, nil, n, nil, nil)
            self.cursor_row = self.cursor_row + 1
            if self.cursor_row >= self.data:current().note_count then
                self.cursor_row = 0
            end
            self:_ensure_visible()
            return true
        end
    end

    -- Waveform column: 0-7
    if col_name == "wave" then
        local n = tonumber(char)
        if n and n >= 0 and n <= 7 then
            self.data:set_note(self.cursor_row + 1, nil, nil, n, nil)
            self.cursor_row = self.cursor_row + 1
            if self.cursor_row >= self.data:current().note_count then
                self.cursor_row = 0
            end
            self:_ensure_visible()
            return true
        end
    end

    -- Effect column: 0-7
    if col_name == "fx" then
        local n = tonumber(char)
        if n and n >= 0 and n <= 7 then
            self.data:set_note(self.cursor_row + 1, nil, nil, nil, n)
            self.cursor_row = self.cursor_row + 1
            if self.cursor_row >= self.data:current().note_count then
                self.cursor_row = 0
            end
            self:_ensure_visible()
            return true
        end
    end

    -- Speed adjust: [ and ] keys (any column)
    if char == "[" then
        local sfx = self.data:current()
        sfx.speed = math.max(1, sfx.speed - 1)
        self.data.modified = true
        return true
    end
    if char == "]" then
        local sfx = self.data:current()
        sfx.speed = math.min(255, sfx.speed + 1)
        self.data.modified = true
        return true
    end

    -- Loop: { sets loop start, } sets loop end
    if char == "{" then
        local sfx = self.data:current()
        sfx.loop_start = self.cursor_row
        self.data.modified = true
        return true
    end
    if char == "}" then
        local sfx = self.data:current()
        sfx.loop_end = self.cursor_row + 1
        self.data.modified = true
        return true
    end

    return false
end

function SfxEditor:clear_cell()
    local col_name = CURSOR_COLS[self.cursor_col]
    if col_name == "note" then
        self.data:set_note(self.cursor_row + 1, -1, nil, nil, nil)
    elseif col_name == "vol" then
        self.data:set_note(self.cursor_row + 1, nil, 0, nil, nil)
    elseif col_name == "wave" then
        self.data:set_note(self.cursor_row + 1, nil, nil, 0, nil)
    elseif col_name == "fx" then
        self.data:set_note(self.cursor_row + 1, nil, nil, nil, 0)
    end
end

function SfxEditor:nav_slot(delta)
    local new_idx = self.data.selected + delta
    if new_idx < 0 then new_idx = 0 end
    if new_idx >= self.SfxData.MAX_SLOTS then
        new_idx = self.SfxData.MAX_SLOTS - 1
    end
    self.data:select(new_idx)
    self.cursor_row = 0
    self.scroll = 0
end

function SfxEditor:_ensure_visible()
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

function SfxEditor:draw(disp, Synth)
    -- Clear content area
    disp.fillRect(0, CONTENT_Y, SCREEN_W, FOOTER_Y - CONTENT_Y, 0x0000)

    self:draw_tracker(disp, Synth)
    self:draw_slots(disp)
    self:draw_controls(disp)
    self:draw_waveform_preview(disp, Synth)
end

function SfxEditor:draw_tracker(disp, Synth)
    local sfx = self.data:current()
    if not sfx then return end

    -- Header
    disp.drawText(GRID_X, GRID_Y - 2,
        " ## NOTE V WV EFX", 0x4A69)

    local y = GRID_Y + ROW_H
    for i = 0, VISIBLE_ROWS - 1 do
        local row = self.scroll + i
        if row >= sfx.note_count then break end

        local note = sfx.notes[row + 1]
        local ry = y + i * ROW_H
        local is_cursor = (row == self.cursor_row)
        local is_playing = self.synth.playing and (row == self.synth.row)

        -- Row background
        if is_cursor then
            disp.fillRect(GRID_X, ry - 1, 180, ROW_H, 0x2104)
        end
        if is_playing then
            disp.fillRect(GRID_X, ry - 1, 180, ROW_H, 0x0208)
        end

        -- Loop markers
        if row == sfx.loop_start and sfx.loop_end > sfx.loop_start then
            disp.fillRect(GRID_X - 2, ry - 1, 2, ROW_H, 0x07E0)
        end
        if row == sfx.loop_end - 1 and sfx.loop_end > sfx.loop_start then
            disp.fillRect(GRID_X - 2, ry - 1, 2, ROW_H, 0xF800)
        end

        -- Row number
        local row_color = (row % 4 == 0) and 0x4A69 or 0x2945
        disp.drawText(GRID_X + COL_ROW_NUM, ry,
            string.format("%02d", row), row_color)

        -- Note name
        local note_str = note.pitch >= 0 and Synth.note_name(note.pitch) or "..."
        local note_color = note.pitch >= 0 and 0x5EFB or 0x2945
        if is_cursor and self.cursor_col == 1 then note_color = 0xFFFF end
        disp.drawText(GRID_X + COL_NOTE, ry, note_str, note_color)

        -- Volume
        local vol_str = tostring(note.volume)
        local vol_color = note.volume > 0 and 0xFD20 or 0x2945
        if is_cursor and self.cursor_col == 2 then vol_color = 0xFFFF end
        disp.drawText(GRID_X + COL_VOL, ry, vol_str, vol_color)

        -- Waveform
        local wave_str = Synth.WAVE_NAMES[note.waveform] or "???"
        local wave_color = 0x07FF
        if is_cursor and self.cursor_col == 3 then wave_color = 0xFFFF end
        disp.drawText(GRID_X + COL_WAVE, ry, wave_str, wave_color)

        -- Effect
        local fx_str = Synth.FX_NAMES[note.effect] or "???"
        local fx_color = note.effect > 0 and 0xFBB5 or 0x2945
        if is_cursor and self.cursor_col == 4 then fx_color = 0xFFFF end
        disp.drawText(GRID_X + COL_FX, ry, fx_str, fx_color)
    end

    -- Cursor column highlight
    local col_x = GRID_X
    local col_w = 18
    if self.cursor_col == 1 then col_x = GRID_X + COL_NOTE; col_w = 20 end
    if self.cursor_col == 2 then col_x = GRID_X + COL_VOL; col_w = 8 end
    if self.cursor_col == 3 then col_x = GRID_X + COL_WAVE; col_w = 20 end
    if self.cursor_col == 4 then col_x = GRID_X + COL_FX; col_w = 20 end

    local cursor_screen_row = self.cursor_row - self.scroll
    if cursor_screen_row >= 0 and cursor_screen_row < VISIBLE_ROWS then
        local cy = y + cursor_screen_row * ROW_H
        disp.drawRect(col_x - 1, cy - 2, col_w + 2, ROW_H + 2, 0xFFFF)
    end
end

function SfxEditor:draw_slots(disp)
    disp.drawText(SLOT_X, SLOT_Y - 2, "SFX Slots", 0x7BEF)

    local y = SLOT_Y + ROW_H
    for r = 0, SLOT_VISIBLE_ROWS - 1 do
        for c = 0, SLOT_COLS - 1 do
            local idx = r * SLOT_COLS + c
            if idx >= self.SfxData.MAX_SLOTS then break end

            local gx = SLOT_X + c * (SLOT_CELL + 1)
            local gy = y + r * (SLOT_CELL + 1)

            local has_content = self.data:slot_has_content(idx)
            local bg = has_content and 0x2945 or 0x0841
            disp.fillRect(gx, gy, SLOT_CELL, SLOT_CELL, bg)

            if idx == self.data.selected then
                disp.drawRect(gx - 1, gy - 1,
                    SLOT_CELL + 2, SLOT_CELL + 2, 0xFFFF)
            end
        end
    end

    -- Slot number label
    disp.drawText(SLOT_X, y + SLOT_VISIBLE_ROWS * (SLOT_CELL + 1) + 2,
        string.format("#%02d", self.data.selected), 0xD6BA)
end

function SfxEditor:draw_controls(disp)
    local sfx = self.data:current()
    if not sfx then return end

    local y = CTRL_Y

    -- Speed
    disp.drawText(CTRL_X, y, "Speed:", 0x7BEF)
    disp.drawText(CTRL_X + 42, y, tostring(sfx.speed), 0xFFFF)
    y = y + 12

    -- Loop
    if sfx.loop_end > sfx.loop_start then
        disp.drawText(CTRL_X, y, "Loop:", 0x7BEF)
        disp.drawText(CTRL_X + 36, y,
            string.format("%d-%d", sfx.loop_start, sfx.loop_end - 1), 0x07E0)
    else
        disp.drawText(CTRL_X, y, "Loop: off", 0x7BEF)
    end
    y = y + 12

    -- Octave
    disp.drawText(CTRL_X, y, "Oct:", 0x7BEF)
    disp.drawText(CTRL_X + 30, y, tostring(self.octave), 0xFFFF)
    y = y + 12

    -- Playback status
    if self.synth.playing then
        disp.drawText(CTRL_X, y, "PLAYING", 0x07E0)
        disp.drawText(CTRL_X + 48, y,
            string.format("R:%02d", self.synth.row), 0x5EFB)
    else
        disp.drawText(CTRL_X, y, "STOPPED", 0x7BEF)
    end
end

function SfxEditor:draw_waveform_preview(disp, Synth)
    local sfx = self.data:current()
    if not sfx then return end
    local note = sfx.notes[self.cursor_row + 1]
    if not note or note.pitch < 0 then return end

    -- Draw box
    disp.drawRect(PREVIEW_X - 1, PREVIEW_Y - 1,
        PREVIEW_W + 2, PREVIEW_H + 2, 0x4208)
    disp.drawText(PREVIEW_X, PREVIEW_Y - 12,
        Synth.WAVE_NAMES[note.waveform] or "???", 0x7BEF)

    -- Draw one cycle of the waveform
    local mid_y = PREVIEW_Y + PREVIEW_H // 2
    local prev_y = nil

    for x = 0, PREVIEW_W - 1 do
        local phase = x / PREVIEW_W
        local val = 0

        -- Simple inline waveform eval for preview
        local w = note.waveform
        if w == 0 then      -- square
            val = phase < 0.5 and 1 or -1
        elseif w == 1 then  -- pulse25
            val = phase < 0.25 and 1 or -1
        elseif w == 2 then  -- saw
            val = 2 * phase - 1
        elseif w == 3 then  -- triangle
            val = math.abs(4 * phase - 2) - 1
        elseif w == 4 then  -- sine
            val = math.sin(math.pi * 2 * phase)
        elseif w == 5 then  -- noise (deterministic for preview)
            val = ((x * 31337 + 12345) % 256) / 128 - 1
        elseif w == 6 then  -- metallic
            local sq = phase < 0.5 and 1 or -1
            val = sq * 0.7 + (((x * 31337) % 256) / 128 - 1) * 0.3
        elseif w == 7 then  -- buzz
            val = 2 * phase - 1
            if val > 0.5 then val = 0.5 end
            if val < -0.5 then val = -0.5 end
            val = val * 2
        end

        local py = mid_y - math.floor(val * (PREVIEW_H / 2 - 2))
        disp.setPixel(PREVIEW_X + x, py, 0x07E0)

        -- Connect dots for smooth line
        if prev_y and math.abs(py - prev_y) > 1 then
            local step = py > prev_y and 1 or -1
            for ly = prev_y + step, py - step, step do
                disp.setPixel(PREVIEW_X + x, ly, 0x07E0)
            end
        end
        prev_y = py
    end

    -- Center line
    for x = 0, PREVIEW_W - 1, 4 do
        disp.setPixel(PREVIEW_X + x, mid_y, 0x2104)
    end
end

function SfxEditor:get_footer_text()
    local col_name = CURSOR_COLS[self.cursor_col] or "?"
    local mod = self.data.modified and "[+]" or ""
    return string.format("SFX#%02d %s Oct:%d %s  Enter:Play  []:Spd",
        self.data.selected, col_name, self.octave, mod)
end

return SfxEditor
