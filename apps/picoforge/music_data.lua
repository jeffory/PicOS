-- music_data.lua — Music song data for PicoForge
-- A song is a sequence of entries; each entry assigns an SFX to 4 channels

local MusicData = {}
MusicData.__index = MusicData

MusicData.MAX_ENTRIES = 64
MusicData.CHANNELS = 4

-- Flags for song entries
MusicData.FLAG_NONE       = 0
MusicData.FLAG_LOOP_START = 1
MusicData.FLAG_LOOP_END   = 2
MusicData.FLAG_STOP       = 4

-- Create a single empty song entry
local function new_entry()
    return {
        sfx = {-1, -1, -1, -1},  -- SFX index per channel (-1 = empty)
        flags = 0,
    }
end

function MusicData.new()
    local self = setmetatable({
        entries = {},
        entry_count = MusicData.MAX_ENTRIES,
        selected = 0,       -- 0-indexed current entry
        modified = false,
    }, MusicData)

    for i = 1, MusicData.MAX_ENTRIES do
        self.entries[i] = new_entry()
    end

    return self
end

-- Get entry (1-indexed)
function MusicData:get(index)
    return self.entries[index + 1]
end

-- Set SFX for a channel in an entry
function MusicData:set_sfx(entry_idx, channel, sfx_idx)
    local entry = self.entries[entry_idx + 1]
    if not entry or channel < 1 or channel > MusicData.CHANNELS then return end
    entry.sfx[channel] = sfx_idx
    self.modified = true
end

-- Toggle a flag on an entry
function MusicData:toggle_flag(entry_idx, flag)
    local entry = self.entries[entry_idx + 1]
    if not entry then return end
    if (entry.flags & flag) ~= 0 then
        entry.flags = entry.flags & ~flag
    else
        entry.flags = entry.flags | flag
    end
    self.modified = true
end

-- Check if an entry has any SFX assigned
function MusicData:entry_has_content(index)
    local entry = self.entries[index + 1]
    if not entry then return false end
    for ch = 1, MusicData.CHANNELS do
        if entry.sfx[ch] >= 0 then return true end
    end
    return false
end

-- Find the last entry with content (for song length)
function MusicData:find_song_end()
    for i = MusicData.MAX_ENTRIES, 1, -1 do
        local entry = self.entries[i]
        for ch = 1, MusicData.CHANNELS do
            if entry.sfx[ch] >= 0 then return i - 1 end
        end
        if entry.flags ~= 0 then return i - 1 end
    end
    return 0
end

-- Find loop start entry (scanning backwards from given position)
function MusicData:find_loop_start(from)
    for i = from, 0, -1 do
        local entry = self.entries[i + 1]
        if (entry.flags & MusicData.FLAG_LOOP_START) ~= 0 then
            return i
        end
    end
    return 0  -- default: loop to beginning
end

------------------------------------------------------------
-- Serialization
------------------------------------------------------------

function MusicData:to_data()
    local parts = {}
    parts[#parts + 1] = tostring(MusicData.MAX_ENTRIES)

    for i = 1, MusicData.MAX_ENTRIES do
        local entry = self.entries[i]
        parts[#parts + 1] = string.format("%d,%d,%d,%d,%d",
            entry.sfx[1], entry.sfx[2], entry.sfx[3], entry.sfx[4],
            entry.flags)
    end

    return table.concat(parts, "\n")
end

function MusicData:from_data(data)
    if not data or #data == 0 then return false end
    local lines = {}
    for line in data:gmatch("[^\n]+") do
        lines[#lines + 1] = line
    end

    local idx = 1
    local count = tonumber(lines[idx])
    if not count then return false end
    idx = idx + 1

    for i = 1, math.min(count, MusicData.MAX_ENTRIES) do
        local line = lines[idx]
        if not line then break end
        idx = idx + 1

        local s1, s2, s3, s4, fl = line:match(
            "^(%-?%d+),(%-?%d+),(%-?%d+),(%-?%d+),(%d+)$")
        if s1 then
            local entry = self.entries[i]
            entry.sfx[1] = tonumber(s1)
            entry.sfx[2] = tonumber(s2)
            entry.sfx[3] = tonumber(s3)
            entry.sfx[4] = tonumber(s4)
            entry.flags = tonumber(fl)
        end
    end

    self.modified = false
    return true
end

function MusicData:save(fs, path)
    local f = fs.open(path, "w")
    if not f then return false end
    fs.write(f, self:to_data())
    fs.close(f)
    self.modified = false
    return true
end

function MusicData:load(fs, path)
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

return MusicData
