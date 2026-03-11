// MUS music player for PicOS DOOM
// MUS sequencer + OPL2 (Nuked-OPL3) synthesizer
//
// References:
//   - DOOM source: mus2mid.c for MUS event format
//   - GENMIDI lump: 175 instruments × 36 bytes each, "#OPL_II#" header
//   - OPL2 register map: Yamaha YM3812 datasheet
//   - MUS format: https://doomwiki.org/wiki/MUS

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "doomtype.h"
#include "w_wad.h"
#include "z_zone.h"

#include "mus_player.h"
#include "opl.h"

// Debug: enable voice allocation logging
#define MUS_DEBUG_VOICE

// --- Constants ---

#define MUS_RATE          140     // MUS ticks per second
#define GAME_RATE         35      // Game ticks per second
#define MUS_TICKS_PER_GAME (MUS_RATE / GAME_RATE)  // = 4
#define OUTPUT_RATE       11025   // Output sample rate (matches SFX mixer)
#define OPL_RATE          11025   // Run OPL at output rate (no resampling needed)

#define NUM_OPL_VOICES    9       // OPL2 melodic voices
#define NUM_MUS_CHANNELS  16      // MUS channels (0-14 melodic, 15 percussion)
#define MUS_PERCUSSION_CHAN 15

// GENMIDI lump constants
#define GENMIDI_HEADER    "#OPL_II#"
#define GENMIDI_HEADER_LEN 8
#define GENMIDI_NUM_INSTRS 175    // 128 melodic + 47 percussion (128-174)
#define GENMIDI_INSTR_SIZE 36     // Bytes per instrument

// MUS event types (from mus2mid.c)
#define MUS_RELEASEKEY    0x00
#define MUS_PRESSKEY      0x10
#define MUS_PITCHWHEEL    0x20
#define MUS_SYSTEMEVENT   0x30
#define MUS_CHANGECONTROL 0x40
#define MUS_SCOREEND      0x60

// --- GENMIDI instrument data ---
// Each instrument has two operators (modulator + carrier)
// Layout per operator (13 bytes):
//   [0] reg20: tremolo/vibrato/sustain/KSR/mult
//   [1] reg40: KSL/total level
//   [2] reg60: attack/decay
//   [3] reg80: sustain/release
//   [4] regE0: waveform
//   [5] regC0: feedback/connection (only from first operator block)
// Full instrument (36 bytes):
//   [0-1]   flags (uint16_t LE): bit 0 = fixed pitch, bit 2 = double-voice
//   [2]     fine tuning
//   [3]     fixed note number
//   [4-16]  voice 0: modulator (6 bytes) + carrier (6 bytes) + padding
//   [17]    padding
//   [18-30] voice 1 (for double-voice instruments, not used here)
//   [31]    padding
//   [32-35] note offset (int16_t LE × 2, for voice 0 and voice 1)

typedef struct {
    uint8_t tremolo_vib;   // reg 0x20: tremolo/vibrato/sustain/KSR/mult
    uint8_t attack_decay;  // reg 0x60: attack/decay
    uint8_t sustain_rel;   // reg 0x80: sustain/release
    uint8_t waveform;      // reg 0xE0: waveform select
    uint8_t scale;         // reg 0x40 bits 6-7: KSL
    uint8_t level;         // reg 0x40 bits 0-5: total output level
} genmidi_op_t;

typedef struct {
    uint16_t    flags;
    uint8_t     fine_tune;
    uint8_t     fixed_note;
    genmidi_op_t modulator;
    uint8_t      feedback_con; // reg 0xC0: feedback(bits 1-3) | connection(bit 0)
    genmidi_op_t carrier;
    uint8_t      pad;
    // Second voice data (skip for OPL2 - only 9 voices)
    genmidi_op_t modulator2;
    uint8_t      feedback_con2;
    genmidi_op_t carrier2;
    uint8_t      pad2;
    int16_t      note_offset1;
    int16_t      note_offset2;
} genmidi_instr_t;

// --- Song structure ---

struct mus_song {
    const uint8_t *data;   // Raw MUS lump data
    int            len;    // Length
    uint16_t       score_start; // Offset to score data
    uint16_t       score_len;
};

// --- OPL voice state ---

typedef struct {
    uint8_t  active;        // Voice is sounding
    uint8_t  mus_channel;   // Which MUS channel owns this voice
    uint8_t  note;          // MIDI note number
    uint8_t  velocity;      // Note velocity
    uint32_t age;           // Allocation age counter (for voice stealing)
    uint8_t  released;      // Key has been released (in release phase)
    uint16_t f_num;         // OPL frequency number (for key_off)
    uint8_t  block;         // OPL block/octave (for key_off)
} opl_voice_t;

// --- Player state ---

static struct {
    opl3_chip      opl;
    genmidi_instr_t instruments[GENMIDI_NUM_INSTRS];
    opl_voice_t    voices[NUM_OPL_VOICES];

    // Per-MUS-channel state
    struct {
        uint8_t  instrument;   // Current instrument (MIDI program)
        uint8_t  volume;       // Channel volume (0-127)
        uint8_t  pan;          // Pan (0=left, 64=center, 127=right) — ignored (mono OPL2)
        int16_t  pitch_bend;   // Pitch bend value (0-255, 128=center)
    } channels[NUM_MUS_CHANNELS];

    mus_song_t    *current_song;
    const uint8_t *score_pos;  // Current position in score data
    const uint8_t *score_end;  // End of score data
    int            delay;      // MUS ticks remaining before next event
    int            master_vol; // Master volume (0-127)
    uint32_t       voice_age;  // Monotonic counter for voice allocation
    int            playing;
    int            paused;
    int            looping;
    int            initialized;

} s_mus;

// --- OPL register helpers ---

// Slot offsets for the 9 OPL2 channels
// OPL2 has 18 slots (2 per channel). Register addresses use this mapping:
//   Channel:  0  1  2  3  4  5  6  7  8
//   Mod slot: 0  1  2  8  9 10 16 17 18  (but in register space:)
//   The register offset for modulator = slot_offsets[ch]
//   The register offset for carrier   = slot_offsets[ch] + 3
static const uint8_t slot_offsets[9] = {
    0x00, 0x01, 0x02, 0x08, 0x09, 0x0A, 0x10, 0x11, 0x12
};

static void opl_write(uint16_t reg, uint8_t val)
{
    OPL3_WriteRegBuffered(&s_mus.opl, reg, val);
}

// --- Frequency table ---
// F-number values for each semitone in the lowest octave
// OPL f_num at block 0 for notes C through B
static const uint16_t fnum_table[12] = {
    // C     C#    D     D#    E     F     F#    G     G#    A     A#    B
    345,  365,  387,  410,  435,  460,  488,  517,  547,  580,  614,  651
};

// Convert MIDI note + pitch bend to OPL f_num and block
// fnum_table values are for 49716Hz. Since OPL runs at 11025Hz, the phase
// accumulator advances 49716/11025 ≈ 4.51× slower per sample. Compensate by
// scaling f-num up by that ratio, then shift excess into block (octave).
static void note_to_freq(uint8_t note, int16_t bend,
                         uint16_t *f_num, uint8_t *block)
{
    if (note > 127) note = 127;

    int semitone = note % 12;
    int octave   = note / 12;

    // Scale f-num for 11025Hz: multiply by 49716/11025
    uint32_t freq = (uint32_t)fnum_table[semitone] * 49716 / 11025;

    // Apply pitch bend: bend 128 = center, range ±2 semitones
    if (bend != 128) {
        int delta = bend - 128;
        freq = (uint32_t)((int32_t)freq + ((int32_t)freq * delta) / 768);
    }

    // The scaled f-num is ~4.51× larger, which is ~2^2.17. Add 2 to octave
    // to absorb most of the scaling, then normalize into 10-bit f-num range.
    octave += 2;

    while (freq > 1023 && octave < 7) {
        freq >>= 1;
        octave++;
    }
    if (freq > 1023) freq = 1023;
    if (octave > 7) octave = 7;

    *f_num = (uint16_t)freq;
    *block = (uint8_t)octave;
}

// --- Voice allocation ---

static int alloc_voice(uint8_t mus_channel)
{
    // 1. Find a free voice
    for (int i = 0; i < NUM_OPL_VOICES; i++) {
        if (!s_mus.voices[i].active) {
#ifdef MUS_DEBUG_VOICE
            printf("[MUS] alloc_voice: ch=%d -> voice=%d (free)\n", mus_channel, i);
#endif
            return i;
        }
    }

    // 2. Steal oldest released voice
    int oldest_idx = -1;
    uint32_t oldest_age = UINT32_MAX;
    for (int i = 0; i < NUM_OPL_VOICES; i++) {
        if (s_mus.voices[i].released && s_mus.voices[i].age < oldest_age) {
            oldest_age = s_mus.voices[i].age;
            oldest_idx = i;
        }
    }
    if (oldest_idx >= 0) {
#ifdef MUS_DEBUG_VOICE
        printf("[MUS] alloc_voice: ch=%d -> voice=%d (steal released, age=%u)\n", 
               mus_channel, oldest_idx, oldest_age);
#endif
        return oldest_idx;
    }

    // 3. Steal oldest active voice
    oldest_age = UINT32_MAX;
    for (int i = 0; i < NUM_OPL_VOICES; i++) {
        if (s_mus.voices[i].age < oldest_age) {
            oldest_age = s_mus.voices[i].age;
            oldest_idx = i;
        }
    }
#ifdef MUS_DEBUG_VOICE
    printf("[MUS] alloc_voice: ch=%d -> voice=%d (steal active, age=%u)\n", 
           mus_channel, oldest_idx, oldest_age);
#endif
    return oldest_idx >= 0 ? oldest_idx : 0;
}

// --- Voice configuration ---

static void voice_key_off(int voice)
{
    // Use the stored f_num/block from when this voice was configured,
    // NOT the current channel values which may have been overwritten
    opl_voice_t *v = &s_mus.voices[voice];
    uint8_t val = ((v->f_num >> 8) & 3) | (v->block << 2);
    opl_write(0xB0 + voice, val);
    v->released = 1;
#ifdef MUS_DEBUG_VOICE
    printf("[MUS] voice_key_off: voice=%d\n", voice);
#endif
}

// Force a voice to silence by resetting its envelope state
// This ensures a stolen voice doesn't bleed into the new note
static void voice_force_silence(int voice)
{
#ifdef MUS_DEBUG_VOICE
    printf("[MUS] voice_force_silence: voice=%d (was active=%d, released=%d)\n", 
           voice, s_mus.voices[voice].active, s_mus.voices[voice].released);
#endif

    struct opl_channel *chan = &s_mus.opl.channel[voice];
    uint8_t mod_off = slot_offsets[voice];
    uint8_t car_off = mod_off + 3;

    // Force key off
    uint8_t val = ((chan->f_num >> 8) & 3) | (chan->block << 2);
    opl_write(0xB0 + voice, val);

    // Force FAST release instead of cutting off completely
    // This prevents clicks/pops while still silencing quickly
    // Set release rate to maximum (0x0F) for both modulator and carrier
    opl_write(0x80 + mod_off, 0x0F);  // Max release on modulator
    opl_write(0x80 + car_off, 0x0F);  // Max release on carrier
    
    // Also set total levels to maximum attenuation for immediate silence
    opl_write(0x40 + mod_off, 0x3F);  // Max attenuation on modulator
    opl_write(0x40 + car_off, 0x3F);  // Max attenuation on carrier
}

static void voice_configure(int voice, const genmidi_instr_t *instr,
                            uint8_t note, uint8_t velocity,
                            uint8_t mus_channel)
{
    uint8_t mod_off = slot_offsets[voice];
    uint8_t car_off = mod_off + 3;

    // Save old f_num/block for pending key_off if voice is being stolen
    uint16_t old_f_num = 0;
    uint8_t old_block = 0;
    if (s_mus.voices[voice].active) {
        old_f_num = s_mus.voices[voice].f_num;
        old_block = s_mus.voices[voice].block;
        // Only force silence if NOT already in release phase
        // If already released, the envelope is already decaying naturally
        if (!s_mus.voices[voice].released) {
            voice_force_silence(voice);
        }
    }

    // Configure modulator
    opl_write(0x20 + mod_off, instr->modulator.tremolo_vib);
    opl_write(0x60 + mod_off, instr->modulator.attack_decay);
    opl_write(0x80 + mod_off, instr->modulator.sustain_rel);
    opl_write(0xE0 + mod_off, instr->modulator.waveform);

    // Configure carrier
    opl_write(0x20 + car_off, instr->carrier.tremolo_vib);
    opl_write(0x60 + car_off, instr->carrier.attack_decay);
    opl_write(0x80 + car_off, instr->carrier.sustain_rel);
    opl_write(0xE0 + car_off, instr->carrier.waveform);

    // Feedback / connection
    opl_write(0xC0 + voice, instr->feedback_con);

    // Set volumes with velocity and channel volume scaling
    int ch_vol = s_mus.channels[mus_channel].volume;
    int vol_scale = (velocity * ch_vol * s_mus.master_vol) / (127 * 127);

    // Modulator level: from instrument patch (0x40)
    // For FM mode (con=0): modulator TL stays as-is (controls modulation depth)
    // For additive mode (con=1): modulator TL is also volume-scaled
    uint8_t mod_tl = (instr->modulator.scale | instr->modulator.level);
    if (instr->feedback_con & 1) {
        // Additive: scale modulator volume too
        int mod_atten = mod_tl & 0x3F;
        mod_atten = 63 - ((63 - mod_atten) * vol_scale / 127);
        if (mod_atten > 63) mod_atten = 63;
        if (mod_atten < 0) mod_atten = 0;
        mod_tl = (mod_tl & 0xC0) | mod_atten;
    }
    opl_write(0x40 + mod_off, mod_tl);

    // Carrier level: always volume-scaled
    uint8_t car_tl = (instr->carrier.scale | instr->carrier.level);
    int car_atten = car_tl & 0x3F;
    car_atten = 63 - ((63 - car_atten) * vol_scale / 127);
    if (car_atten > 63) car_atten = 63;
    if (car_atten < 0) car_atten = 0;
    opl_write(0x40 + car_off, (car_tl & 0xC0) | car_atten);

    // Set frequency and key-on
    uint16_t f_num;
    uint8_t  block;

    // Fixed-pitch instruments use the fixed note
    uint8_t play_note = (instr->flags & 0x01) ? instr->fixed_note : note;

    // Apply note offset
    int adjusted = (int)play_note + instr->note_offset1;
    if (adjusted < 0) adjusted = 0;
    if (adjusted > 127) adjusted = 127;

    note_to_freq((uint8_t)adjusted,
                 s_mus.channels[mus_channel].pitch_bend,
                 &f_num, &block);

    opl_write(0xA0 + voice, f_num & 0xFF);
    opl_write(0xB0 + voice, 0x20 | ((f_num >> 8) & 3) | (block << 2));

    // Update voice state
    opl_voice_t *v = &s_mus.voices[voice];
    v->active = 1;
    v->mus_channel = mus_channel;
    v->note = note;
    v->velocity = velocity;
    v->age = s_mus.voice_age++;
    v->released = 0;
    v->f_num = f_num;
    v->block = block;
    // If there was an old note being released, preserve its values for key_off
    if (old_f_num != 0 || old_block != 0) {
        v->f_num = old_f_num;
        v->block = old_block;
    }
}

// --- GENMIDI loading ---

static int load_genmidi(void)
{
    int lump = W_CheckNumForName("GENMIDI");
    if (lump < 0)
        return 0;

    int lump_len = W_LumpLength(lump);
    int expected = GENMIDI_HEADER_LEN + GENMIDI_NUM_INSTRS * GENMIDI_INSTR_SIZE;
    if (lump_len < expected)
        return 0;

    const uint8_t *raw = W_CacheLumpNum(lump, PU_STATIC);
    if (!raw)
        return 0;

    // Validate header
    if (memcmp(raw, GENMIDI_HEADER, GENMIDI_HEADER_LEN) != 0) {
        W_ReleaseLumpNum(lump);
        return 0;
    }

    // Parse 175 instruments
    const uint8_t *p = raw + GENMIDI_HEADER_LEN;
    for (int i = 0; i < GENMIDI_NUM_INSTRS; i++) {
        genmidi_instr_t *inst = &s_mus.instruments[i];

        inst->flags = p[0] | (p[1] << 8);
        inst->fine_tune = p[2];
        inst->fixed_note = p[3];

        // Voice 0 modulator (6 bytes at offset 4)
        inst->modulator.tremolo_vib  = p[4];   // reg 0x20
        inst->modulator.attack_decay = p[5];   // reg 0x60
        inst->modulator.sustain_rel  = p[6];   // reg 0x80
        inst->modulator.waveform     = p[7];   // reg 0xE0
        inst->modulator.scale        = p[8];   // reg 0x40 upper (KSL)
        inst->modulator.level        = p[9];   // reg 0x40 lower (TL)
        inst->feedback_con           = p[10];  // reg 0xC0

        // Voice 0 carrier (6 bytes at offset 11)
        inst->carrier.tremolo_vib  = p[11];
        inst->carrier.attack_decay = p[12];
        inst->carrier.sustain_rel  = p[13];
        inst->carrier.waveform     = p[14];
        inst->carrier.scale        = p[15];
        inst->carrier.level        = p[16];
        // p[17] = padding
        inst->note_offset1 = (int16_t)(p[18] | (p[19] << 8));

        // Voice 1 (offset 20, 16 bytes) - skip for OPL2
        inst->modulator2.tremolo_vib  = p[20];
        inst->modulator2.attack_decay = p[21];
        inst->modulator2.sustain_rel  = p[22];
        inst->modulator2.waveform     = p[23];
        inst->modulator2.scale        = p[24];
        inst->modulator2.level        = p[25];
        inst->feedback_con2           = p[26];
        inst->carrier2.tremolo_vib  = p[27];
        inst->carrier2.attack_decay = p[28];
        inst->carrier2.sustain_rel  = p[29];
        inst->carrier2.waveform     = p[30];
        inst->carrier2.scale        = p[31];
        inst->carrier2.level        = p[32];
        // p[33] = padding
        inst->note_offset2 = (int16_t)(p[34] | (p[35] << 8));

        // Skip remaining padding to reach 36 bytes
        p += GENMIDI_INSTR_SIZE;
    }

    return 1;
}

// --- MUS sequencer ---

// Process one MUS event, return false on score end
static int process_mus_event(void)
{
    if (s_mus.score_pos >= s_mus.score_end)
        return 0;

    uint8_t desc = *s_mus.score_pos++;
    uint8_t mus_chan = desc & 0x0F;
    uint8_t event_type = desc & 0x70;
    int has_delay = (desc & 0x80) != 0;

    switch (event_type) {
    case MUS_RELEASEKEY: {
        if (s_mus.score_pos >= s_mus.score_end) return 0;
        uint8_t note = *s_mus.score_pos++ & 0x7F;
        // Find the voice playing this note on this channel and release it
        for (int i = 0; i < NUM_OPL_VOICES; i++) {
            if (s_mus.voices[i].active &&
                s_mus.voices[i].mus_channel == mus_chan &&
                s_mus.voices[i].note == note &&
                !s_mus.voices[i].released) {
                voice_key_off(i);
                break;
            }
        }
        break;
    }

    case MUS_PRESSKEY: {
        if (s_mus.score_pos >= s_mus.score_end) return 0;
        uint8_t key = *s_mus.score_pos++;
        uint8_t note = key & 0x7F;
        uint8_t velocity;

        if (key & 0x80) {
            // Has velocity byte
            if (s_mus.score_pos >= s_mus.score_end) return 0;
            velocity = *s_mus.score_pos++ & 0x7F;
        } else {
            velocity = 64; // Default velocity
        }

        // Get instrument for this channel
        uint8_t instr_idx;
        if (mus_chan == MUS_PERCUSSION_CHAN) {
            // Percussion: GENMIDI instruments 128-174, indexed by note-35
            int perc_idx = 128 + (note - 35);
            if (perc_idx < 128 || perc_idx >= GENMIDI_NUM_INSTRS)
                break;
            instr_idx = perc_idx;
        } else {
            instr_idx = s_mus.channels[mus_chan].instrument;
            if (instr_idx >= 128) instr_idx = 0;
        }

        // Allocate a voice and configure it
        int voice = alloc_voice(mus_chan);
        if (voice >= 0) {
            voice_configure(voice, &s_mus.instruments[instr_idx],
                          note, velocity, mus_chan);
        }
        break;
    }

    case MUS_PITCHWHEEL: {
        if (s_mus.score_pos >= s_mus.score_end) return 0;
        uint8_t bend_val = *s_mus.score_pos++;
        s_mus.channels[mus_chan].pitch_bend = bend_val;

        // Update frequency of all active voices on this channel
        for (int i = 0; i < NUM_OPL_VOICES; i++) {
            if (s_mus.voices[i].active &&
                s_mus.voices[i].mus_channel == mus_chan &&
                !s_mus.voices[i].released) {
                uint8_t play_note = s_mus.voices[i].note;
                uint8_t instr_idx = s_mus.channels[mus_chan].instrument;
                if (mus_chan == MUS_PERCUSSION_CHAN)
                    break; // Don't bend percussion
                if (instr_idx >= 128) instr_idx = 0;
                const genmidi_instr_t *instr = &s_mus.instruments[instr_idx];
                if (instr->flags & 0x01)
                    play_note = instr->fixed_note;
                int adjusted = play_note + instr->note_offset1;
                if (adjusted < 0) adjusted = 0;
                if (adjusted > 127) adjusted = 127;

                uint16_t f_num;
                uint8_t block;
                note_to_freq((uint8_t)adjusted, bend_val, &f_num, &block);
                opl_write(0xA0 + i, f_num & 0xFF);
                opl_write(0xB0 + i, 0x20 | ((f_num >> 8) & 3) | (block << 2));
            }
        }
        break;
    }

    case MUS_SYSTEMEVENT: {
        if (s_mus.score_pos >= s_mus.score_end) return 0;
        // System events: all-notes-off, etc. — just consume the byte
        s_mus.score_pos++;
        break;
    }

    case MUS_CHANGECONTROL: {
        if (s_mus.score_pos >= s_mus.score_end) return 0;
        uint8_t ctrl = *s_mus.score_pos++;
        if (s_mus.score_pos >= s_mus.score_end) return 0;
        uint8_t value = *s_mus.score_pos++;

        switch (ctrl) {
        case 0: // Instrument change
            s_mus.channels[mus_chan].instrument = value;
            break;
        case 3: // Volume
            s_mus.channels[mus_chan].volume = value;
            // Update volume of active voices on this channel
            for (int i = 0; i < NUM_OPL_VOICES; i++) {
                if (s_mus.voices[i].active &&
                    s_mus.voices[i].mus_channel == mus_chan &&
                    !s_mus.voices[i].released) {
                    uint8_t instr_idx = s_mus.channels[mus_chan].instrument;
                    if (mus_chan == MUS_PERCUSSION_CHAN) {
                        int perc_idx = 128 + (s_mus.voices[i].note - 35);
                        if (perc_idx >= 128 && perc_idx < GENMIDI_NUM_INSTRS)
                            instr_idx = perc_idx;
                        else
                            continue;
                    }
                    if (instr_idx >= 128 && mus_chan != MUS_PERCUSSION_CHAN)
                        instr_idx = 0;
                    const genmidi_instr_t *instr = &s_mus.instruments[instr_idx];
                    int vol_scale = (s_mus.voices[i].velocity * value * s_mus.master_vol)
                                  / (127 * 127);

                    // Update carrier volume
                    uint8_t car_off = slot_offsets[i] + 3;
                    uint8_t car_tl = (instr->carrier.scale | instr->carrier.level);
                    int car_atten = car_tl & 0x3F;
                    car_atten = 63 - ((63 - car_atten) * vol_scale / 127);
                    if (car_atten > 63) car_atten = 63;
                    if (car_atten < 0) car_atten = 0;
                    opl_write(0x40 + car_off, (car_tl & 0xC0) | car_atten);

                    // If additive, update modulator too
                    if (instr->feedback_con & 1) {
                        uint8_t mod_off = slot_offsets[i];
                        uint8_t mod_tl = (instr->modulator.scale | instr->modulator.level);
                        int mod_atten = mod_tl & 0x3F;
                        mod_atten = 63 - ((63 - mod_atten) * vol_scale / 127);
                        if (mod_atten > 63) mod_atten = 63;
                        if (mod_atten < 0) mod_atten = 0;
                        opl_write(0x40 + mod_off, (mod_tl & 0xC0) | mod_atten);
                    }
                }
            }
            break;
        case 4: // Pan — OPL2 is mono, ignore
            s_mus.channels[mus_chan].pan = value;
            break;
        default:
            break;
        }
        break;
    }

    case MUS_SCOREEND:
        if (s_mus.looping && s_mus.current_song) {
            // Loop: reset position to start of score
            s_mus.score_pos = s_mus.current_song->data + s_mus.current_song->score_start;
            s_mus.delay = 0;
        } else {
            s_mus.playing = 0;
        }
        return s_mus.playing;

    default:
        // Unknown event — skip
        break;
    }

    // Read delay if bit 7 was set
    if (has_delay && s_mus.score_pos < s_mus.score_end) {
        int delay = 0;
        for (;;) {
            if (s_mus.score_pos >= s_mus.score_end) break;
            uint8_t b = *s_mus.score_pos++;
            delay = delay * 128 + (b & 0x7F);
            if (!(b & 0x80)) break;
        }
        s_mus.delay += delay;
    }

    return 1;
}

// --- Public API ---

void mus_init(void)
{
    memset(&s_mus, 0, sizeof(s_mus));

    OPL3_Reset(&s_mus.opl, OPL_RATE);

    // Enable waveform select (register 0x01 bit 5)
    opl_write(0x01, 0x20);

    // Initialize channel defaults
    for (int i = 0; i < NUM_MUS_CHANNELS; i++) {
        s_mus.channels[i].volume = 100;
        s_mus.channels[i].pitch_bend = 128; // Center
        s_mus.channels[i].pan = 64;         // Center
    }

    s_mus.master_vol = 127;

    if (!load_genmidi()) {
        // No GENMIDI lump — music won't work but won't crash
        return;
    }

    s_mus.initialized = 1;
}

void mus_shutdown(void)
{
    mus_stop();
    s_mus.initialized = 0;
}

mus_song_t *mus_register(const void *data, int len)
{
    if (!data || len < 16)
        return NULL;

    const uint8_t *raw = (const uint8_t *)data;

    // Validate MUS header: "MUS\x1A"
    if (raw[0] != 'M' || raw[1] != 'U' || raw[2] != 'S' || raw[3] != 0x1A)
        return NULL;

    mus_song_t *song = malloc(sizeof(mus_song_t));
    if (!song)
        return NULL;

    song->data = raw;
    song->len  = len;
    song->score_start = raw[6] | (raw[7] << 8);  // scorestart at offset 6
    song->score_len   = raw[4] | (raw[5] << 8);  // scorelength at offset 4

    if (song->score_start >= len) {
        free(song);
        return NULL;
    }

    return song;
}

void mus_unregister(mus_song_t *song)
{
    if (song) {
        if (s_mus.current_song == song)
            mus_stop();
        free(song);
    }
}

void mus_play(mus_song_t *song, int looping)
{
    if (!s_mus.initialized || !song)
        return;

    mus_stop();

    s_mus.current_song = song;
    s_mus.score_pos = song->data + song->score_start;
    s_mus.score_end = song->data + song->len;
    s_mus.delay = 0;
    s_mus.looping = looping;
    s_mus.playing = 1;
    s_mus.paused = 0;

    // Reset channel state
    for (int i = 0; i < NUM_MUS_CHANNELS; i++) {
        s_mus.channels[i].instrument = 0;
        s_mus.channels[i].volume = 100;
        s_mus.channels[i].pitch_bend = 128;
        s_mus.channels[i].pan = 64;
    }

}

void mus_stop(void)
{
    s_mus.playing = 0;
    s_mus.current_song = NULL;

    // Silence all OPL voices
    for (int i = 0; i < NUM_OPL_VOICES; i++) {
        if (s_mus.voices[i].active) {
            voice_key_off(i);
            s_mus.voices[i].active = 0;
        }
    }
}

void mus_pause(void)
{
    if (s_mus.playing) {
        s_mus.paused = 1;
        // Key off all voices to silence during pause
        for (int i = 0; i < NUM_OPL_VOICES; i++) {
            if (s_mus.voices[i].active)
                voice_key_off(i);
        }
    }
}

void mus_resume(void)
{
    s_mus.paused = 0;
}

void mus_set_volume(int volume)
{
    if (volume < 0) volume = 0;
    if (volume > 127) volume = 127;
    s_mus.master_vol = volume;
}

int mus_is_playing(void)
{
    return s_mus.playing && !s_mus.paused;
}

void mus_tick_n(int n)
{
    if (!s_mus.playing || s_mus.paused || !s_mus.current_song)
        return;

    for (int t = 0; t < n; t++) {
        // Consume delay
        if (s_mus.delay > 0) {
            s_mus.delay--;
            continue;
        }

        // Process events until we hit a delay or end
        while (s_mus.delay == 0 && s_mus.playing) {
            if (!process_mus_event())
                break;
            if (s_mus.delay > 0) {
                s_mus.delay--;
                break;
            }
        }
    }

    // Age out released voices that have been silent long enough
    for (int i = 0; i < NUM_OPL_VOICES; i++) {
        if (s_mus.voices[i].active && s_mus.voices[i].released) {
            // Check if envelope has decayed
            if (s_mus.opl.slot[i * 2 + 1].eg_rout >= 0x1f0) {
                s_mus.voices[i].active = 0;
            }
        }
    }
}

void mus_tick(void)
{
    mus_tick_n(MUS_TICKS_PER_GAME);
}

void mus_render(int16_t *buf, int count)
{
    if (!s_mus.playing || s_mus.paused || !s_mus.initialized) {
        memset(buf, 0, count * 2 * sizeof(int16_t));
        return;
    }

    // Debug: count active voices
#ifdef MUS_DEBUG_VOICE
    int active = 0;
    for (int i = 0; i < NUM_OPL_VOICES; i++) {
        if (s_mus.voices[i].active) active++;
    }
    if (active > 6) {
        printf("[MUS] render: %d active voices\n", active);
    }
#endif

    // OPL runs at OUTPUT_RATE — 1:1, batch for performance
    OPL3_GenerateBatch(&s_mus.opl, buf, count);
}
