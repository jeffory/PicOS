-- synth.lua — Software synthesizer for PicoForge SFX editor
-- Generates PCM samples (int16 stereo) for audio.pushSamples()

local Synth = {}
Synth.__index = Synth

-- Sample rate for SFX playback
Synth.SAMPLE_RATE = 11025

-- Waveform IDs
Synth.SQUARE   = 0
Synth.PULSE25  = 1
Synth.SAW      = 2
Synth.TRIANGLE = 3
Synth.SINE     = 4
Synth.NOISE    = 5
Synth.METALLIC = 6
Synth.BUZZ     = 7

Synth.WAVE_NAMES = {
    [0] = "SQR", "PUL", "SAW", "TRI", "SIN", "NOI", "MET", "BUZ",
}

Synth.WAVE_COUNT = 8

-- Effect IDs
Synth.FX_NONE     = 0
Synth.FX_SLIDE_UP = 1
Synth.FX_SLIDE_DN = 2
Synth.FX_DROP     = 3
Synth.FX_VIBRATO  = 4
Synth.FX_ARP_MIN  = 5
Synth.FX_ARP_MAJ  = 6
Synth.FX_FADEOUT  = 7

Synth.FX_NAMES = {
    [0] = "---", "SU+", "SD-", "DRP", "VIB", "Am ", "AM ", "FO ",
}

Synth.FX_COUNT = 8

-- Note frequency table: MIDI-style, C-0 = note 0, B-7 = note 95
-- freq = base * 2^(note/12)
local NOTE_FREQ = {}
do
    local base = 32.7032  -- C1 in standard tuning, we use C-0 = ~16.35
    base = 16.3516        -- C-0
    for i = 0, 95 do
        NOTE_FREQ[i] = base * (2 ^ (i / 12))
    end
end
Synth.NOTE_FREQ = NOTE_FREQ

-- Note names
local NOTE_NAMES = {
    "C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-",
}

function Synth.note_name(pitch)
    if pitch < 0 or pitch > 95 then return "..." end
    local octave = math.floor(pitch / 12)
    local note = (pitch % 12) + 1
    return NOTE_NAMES[note] .. octave
end

-- Parse a note name like "C-4" or "C#3" back to pitch number
function Synth.parse_note(name)
    if not name or #name < 3 then return nil end
    local note_str = name:sub(1, 2)
    local oct = tonumber(name:sub(3, 3))
    if not oct then return nil end
    for i, n in ipairs(NOTE_NAMES) do
        if n == note_str then
            return (oct * 12) + (i - 1)
        end
    end
    return nil
end

------------------------------------------------------------
-- Waveform generators: phase in [0, 1), returns [-1, 1]
------------------------------------------------------------

local sin = math.sin
local pi2 = math.pi * 2
local floor = math.floor
local abs = math.abs

-- Simple LFSR noise generator
local noise_state = 0x1234
local function noise_next()
    noise_state = noise_state ~ (noise_state << 13)
    noise_state = noise_state ~ (noise_state >> 17)
    noise_state = noise_state ~ (noise_state << 5)
    noise_state = noise_state & 0xFFFF
    return (noise_state / 32768) - 1  -- [-1, 1]
end

local function wave_square(phase)
    return phase < 0.5 and 1 or -1
end

local function wave_pulse25(phase)
    return phase < 0.25 and 1 or -1
end

local function wave_saw(phase)
    return 2 * phase - 1
end

local function wave_triangle(phase)
    return abs(4 * phase - 2) - 1
end

local function wave_sine(phase)
    return sin(pi2 * phase)
end

local function wave_noise(_phase)
    return noise_next()
end

local function wave_metallic(phase)
    -- Square + noise mix
    local sq = phase < 0.5 and 1 or -1
    return sq * 0.7 + noise_next() * 0.3
end

local function wave_buzz(phase)
    -- Harsh buzzy wave: clipped sawtooth
    local s = 2 * phase - 1
    if s > 0.5 then s = 0.5 end
    if s < -0.5 then s = -0.5 end
    return s * 2
end

local WAVE_FN = {
    [0] = wave_square, wave_pulse25, wave_saw, wave_triangle,
    wave_sine, wave_noise, wave_metallic, wave_buzz,
}

------------------------------------------------------------
-- Voice state: tracks a playing SFX
------------------------------------------------------------

function Synth.new()
    return setmetatable({
        playing = false,
        sfx = nil,          -- SFX data reference
        row = 0,            -- current row (0-indexed)
        tick = 0,           -- sample counter within current row
        samples_per_row = 0,
        phase = 0.0,        -- oscillator phase [0, 1)
        freq = 440,         -- current frequency
        vol = 1.0,          -- current volume [0, 1]
        waveform = 0,       -- current waveform ID
        effect = 0,         -- current effect
        effect_param = 0,   -- effect-specific state
        base_freq = 440,    -- frequency at row start (for effects)
        vib_phase = 0,      -- vibrato LFO phase
        arp_tick = 0,       -- arpeggio cycle counter
    }, Synth)
end

-- Start playing an SFX
function Synth:play(sfx)
    self.sfx = sfx
    self.row = 0
    self.tick = 0
    self.phase = 0
    self.vib_phase = 0
    self.arp_tick = 0
    self.playing = true
    -- speed is in 120ths of a second per note
    self.samples_per_row = floor(Synth.SAMPLE_RATE * sfx.speed / 120)
    if self.samples_per_row < 1 then self.samples_per_row = 1 end
    self:_load_row()
end

-- Stop playback
function Synth:stop()
    self.playing = false
    self.sfx = nil
end

-- Load parameters from current row
function Synth:_load_row()
    local sfx = self.sfx
    if not sfx then return end
    local note = sfx.notes[self.row + 1]  -- 1-indexed Lua table
    if not note then
        self.playing = false
        return
    end

    if note.pitch >= 0 then
        self.freq = NOTE_FREQ[note.pitch] or 440
        self.base_freq = self.freq
    end
    self.vol = note.volume / 7  -- 0-7 → 0.0-1.0
    self.waveform = note.waveform
    self.effect = note.effect
    self.arp_tick = 0
end

-- Apply per-sample effects
function Synth:_apply_effects()
    local fx = self.effect

    if fx == Synth.FX_SLIDE_UP then
        -- Slide up: increase freq by ~1% per sample
        self.freq = self.freq * 1.0003
        if self.freq > 20000 then self.freq = 20000 end

    elseif fx == Synth.FX_SLIDE_DN then
        self.freq = self.freq * 0.9997
        if self.freq < 20 then self.freq = 20 end

    elseif fx == Synth.FX_DROP then
        -- Fast drop
        self.freq = self.freq * 0.998
        if self.freq < 20 then self.freq = 20 end

    elseif fx == Synth.FX_VIBRATO then
        -- LFO at ~6 Hz, ±2% frequency deviation
        self.vib_phase = self.vib_phase + 6 / Synth.SAMPLE_RATE
        if self.vib_phase >= 1 then self.vib_phase = self.vib_phase - 1 end
        local mod = sin(pi2 * self.vib_phase) * 0.02
        self.freq = self.base_freq * (1 + mod)

    elseif fx == Synth.FX_ARP_MIN then
        -- Minor arpeggio: root, +3, +7 semitones
        local cycle = floor(self.arp_tick / (Synth.SAMPLE_RATE / 30)) % 3
        local semi = cycle == 0 and 0 or (cycle == 1 and 3 or 7)
        self.freq = self.base_freq * (2 ^ (semi / 12))

    elseif fx == Synth.FX_ARP_MAJ then
        -- Major arpeggio: root, +4, +7 semitones
        local cycle = floor(self.arp_tick / (Synth.SAMPLE_RATE / 30)) % 3
        local semi = cycle == 0 and 0 or (cycle == 1 and 4 or 7)
        self.freq = self.base_freq * (2 ^ (semi / 12))

    elseif fx == Synth.FX_FADEOUT then
        -- Linear fade to zero over the row duration
        local progress = self.tick / self.samples_per_row
        self.vol = self.vol * (1 - progress)
    end

    self.arp_tick = self.arp_tick + 1
end

-- Render samples into a table (stereo interleaved int16)
-- Returns: table of int16 values, count of values
function Synth:render(max_frames)
    if not self.playing then return nil, 0 end

    local samples = {}
    local count = 0
    local wave_fn = WAVE_FN[self.waveform] or wave_square

    for _ = 1, max_frames do
        if not self.playing then break end

        -- Apply effects
        self:_apply_effects()

        -- Generate sample
        local s = wave_fn(self.phase) * self.vol

        -- Convert to int16 range
        local i16 = floor(s * 16000 + 0.5)
        if i16 > 32767 then i16 = 32767 end
        if i16 < -32768 then i16 = -32768 end

        -- Stereo: same on both channels
        count = count + 1
        samples[count] = i16
        count = count + 1
        samples[count] = i16

        -- Advance phase
        self.phase = self.phase + self.freq / Synth.SAMPLE_RATE
        if self.phase >= 1 then self.phase = self.phase - 1 end

        -- Advance row tick
        self.tick = self.tick + 1
        if self.tick >= self.samples_per_row then
            self.tick = 0
            self.row = self.row + 1

            -- Check loop
            local sfx = self.sfx
            if sfx.loop_end > sfx.loop_start and self.row >= sfx.loop_end then
                self.row = sfx.loop_start
            end

            -- Check end
            if self.row >= sfx.note_count then
                self.playing = false
                break
            end

            self:_load_row()
            wave_fn = WAVE_FN[self.waveform] or wave_square
        end
    end

    return samples, count
end

return Synth
