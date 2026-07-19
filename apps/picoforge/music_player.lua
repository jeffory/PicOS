-- music_player.lua — 4-channel music playback for PicoForge
-- Mixes 4 Synth voices playing SFX simultaneously

local MusicPlayer = {}
MusicPlayer.__index = MusicPlayer

function MusicPlayer.new(Synth, sfx_data, music_data)
    local voices = {}
    for i = 1, 4 do
        voices[i] = Synth.new()
    end

    return setmetatable({
        Synth = Synth,
        sfx_data = sfx_data,     -- SfxData instance (holds all SFX slots)
        music_data = music_data, -- MusicData instance (holds song)
        voices = voices,
        playing = false,
        current_entry = 0,       -- 0-indexed song entry
        sample_rate = Synth.SAMPLE_RATE,
    }, MusicPlayer)
end

-- Start playing from a song entry
function MusicPlayer:play(entry_idx)
    entry_idx = entry_idx or self.music_data.selected
    self.current_entry = entry_idx
    self.playing = true
    self:_start_entry(entry_idx)
end

-- Stop all playback
function MusicPlayer:stop()
    self.playing = false
    for i = 1, 4 do
        self.voices[i]:stop()
    end
end

-- Start playing all voices for a song entry
function MusicPlayer:_start_entry(entry_idx)
    local entry = self.music_data:get(entry_idx)
    if not entry then
        self.playing = false
        return
    end

    local any_active = false
    for ch = 1, 4 do
        local sfx_idx = entry.sfx[ch]
        if sfx_idx >= 0 and sfx_idx < self.sfx_data.MAX_SLOTS then
            local sfx = self.sfx_data.slots[sfx_idx + 1]
            if sfx then
                self.voices[ch]:play(sfx)
                any_active = true
            else
                self.voices[ch]:stop()
            end
        else
            self.voices[ch]:stop()
        end
    end

    -- If no channels have content, skip to next entry
    if not any_active then
        self:_advance_entry()
    end
end

-- Advance to next song entry
function MusicPlayer:_advance_entry()
    if not self.playing then return end

    local entry = self.music_data:get(self.current_entry)

    -- Check stop flag
    if entry and (entry.flags & self.music_data.FLAG_STOP) ~= 0 then
        self.playing = false
        return
    end

    -- Check loop end flag
    if entry and (entry.flags & self.music_data.FLAG_LOOP_END) ~= 0 then
        local loop_start = self.music_data:find_loop_start(self.current_entry)
        self.current_entry = loop_start
        self:_start_entry(self.current_entry)
        return
    end

    -- Advance
    self.current_entry = self.current_entry + 1

    -- Check if past song end
    local song_end = self.music_data:find_song_end()
    if self.current_entry > song_end then
        self.playing = false
        return
    end

    self:_start_entry(self.current_entry)
end

-- Render mixed samples from all 4 voices
-- Returns: stereo interleaved int16 table, count
function MusicPlayer:render(max_frames)
    if not self.playing then return nil, 0 end

    local mixed = {}
    local count = 0
    local floor = math.floor

    -- Check if all voices are done
    local any_playing = false
    for i = 1, 4 do
        if self.voices[i].playing then
            any_playing = true
            break
        end
    end

    if not any_playing then
        self:_advance_entry()
        if not self.playing then return nil, 0 end
        -- Re-check after advance
        any_playing = false
        for i = 1, 4 do
            if self.voices[i].playing then
                any_playing = true
                break
            end
        end
        if not any_playing then return nil, 0 end
    end

    -- Render each voice separately, then mix
    local bufs = {}
    local counts = {}
    local max_count = 0

    for i = 1, 4 do
        if self.voices[i].playing then
            local samples, c = self.voices[i]:render(max_frames)
            bufs[i] = samples
            counts[i] = c
            if c > max_count then max_count = c end
        else
            bufs[i] = nil
            counts[i] = 0
        end
    end

    if max_count == 0 then return nil, 0 end

    -- Mix all voices
    for j = 1, max_count do
        local sum = 0
        local active = 0
        for i = 1, 4 do
            if bufs[i] and j <= counts[i] then
                sum = sum + bufs[i][j]
                active = active + 1
            end
        end

        -- Average and clamp to prevent clipping
        if active > 1 then
            sum = floor(sum / active * 1.5)  -- slight boost after averaging
        end
        if sum > 32767 then sum = 32767 end
        if sum < -32768 then sum = -32768 end

        count = count + 1
        mixed[count] = sum
    end

    return mixed, count
end

return MusicPlayer
