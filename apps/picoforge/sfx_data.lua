-- sfx_data.lua — SFX data model for PicoForge
-- 64 SFX slots, each with 32 notes

local SfxData = {}
SfxData.__index = SfxData

SfxData.MAX_SLOTS = 64
SfxData.NOTES_PER_SFX = 32

-- Create a single empty note
local function new_note()
    return {
        pitch = -1,       -- -1 = rest/empty, 0-95 = C-0 to B-7
        volume = 5,       -- 0-7
        waveform = 0,     -- 0-7 (see Synth.WAVE_NAMES)
        effect = 0,       -- 0-7 (see Synth.FX_NAMES)
    }
end

-- Create a single empty SFX
function SfxData.new_sfx()
    local notes = {}
    for i = 1, SfxData.NOTES_PER_SFX do
        notes[i] = new_note()
    end
    return {
        notes = notes,
        note_count = SfxData.NOTES_PER_SFX,
        speed = 8,          -- in 120ths of a second per note (~15 notes/sec at speed 8)
        loop_start = 0,     -- 0-indexed
        loop_end = 0,       -- 0 = no loop
    }
end

-- Create the full SFX bank (64 slots)
function SfxData.new()
    local self = setmetatable({
        slots = {},
        selected = 0,       -- 0-indexed
        modified = false,
    }, SfxData)

    for i = 1, SfxData.MAX_SLOTS do
        self.slots[i] = SfxData.new_sfx()
    end

    return self
end

-- Get current SFX (1-indexed into slots table)
function SfxData:current()
    return self.slots[self.selected + 1]
end

-- Select a slot
function SfxData:select(index)
    if index >= 0 and index < SfxData.MAX_SLOTS then
        self.selected = index
    end
end

-- Set a note in the current SFX
function SfxData:set_note(row, pitch, volume, waveform, effect)
    local sfx = self:current()
    if row < 1 or row > sfx.note_count then return end
    local note = sfx.notes[row]
    if pitch ~= nil then note.pitch = pitch end
    if volume ~= nil then note.volume = volume end
    if waveform ~= nil then note.waveform = waveform end
    if effect ~= nil then note.effect = effect end
    self.modified = true
end

-- Check if an SFX slot has any content
function SfxData:slot_has_content(index)
    local sfx = self.slots[index + 1]
    if not sfx then return false end
    for i = 1, sfx.note_count do
        if sfx.notes[i].pitch >= 0 then return true end
    end
    return false
end

------------------------------------------------------------
-- Serialization: text-based format
-- Line 1: slot_count
-- Per slot: header line (speed, loop_start, loop_end)
--           then note_count lines: pitch,vol,wave,fx
------------------------------------------------------------

function SfxData:to_data()
    local parts = {}
    parts[#parts + 1] = tostring(SfxData.MAX_SLOTS)

    for i = 1, SfxData.MAX_SLOTS do
        local sfx = self.slots[i]
        parts[#parts + 1] = string.format("%d,%d,%d,%d",
            sfx.speed, sfx.loop_start, sfx.loop_end, sfx.note_count)

        for j = 1, sfx.note_count do
            local n = sfx.notes[j]
            parts[#parts + 1] = string.format("%d,%d,%d,%d",
                n.pitch, n.volume, n.waveform, n.effect)
        end
    end

    return table.concat(parts, "\n")
end

function SfxData:from_data(data)
    if not data or #data == 0 then return false end
    local lines = {}
    for line in data:gmatch("[^\n]+") do
        lines[#lines + 1] = line
    end

    local idx = 1
    local slot_count = tonumber(lines[idx])
    if not slot_count then return false end
    idx = idx + 1

    for i = 1, math.min(slot_count, SfxData.MAX_SLOTS) do
        local header = lines[idx]
        if not header then break end
        idx = idx + 1

        local spd, ls, le, nc = header:match("^(%d+),(%d+),(%d+),(%d+)$")
        if not spd then break end

        local sfx = self.slots[i]
        sfx.speed = tonumber(spd)
        sfx.loop_start = tonumber(ls)
        sfx.loop_end = tonumber(le)
        sfx.note_count = tonumber(nc)

        for j = 1, sfx.note_count do
            local note_line = lines[idx]
            if not note_line then break end
            idx = idx + 1

            local p, v, w, f = note_line:match("^(%-?%d+),(%d+),(%d+),(%d+)$")
            if p then
                sfx.notes[j].pitch = tonumber(p)
                sfx.notes[j].volume = tonumber(v)
                sfx.notes[j].waveform = tonumber(w)
                sfx.notes[j].effect = tonumber(f)
            end
        end
    end

    self.modified = false
    return true
end

-- Save to file
function SfxData:save(fs, path)
    local f = fs.open(path, "w")
    if not f then return false end
    fs.write(f, self:to_data())
    fs.close(f)
    self.modified = false
    return true
end

-- Load from file
function SfxData:load(fs, path)
    if not fs.exists(path) then return false end
    local f = fs.open(path, "r")
    if not f then return false end
    local chunks = {}
    while true do
        local data = fs.read(f, 4096)
        if not data or #data == 0 then break end
        chunks[#chunks + 1] = data
    end
    fs.close(f)
    return self:from_data(table.concat(chunks))
end

return SfxData
