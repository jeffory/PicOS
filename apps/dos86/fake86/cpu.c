/*
  Fake86: A portable, open-source 8086 PC emulator.
  Copyright (C)2010-2013 Mike Chambers

  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License
  as published by the Free Software Foundation; either version 2
  of the License, or (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

/* cpu.c: functions to emulate the 8086/V20 CPU in software. the heart of Fake86.
   Adapted for PicOS/dos86: external memory pointers, no SDL/audio/video/timing. */

#include "config.h"
#include <stdint.h>
#include <stdio.h>
#include "cpu.h"
#include "disk.h"
#include "video.h"
#include "bios.h"

/* ---- modregrm decoder (no address mode cache) ---- */
static uint8_t addrbyte;
static uint16_t disp16;
static uint8_t disp8;

#define modregrm() { \
	addrbyte = getmem8(segregs[regcs], ip); \
	StepIP(1); \
	mode = addrbyte >> 6; \
	reg = (addrbyte >> 3) & 7; \
	rm = addrbyte & 7; \
	switch(mode) \
	{ \
	case 0: \
	if(rm == 6) { \
	disp16 = getmem16(segregs[regcs], ip); \
	StepIP(2); \
	} \
	if(((rm == 2) || (rm == 3)) && !segoverride) { \
	useseg = segregs[regss]; \
	} \
	break; \
 \
	case 1: \
	disp16 = signext(getmem8(segregs[regcs], ip)); \
	StepIP(1); \
	if(((rm == 2) || (rm == 3) || (rm == 6)) && !segoverride) { \
	useseg = segregs[regss]; \
	} \
	break; \
 \
	case 2: \
	disp16 = getmem16(segregs[regcs], ip); \
	StepIP(2); \
	if(((rm == 2) || (rm == 3) || (rm == 6)) && !segoverride) { \
	useseg = segregs[regss]; \
	} \
	break; \
 \
	default: \
	disp8 = 0; \
	disp16 = 0; \
	} \
}

/* ---- Parity lookup table ---- */
static const uint8_t parity[0x100] = {
	1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,
	0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1,
	0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1,
	1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,
	0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1,
	1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,
	1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,
	0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1
};

/* ---- Byte register index mapping ---- */
uint8_t byteregtable[8] = { regal, regcl, regdl, regbl, regah, regch, regdh, regbh };

/* ---- CPU state ---- */
uint8_t  opcode, segoverride, reptype, hltstate = 0;
uint16_t segregs[4], savecs, saveip, ip, useseg, oldsp;
uint8_t  tempcf, oldcf, cf, pf, af, zf, sf, tf, ifl, df, of, mode, reg, rm;
uint16_t oper1, oper2, res16, temp16, dummy, stacksize, frametemp;
uint8_t  oper1b, oper2b, res8, temp8, nestlev;
uint32_t temp1, temp2, temp3, temp4, temp5, temp32, tempaddr32, ea;
int32_t  result;
uint64_t totalexec;
union _bytewordregs_ regs;

uint8_t  running = 0, didbootstrap = 0;
uint8_t  bootdrive = 0;

/* Video BIOS interrupt handler — delegates to video.c */
void vidinterrupt(void) {
	video_int10h();
}

/* ---- Memory access ---- */

void write86(uint32_t addr32, uint8_t value) {
	tempaddr32 = addr32 & 0xFFFFF;

	/* Don't write to ROM areas */
	if (is_readonly(tempaddr32)) {
		return;
	}

	/* Video RAM: 0xA0000 - 0xBFFFF */
	if ((tempaddr32 >= 0xA0000) && (tempaddr32 <= 0xBFFFF)) {
		g_vram[tempaddr32 - 0xA0000] = value;
		video_set_dirty();
		return;
	}

	/* Conventional memory and everything else */
	g_ram[tempaddr32] = value;
}

void writew86(uint32_t addr32, uint16_t value) {
	write86(addr32, (uint8_t)value);
	write86(addr32 + 1, (uint8_t)(value >> 8));
}

uint8_t read86(uint32_t addr32) {
	addr32 &= 0xFFFFF;

	/* Video RAM: 0xA0000 - 0xBFFFF */
	if ((addr32 >= 0xA0000) && (addr32 <= 0xBFFFF)) {
		return g_vram[addr32 - 0xA0000];
	}

	/* Everything else (conventional + ROM area) comes from g_ram */
	return g_ram[addr32];
}

uint16_t readw86(uint32_t addr32) {
	return (uint16_t)read86(addr32) | (uint16_t)(read86(addr32 + 1) << 8);
}

/* ---- Flag helpers ---- */

static void flag_szp8(uint8_t value) {
	zf = !value;
	sf = (value & 0x80) ? 1 : 0;
	pf = parity[value];
}

static void flag_szp16(uint16_t value) {
	zf = !value;
	sf = (value & 0x8000) ? 1 : 0;
	pf = parity[value & 255];
}

static void flag_log8(uint8_t value) {
	flag_szp8(value);
	cf = 0;
	of = 0;
}

static void flag_log16(uint16_t value) {
	flag_szp16(value);
	cf = 0;
	of = 0;
}

static void flag_adc8(uint8_t v1, uint8_t v2, uint8_t v3) {
	uint16_t dst = (uint16_t)v1 + (uint16_t)v2 + (uint16_t)v3;
	flag_szp8((uint8_t)dst);
	of = (((dst ^ v1) & (dst ^ v2) & 0x80) == 0x80) ? 1 : 0;
	cf = (dst & 0xFF00) ? 1 : 0;
	af = (((v1 ^ v2 ^ dst) & 0x10) == 0x10) ? 1 : 0;
}

static void flag_adc16(uint16_t v1, uint16_t v2, uint16_t v3) {
	uint32_t dst = (uint32_t)v1 + (uint32_t)v2 + (uint32_t)v3;
	flag_szp16((uint16_t)dst);
	of = ((((dst ^ v1) & (dst ^ v2)) & 0x8000) == 0x8000) ? 1 : 0;
	cf = (dst & 0xFFFF0000) ? 1 : 0;
	af = (((v1 ^ v2 ^ dst) & 0x10) == 0x10) ? 1 : 0;
}

static void flag_add8(uint8_t v1, uint8_t v2) {
	uint16_t dst = (uint16_t)v1 + (uint16_t)v2;
	flag_szp8((uint8_t)dst);
	cf = (dst & 0xFF00) ? 1 : 0;
	of = (((dst ^ v1) & (dst ^ v2) & 0x80) == 0x80) ? 1 : 0;
	af = (((v1 ^ v2 ^ dst) & 0x10) == 0x10) ? 1 : 0;
}

static void flag_add16(uint16_t v1, uint16_t v2) {
	uint32_t dst = (uint32_t)v1 + (uint32_t)v2;
	flag_szp16((uint16_t)dst);
	cf = (dst & 0xFFFF0000) ? 1 : 0;
	of = (((dst ^ v1) & (dst ^ v2) & 0x8000) == 0x8000) ? 1 : 0;
	af = (((v1 ^ v2 ^ dst) & 0x10) == 0x10) ? 1 : 0;
}

static void flag_sbb8(uint8_t v1, uint8_t v2, uint8_t v3) {
	v2 += v3;
	uint16_t dst = (uint16_t)v1 - (uint16_t)v2;
	flag_szp8((uint8_t)dst);
	cf = (dst & 0xFF00) ? 1 : 0;
	of = ((dst ^ v1) & (v1 ^ v2) & 0x80) ? 1 : 0;
	af = ((v1 ^ v2 ^ dst) & 0x10) ? 1 : 0;
}

static void flag_sbb16(uint16_t v1, uint16_t v2, uint16_t v3) {
	v2 += v3;
	uint32_t dst = (uint32_t)v1 - (uint32_t)v2;
	flag_szp16((uint16_t)dst);
	cf = (dst & 0xFFFF0000) ? 1 : 0;
	of = ((dst ^ v1) & (v1 ^ v2) & 0x8000) ? 1 : 0;
	af = ((v1 ^ v2 ^ dst) & 0x10) ? 1 : 0;
}

static void flag_sub8(uint8_t v1, uint8_t v2) {
	uint16_t dst = (uint16_t)v1 - (uint16_t)v2;
	flag_szp8((uint8_t)dst);
	cf = (dst & 0xFF00) ? 1 : 0;
	of = ((dst ^ v1) & (v1 ^ v2) & 0x80) ? 1 : 0;
	af = ((v1 ^ v2 ^ dst) & 0x10) ? 1 : 0;
}

static void flag_sub16(uint16_t v1, uint16_t v2) {
	uint32_t dst = (uint32_t)v1 - (uint32_t)v2;
	flag_szp16((uint16_t)dst);
	cf = (dst & 0xFFFF0000) ? 1 : 0;
	of = ((dst ^ v1) & (v1 ^ v2) & 0x8000) ? 1 : 0;
	af = ((v1 ^ v2 ^ dst) & 0x10) ? 1 : 0;
}

/* ---- ALU operations ---- */

static void op_adc8(void)  { res8  = oper1b + oper2b + cf; flag_adc8(oper1b, oper2b, cf); }
static void op_adc16(void) { res16 = oper1 + oper2 + cf;   flag_adc16(oper1, oper2, cf); }
static void op_add8(void)  { res8  = oper1b + oper2b;      flag_add8(oper1b, oper2b); }
static void op_add16(void) { res16 = oper1 + oper2;        flag_add16(oper1, oper2); }
static void op_and8(void)  { res8  = oper1b & oper2b;      flag_log8(res8); }
static void op_and16(void) { res16 = oper1 & oper2;        flag_log16(res16); }
static void op_or8(void)   { res8  = oper1b | oper2b;      flag_log8(res8); }
static void op_or16(void)  { res16 = oper1 | oper2;        flag_log16(res16); }
static void op_xor8(void)  { res8  = oper1b ^ oper2b;      flag_log8(res8); }
static void op_xor16(void) { res16 = oper1 ^ oper2;        flag_log16(res16); }
static void op_sub8(void)  { res8  = oper1b - oper2b;      flag_sub8(oper1b, oper2b); }
static void op_sub16(void) { res16 = oper1 - oper2;        flag_sub16(oper1, oper2); }
static void op_sbb8(void)  { res8  = oper1b - (oper2b + cf); flag_sbb8(oper1b, oper2b, cf); }
static void op_sbb16(void) { res16 = oper1 - (oper2 + cf);   flag_sbb16(oper1, oper2, cf); }

/* ---- Effective address calculation ---- */

static void getea(uint8_t rmval) {
	uint32_t tempea = 0;
	switch (mode) {
		case 0:
			switch (rmval) {
				case 0: tempea = regs.wordregs[regbx] + regs.wordregs[regsi]; break;
				case 1: tempea = regs.wordregs[regbx] + regs.wordregs[regdi]; break;
				case 2: tempea = regs.wordregs[regbp] + regs.wordregs[regsi]; break;
				case 3: tempea = regs.wordregs[regbp] + regs.wordregs[regdi]; break;
				case 4: tempea = regs.wordregs[regsi]; break;
				case 5: tempea = regs.wordregs[regdi]; break;
				case 6: tempea = disp16; break;
				case 7: tempea = regs.wordregs[regbx]; break;
			}
			break;
		case 1:
		case 2:
			switch (rmval) {
				case 0: tempea = regs.wordregs[regbx] + regs.wordregs[regsi] + disp16; break;
				case 1: tempea = regs.wordregs[regbx] + regs.wordregs[regdi] + disp16; break;
				case 2: tempea = regs.wordregs[regbp] + regs.wordregs[regsi] + disp16; break;
				case 3: tempea = regs.wordregs[regbp] + regs.wordregs[regdi] + disp16; break;
				case 4: tempea = regs.wordregs[regsi] + disp16; break;
				case 5: tempea = regs.wordregs[regdi] + disp16; break;
				case 6: tempea = regs.wordregs[regbp] + disp16; break;
				case 7: tempea = regs.wordregs[regbx] + disp16; break;
			}
			break;
	}
	ea = (tempea & 0xFFFF) + (useseg << 4);
}

/* ---- Stack ---- */

static void push(uint16_t pushval) {
	regs.wordregs[regsp] = regs.wordregs[regsp] - 2;
	putmem16(segregs[regss], regs.wordregs[regsp], pushval);
}

static uint16_t pop(void) {
	uint16_t tempval = getmem16(segregs[regss], regs.wordregs[regsp]);
	regs.wordregs[regsp] = regs.wordregs[regsp] + 2;
	return tempval;
}

/* ---- CPU reset ---- */

void cpu_reset(void) {
	/* Standard x86 reset vector: CS=0xF000, IP=0xFFF0 */
	segregs[regcs] = 0xF000;
	ip = 0xFFF0;
	segregs[regds] = 0x0000;
	segregs[reges] = 0x0000;
	segregs[regss] = 0x0000;
	regs.wordregs[regsp] = 0x0000;
	hltstate = 0;
	running = 1;
	totalexec = 0;
	ifl = 0;
	tf = 0;
}

/* ---- Register read/write helpers ---- */

static uint16_t readrm16(uint8_t rmval) {
	if (mode < 3) {
		getea(rmval);
		return read86(ea) | ((uint16_t)read86(ea + 1) << 8);
	} else {
		return getreg16(rmval);
	}
}

static uint8_t readrm8(uint8_t rmval) {
	if (mode < 3) {
		getea(rmval);
		return read86(ea);
	} else {
		return getreg8(rmval);
	}
}

static void writerm16(uint8_t rmval, uint16_t value) {
	if (mode < 3) {
		getea(rmval);
		write86(ea, value & 0xFF);
		write86(ea + 1, value >> 8);
	} else {
		putreg16(rmval, value);
	}
}

static void writerm8(uint8_t rmval, uint8_t value) {
	if (mode < 3) {
		getea(rmval);
		write86(ea, value);
	} else {
		putreg8(rmval, value);
	}
}

/* ---- Group 2 (shift/rotate) ---- */

static uint8_t op_grp2_8(uint8_t cnt) {
	uint16_t s, shift, oldcf2, msb;
	s = oper1b;
	oldcf2 = cf;
#ifdef CPU_LIMIT_SHIFT_COUNT
	cnt &= 0x1F;
#endif
	switch (reg) {
		case 0: /* ROL r/m8 */
			for (shift = 1; shift <= cnt; shift++) {
				cf = (s & 0x80) ? 1 : 0;
				s = (s << 1) | cf;
			}
			if (cnt == 1) { if ((s & 0x80) && cf) of = 1; else of = 0; } else of = 0;
			break;
		case 1: /* ROR r/m8 */
			for (shift = 1; shift <= cnt; shift++) {
				cf = s & 1;
				s = (s >> 1) | (cf << 7);
			}
			if (cnt == 1) of = (s >> 7) ^ ((s >> 6) & 1);
			break;
		case 2: /* RCL r/m8 */
			for (shift = 1; shift <= cnt; shift++) {
				oldcf2 = cf;
				cf = (s & 0x80) ? 1 : 0;
				s = (s << 1) | oldcf2;
			}
			if (cnt == 1) of = cf ^ ((s >> 7) & 1);
			break;
		case 3: /* RCR r/m8 */
			for (shift = 1; shift <= cnt; shift++) {
				oldcf2 = cf;
				cf = s & 1;
				s = (s >> 1) | (oldcf2 << 7);
			}
			if (cnt == 1) of = (s >> 7) ^ ((s >> 6) & 1);
			break;
		case 4:
		case 6: /* SHL r/m8 */
			for (shift = 1; shift <= cnt; shift++) {
				cf = (s & 0x80) ? 1 : 0;
				s = (s << 1) & 0xFF;
			}
			if ((cnt == 1) && (cf == (s >> 7))) of = 0; else of = 1;
			flag_szp8((uint8_t)s);
			break;
		case 5: /* SHR r/m8 */
			if ((cnt == 1) && (s & 0x80)) of = 1; else of = 0;
			for (shift = 1; shift <= cnt; shift++) {
				cf = s & 1;
				s = s >> 1;
			}
			flag_szp8((uint8_t)s);
			break;
		case 7: /* SAR r/m8 */
			for (shift = 1; shift <= cnt; shift++) {
				msb = s & 0x80;
				cf = s & 1;
				s = (s >> 1) | msb;
			}
			of = 0;
			flag_szp8((uint8_t)s);
			break;
	}
	(void)oldcf2;
	return s & 0xFF;
}

static uint16_t op_grp2_16(uint8_t cnt) {
	uint32_t s, shift, oldcf2, msb;
	s = oper1;
	oldcf2 = cf;
#ifdef CPU_LIMIT_SHIFT_COUNT
	cnt &= 0x1F;
#endif
	switch (reg) {
		case 0: /* ROL r/m16 */
			for (shift = 1; shift <= cnt; shift++) {
				cf = (s & 0x8000) ? 1 : 0;
				s = (s << 1) | cf;
			}
			if (cnt == 1) of = cf ^ ((s >> 15) & 1);
			break;
		case 1: /* ROR r/m16 */
			for (shift = 1; shift <= cnt; shift++) {
				cf = s & 1;
				s = (s >> 1) | (cf << 15);
			}
			if (cnt == 1) of = (s >> 15) ^ ((s >> 14) & 1);
			break;
		case 2: /* RCL r/m16 */
			for (shift = 1; shift <= cnt; shift++) {
				oldcf2 = cf;
				cf = (s & 0x8000) ? 1 : 0;
				s = (s << 1) | oldcf2;
			}
			if (cnt == 1) of = cf ^ ((s >> 15) & 1);
			break;
		case 3: /* RCR r/m16 */
			for (shift = 1; shift <= cnt; shift++) {
				oldcf2 = cf;
				cf = s & 1;
				s = (s >> 1) | (oldcf2 << 15);
			}
			if (cnt == 1) of = (s >> 15) ^ ((s >> 14) & 1);
			break;
		case 4:
		case 6: /* SHL r/m16 */
			for (shift = 1; shift <= cnt; shift++) {
				cf = (s & 0x8000) ? 1 : 0;
				s = (s << 1) & 0xFFFF;
			}
			if ((cnt == 1) && (cf == (s >> 15))) of = 0; else of = 1;
			flag_szp16((uint16_t)s);
			break;
		case 5: /* SHR r/m16 */
			if ((cnt == 1) && (s & 0x8000)) of = 1; else of = 0;
			for (shift = 1; shift <= cnt; shift++) {
				cf = s & 1;
				s = s >> 1;
			}
			flag_szp16((uint16_t)s);
			break;
		case 7: /* SAR r/m16 */
			for (shift = 1; shift <= cnt; shift++) {
				msb = s & 0x8000;
				cf = s & 1;
				s = (s >> 1) | msb;
			}
			of = 0;
			flag_szp16((uint16_t)s);
			break;
	}
	(void)oldcf2;
	return (uint16_t)s & 0xFFFF;
}

/* ---- Division ---- */

static void op_div8(uint16_t valdiv, uint8_t divisor) {
	if (divisor == 0) { intcall86(0); return; }
	if ((valdiv / (uint16_t)divisor) > 0xFF) { intcall86(0); return; }
	regs.byteregs[regah] = valdiv % (uint16_t)divisor;
	regs.byteregs[regal] = valdiv / (uint16_t)divisor;
}

static void op_idiv8(uint16_t valdiv, uint8_t divisor) {
	uint16_t s1, s2, d1, d2;
	int sign;
	if (divisor == 0) { intcall86(0); return; }
	s1 = valdiv;
	s2 = divisor;
	sign = (((s1 ^ s2) & 0x8000) != 0);
	s1 = (s1 < 0x8000) ? s1 : ((~s1 + 1) & 0xffff);
	s2 = (s2 < 0x8000) ? s2 : ((~s2 + 1) & 0xffff);
	d1 = s1 / s2;
	d2 = s1 % s2;
	if (d1 & 0xFF00) { intcall86(0); return; }
	if (sign) { d1 = (~d1 + 1) & 0xff; d2 = (~d2 + 1) & 0xff; }
	regs.byteregs[regah] = (uint8_t)d2;
	regs.byteregs[regal] = (uint8_t)d1;
}

static void op_div16(uint32_t valdiv, uint16_t divisor) {
	if (divisor == 0) { intcall86(0); return; }
	if ((valdiv / (uint32_t)divisor) > 0xFFFF) { intcall86(0); return; }
	regs.wordregs[regdx] = valdiv % (uint32_t)divisor;
	regs.wordregs[regax] = valdiv / (uint32_t)divisor;
}

static void op_idiv16(uint32_t valdiv, uint16_t divisor) {
	uint32_t d1, d2, s1, s2;
	int sign;
	if (divisor == 0) { intcall86(0); return; }
	s1 = valdiv;
	s2 = divisor;
	s2 = (s2 & 0x8000) ? (s2 | 0xffff0000) : s2;
	sign = (((s1 ^ s2) & 0x80000000) != 0);
	s1 = (s1 < 0x80000000) ? s1 : ((~s1 + 1) & 0xffffffff);
	s2 = (s2 < 0x80000000) ? s2 : ((~s2 + 1) & 0xffffffff);
	d1 = s1 / s2;
	d2 = s1 % s2;
	if (d1 & 0xFFFF0000) { intcall86(0); return; }
	if (sign) { d1 = (~d1 + 1) & 0xffff; d2 = (~d2 + 1) & 0xffff; }
	regs.wordregs[regax] = d1;
	regs.wordregs[regdx] = d2;
}

/* ---- Group 3 (TEST/NOT/NEG/MUL/IMUL/DIV/IDIV) ---- */

static void op_grp3_8(void) {
	oper1 = signext(oper1b);
	oper2 = signext(oper2b);
	switch (reg) {
		case 0: case 1: /* TEST */
			flag_log8(oper1b & getmem8(segregs[regcs], ip));
			StepIP(1);
			break;
		case 2: /* NOT */
			res8 = ~oper1b;
			break;
		case 3: /* NEG */
			res8 = (~oper1b) + 1;
			flag_sub8(0, oper1b);
			cf = res8 ? 1 : 0;
			break;
		case 4: /* MUL */
			temp1 = (uint32_t)oper1b * (uint32_t)regs.byteregs[regal];
			regs.wordregs[regax] = temp1 & 0xFFFF;
			flag_szp8((uint8_t)temp1);
			if (regs.byteregs[regah]) { cf = 1; of = 1; } else { cf = 0; of = 0; }
#ifdef CPU_CLEAR_ZF_ON_MUL
			zf = 0;
#endif
			break;
		case 5: /* IMUL */
			oper1 = signext(oper1b);
			temp1 = signext(regs.byteregs[regal]);
			temp2 = oper1;
			if ((temp1 & 0x80) == 0x80) temp1 |= 0xFFFFFF00;
			if ((temp2 & 0x80) == 0x80) temp2 |= 0xFFFFFF00;
			temp3 = (temp1 * temp2) & 0xFFFF;
			regs.wordregs[regax] = temp3 & 0xFFFF;
			if (regs.byteregs[regah]) { cf = 1; of = 1; } else { cf = 0; of = 0; }
#ifdef CPU_CLEAR_ZF_ON_MUL
			zf = 0;
#endif
			break;
		case 6: op_div8(regs.wordregs[regax], oper1b); break;
		case 7: op_idiv8(regs.wordregs[regax], oper1b); break;
	}
}

static void op_grp3_16(void) {
	switch (reg) {
		case 0: case 1: /* TEST */
			flag_log16(oper1 & getmem16(segregs[regcs], ip));
			StepIP(2);
			break;
		case 2: res16 = ~oper1; break;
		case 3:
			res16 = (~oper1) + 1;
			flag_sub16(0, oper1);
			cf = res16 ? 1 : 0;
			break;
		case 4: /* MUL */
			temp1 = (uint32_t)oper1 * (uint32_t)regs.wordregs[regax];
			regs.wordregs[regax] = temp1 & 0xFFFF;
			regs.wordregs[regdx] = temp1 >> 16;
			flag_szp16((uint16_t)temp1);
			if (regs.wordregs[regdx]) { cf = 1; of = 1; } else { cf = 0; of = 0; }
#ifdef CPU_CLEAR_ZF_ON_MUL
			zf = 0;
#endif
			break;
		case 5: /* IMUL */
			temp1 = regs.wordregs[regax];
			temp2 = oper1;
			if (temp1 & 0x8000) temp1 |= 0xFFFF0000;
			if (temp2 & 0x8000) temp2 |= 0xFFFF0000;
			temp3 = temp1 * temp2;
			regs.wordregs[regax] = temp3 & 0xFFFF;
			regs.wordregs[regdx] = temp3 >> 16;
			if (regs.wordregs[regdx]) { cf = 1; of = 1; } else { cf = 0; of = 0; }
#ifdef CPU_CLEAR_ZF_ON_MUL
			zf = 0;
#endif
			break;
		case 6: op_div16(((uint32_t)regs.wordregs[regdx] << 16) + regs.wordregs[regax], oper1); break;
		case 7: op_idiv16(((uint32_t)regs.wordregs[regdx] << 16) + regs.wordregs[regax], oper1); break;
	}
}

/* ---- Group 5 (INC/DEC/CALL/JMP/PUSH) ---- */

static void op_grp5(void) {
	switch (reg) {
		case 0: /* INC Ev */
			oper2 = 1; tempcf = cf; op_add16(); cf = tempcf;
			writerm16(rm, res16);
			break;
		case 1: /* DEC Ev */
			oper2 = 1; tempcf = cf; op_sub16(); cf = tempcf;
			writerm16(rm, res16);
			break;
		case 2: /* CALL Ev */
			push(ip); ip = oper1;
			break;
		case 3: /* CALL Mp */
			push(segregs[regcs]); push(ip);
			getea(rm);
			ip = (uint16_t)read86(ea) + (uint16_t)read86(ea + 1) * 256;
			segregs[regcs] = (uint16_t)read86(ea + 2) + (uint16_t)read86(ea + 3) * 256;
			break;
		case 4: /* JMP Ev */
			ip = oper1;
			break;
		case 5: /* JMP Mp */
			getea(rm);
			ip = (uint16_t)read86(ea) + (uint16_t)read86(ea + 1) * 256;
			segregs[regcs] = (uint16_t)read86(ea + 2) + (uint16_t)read86(ea + 3) * 256;
			break;
		case 6: /* PUSH Ev */
			push(oper1);
			break;
	}
}

/* ---- Interrupt call ---- */

void intcall86(uint8_t intnum) {
	if (intnum == 0x19) didbootstrap = 1;

	switch (intnum) {
		case 0x10:  /* BIOS video services — handled in C */
			vidinterrupt();
			return;

		case 0x13:  /* BIOS disk services — handled in C */
		case 0xFD:
			diskhandler();
			return;

		case 0x16:  /* BIOS keyboard services — handled in C */
			bios_int16h();
			return;

		case 0x1A:  /* BIOS time services — handled in C */
			bios_int1ah();
			return;

		case 0x19:  /* Bootstrap loader */
			disk_bootstrap();
			return;

		default:
			break;
	}

	push(makeflagsword());
	push(segregs[regcs]);
	push(ip);
	segregs[regcs] = getmem16(0, (uint16_t)intnum * 4 + 2);
	ip = getmem16(0, (uint16_t)intnum * 4);
	ifl = 0;
	tf = 0;
}

/* ---- Main execution loop ---- */

void exec86(uint32_t execloops) {
	uint32_t loopcount;
	uint8_t docontinue;
	static uint16_t firstip;
	static uint16_t trap_toggle = 0;

	for (loopcount = 0; loopcount < execloops; loopcount++) {

		if (trap_toggle) {
			intcall86(1);
		}

		if (tf) {
			trap_toggle = 1;
		} else {
			trap_toggle = 0;
		}

		if (hltstate) goto skipexecution;

		reptype = 0;
		segoverride = 0;
		useseg = segregs[regds];
		docontinue = 0;
		firstip = ip;

		while (!docontinue) {
			segregs[regcs] = segregs[regcs] & 0xFFFF;
			ip = ip & 0xFFFF;
			savecs = segregs[regcs];
			saveip = ip;
			opcode = getmem8(segregs[regcs], ip);
			StepIP(1);

			switch (opcode) {
				/* segment prefix check */
				case 0x2E: useseg = segregs[regcs]; segoverride = 1; break;
				case 0x3E: useseg = segregs[regds]; segoverride = 1; break;
				case 0x26: useseg = segregs[reges]; segoverride = 1; break;
				case 0x36: useseg = segregs[regss]; segoverride = 1; break;
				/* repetition prefix check */
				case 0xF3: reptype = 1; break;
				case 0xF2: reptype = 2; break;
				default: docontinue = 1; break;
			}
		}

		totalexec++;

		switch (opcode) {
			case 0x0: /* 00 ADD Eb Gb */
				modregrm(); oper1b = readrm8(rm); oper2b = getreg8(reg);
				op_add8(); writerm8(rm, res8); break;
			case 0x1: /* 01 ADD Ev Gv */
				modregrm(); oper1 = readrm16(rm); oper2 = getreg16(reg);
				op_add16(); writerm16(rm, res16); break;
			case 0x2: /* 02 ADD Gb Eb */
				modregrm(); oper1b = getreg8(reg); oper2b = readrm8(rm);
				op_add8(); putreg8(reg, res8); break;
			case 0x3: /* 03 ADD Gv Ev */
				modregrm(); oper1 = getreg16(reg); oper2 = readrm16(rm);
				op_add16(); putreg16(reg, res16); break;
			case 0x4: /* 04 ADD AL Ib */
				oper1b = regs.byteregs[regal]; oper2b = getmem8(segregs[regcs], ip);
				StepIP(1); op_add8(); regs.byteregs[regal] = res8; break;
			case 0x5: /* 05 ADD AX Iv */
				oper1 = regs.wordregs[regax]; oper2 = getmem16(segregs[regcs], ip);
				StepIP(2); op_add16(); regs.wordregs[regax] = res16; break;
			case 0x6: /* 06 PUSH ES */ push(segregs[reges]); break;
			case 0x7: /* 07 POP ES */ segregs[reges] = pop(); break;
			case 0x8: /* 08 OR Eb Gb */
				modregrm(); oper1b = readrm8(rm); oper2b = getreg8(reg);
				op_or8(); writerm8(rm, res8); break;
			case 0x9: /* 09 OR Ev Gv */
				modregrm(); oper1 = readrm16(rm); oper2 = getreg16(reg);
				op_or16(); writerm16(rm, res16); break;
			case 0xA: /* 0A OR Gb Eb */
				modregrm(); oper1b = getreg8(reg); oper2b = readrm8(rm);
				op_or8(); putreg8(reg, res8); break;
			case 0xB: /* 0B OR Gv Ev */
				modregrm(); oper1 = getreg16(reg); oper2 = readrm16(rm);
				op_or16(); putreg16(reg, res16); break;
			case 0xC: /* 0C OR AL Ib */
				oper1b = regs.byteregs[regal]; oper2b = getmem8(segregs[regcs], ip);
				StepIP(1); op_or8(); regs.byteregs[regal] = res8; break;
			case 0xD: /* 0D OR AX Iv */
				oper1 = regs.wordregs[regax]; oper2 = getmem16(segregs[regcs], ip);
				StepIP(2); op_or16(); regs.wordregs[regax] = res16; break;
			case 0xE: /* 0E PUSH CS */ push(segregs[regcs]); break;
#ifdef CPU_ALLOW_POP_CS
			case 0xF: /* 0F POP CS (8086 only) */ segregs[regcs] = pop(); break;
#endif
			case 0x10: /* 10 ADC Eb Gb */
				modregrm(); oper1b = readrm8(rm); oper2b = getreg8(reg);
				op_adc8(); writerm8(rm, res8); break;
			case 0x11: /* 11 ADC Ev Gv */
				modregrm(); oper1 = readrm16(rm); oper2 = getreg16(reg);
				op_adc16(); writerm16(rm, res16); break;
			case 0x12: /* 12 ADC Gb Eb */
				modregrm(); oper1b = getreg8(reg); oper2b = readrm8(rm);
				op_adc8(); putreg8(reg, res8); break;
			case 0x13: /* 13 ADC Gv Ev */
				modregrm(); oper1 = getreg16(reg); oper2 = readrm16(rm);
				op_adc16(); putreg16(reg, res16); break;
			case 0x14: /* 14 ADC AL Ib */
				oper1b = regs.byteregs[regal]; oper2b = getmem8(segregs[regcs], ip);
				StepIP(1); op_adc8(); regs.byteregs[regal] = res8; break;
			case 0x15: /* 15 ADC AX Iv */
				oper1 = regs.wordregs[regax]; oper2 = getmem16(segregs[regcs], ip);
				StepIP(2); op_adc16(); regs.wordregs[regax] = res16; break;
			case 0x16: /* 16 PUSH SS */ push(segregs[regss]); break;
			case 0x17: /* 17 POP SS */ segregs[regss] = pop(); break;
			case 0x18: /* 18 SBB Eb Gb */
				modregrm(); oper1b = readrm8(rm); oper2b = getreg8(reg);
				op_sbb8(); writerm8(rm, res8); break;
			case 0x19: /* 19 SBB Ev Gv */
				modregrm(); oper1 = readrm16(rm); oper2 = getreg16(reg);
				op_sbb16(); writerm16(rm, res16); break;
			case 0x1A: /* 1A SBB Gb Eb */
				modregrm(); oper1b = getreg8(reg); oper2b = readrm8(rm);
				op_sbb8(); putreg8(reg, res8); break;
			case 0x1B: /* 1B SBB Gv Ev */
				modregrm(); oper1 = getreg16(reg); oper2 = readrm16(rm);
				op_sbb16(); putreg16(reg, res16); break;
			case 0x1C: /* 1C SBB AL Ib */
				oper1b = regs.byteregs[regal]; oper2b = getmem8(segregs[regcs], ip);
				StepIP(1); op_sbb8(); regs.byteregs[regal] = res8; break;
			case 0x1D: /* 1D SBB AX Iv */
				oper1 = regs.wordregs[regax]; oper2 = getmem16(segregs[regcs], ip);
				StepIP(2); op_sbb16(); regs.wordregs[regax] = res16; break;
			case 0x1E: /* 1E PUSH DS */ push(segregs[regds]); break;
			case 0x1F: /* 1F POP DS */ segregs[regds] = pop(); break;
			case 0x20: /* 20 AND Eb Gb */
				modregrm(); oper1b = readrm8(rm); oper2b = getreg8(reg);
				op_and8(); writerm8(rm, res8); break;
			case 0x21: /* 21 AND Ev Gv */
				modregrm(); oper1 = readrm16(rm); oper2 = getreg16(reg);
				op_and16(); writerm16(rm, res16); break;
			case 0x22: /* 22 AND Gb Eb */
				modregrm(); oper1b = getreg8(reg); oper2b = readrm8(rm);
				op_and8(); putreg8(reg, res8); break;
			case 0x23: /* 23 AND Gv Ev */
				modregrm(); oper1 = getreg16(reg); oper2 = readrm16(rm);
				op_and16(); putreg16(reg, res16); break;
			case 0x24: /* 24 AND AL Ib */
				oper1b = regs.byteregs[regal]; oper2b = getmem8(segregs[regcs], ip);
				StepIP(1); op_and8(); regs.byteregs[regal] = res8; break;
			case 0x25: /* 25 AND AX Iv */
				oper1 = regs.wordregs[regax]; oper2 = getmem16(segregs[regcs], ip);
				StepIP(2); op_and16(); regs.wordregs[regax] = res16; break;
			case 0x27: /* 27 DAA */
				if (((regs.byteregs[regal] & 0xF) > 9) || (af == 1)) {
					oper1 = regs.byteregs[regal] + 6;
					regs.byteregs[regal] = oper1 & 255;
					if (oper1 & 0xFF00) cf = 1;
					af = 1;
				}
				if ((regs.byteregs[regal] > 0x9F) || (cf == 1)) {
					regs.byteregs[regal] = regs.byteregs[regal] + 0x60;
					cf = 1;
				}
				regs.byteregs[regal] = regs.byteregs[regal] & 255;
				flag_szp8(regs.byteregs[regal]);
				break;
			case 0x28: /* 28 SUB Eb Gb */
				modregrm(); oper1b = readrm8(rm); oper2b = getreg8(reg);
				op_sub8(); writerm8(rm, res8); break;
			case 0x29: /* 29 SUB Ev Gv */
				modregrm(); oper1 = readrm16(rm); oper2 = getreg16(reg);
				op_sub16(); writerm16(rm, res16); break;
			case 0x2A: /* 2A SUB Gb Eb */
				modregrm(); oper1b = getreg8(reg); oper2b = readrm8(rm);
				op_sub8(); putreg8(reg, res8); break;
			case 0x2B: /* 2B SUB Gv Ev */
				modregrm(); oper1 = getreg16(reg); oper2 = readrm16(rm);
				op_sub16(); putreg16(reg, res16); break;
			case 0x2C: /* 2C SUB AL Ib */
				oper1b = regs.byteregs[regal]; oper2b = getmem8(segregs[regcs], ip);
				StepIP(1); op_sub8(); regs.byteregs[regal] = res8; break;
			case 0x2D: /* 2D SUB AX Iv */
				oper1 = regs.wordregs[regax]; oper2 = getmem16(segregs[regcs], ip);
				StepIP(2); op_sub16(); regs.wordregs[regax] = res16; break;
			case 0x2F: /* 2F DAS */
				if (((regs.byteregs[regal] & 15) > 9) || (af == 1)) {
					oper1 = regs.byteregs[regal] - 6;
					regs.byteregs[regal] = oper1 & 255;
					if (oper1 & 0xFF00) cf = 1;
					af = 1;
				} else { af = 0; }
				if (((regs.byteregs[regal] & 0xF0) > 0x90) || (cf == 1)) {
					regs.byteregs[regal] = regs.byteregs[regal] - 0x60;
					cf = 1;
				} else { cf = 0; }
				flag_szp8(regs.byteregs[regal]);
				break;
			case 0x30: /* 30 XOR Eb Gb */
				modregrm(); oper1b = readrm8(rm); oper2b = getreg8(reg);
				op_xor8(); writerm8(rm, res8); break;
			case 0x31: /* 31 XOR Ev Gv */
				modregrm(); oper1 = readrm16(rm); oper2 = getreg16(reg);
				op_xor16(); writerm16(rm, res16); break;
			case 0x32: /* 32 XOR Gb Eb */
				modregrm(); oper1b = getreg8(reg); oper2b = readrm8(rm);
				op_xor8(); putreg8(reg, res8); break;
			case 0x33: /* 33 XOR Gv Ev */
				modregrm(); oper1 = getreg16(reg); oper2 = readrm16(rm);
				op_xor16(); putreg16(reg, res16); break;
			case 0x34: /* 34 XOR AL Ib */
				oper1b = regs.byteregs[regal]; oper2b = getmem8(segregs[regcs], ip);
				StepIP(1); op_xor8(); regs.byteregs[regal] = res8; break;
			case 0x35: /* 35 XOR AX Iv */
				oper1 = regs.wordregs[regax]; oper2 = getmem16(segregs[regcs], ip);
				StepIP(2); op_xor16(); regs.wordregs[regax] = res16; break;
			case 0x37: /* 37 AAA */
				if (((regs.byteregs[regal] & 0xF) > 9) || (af == 1)) {
					regs.byteregs[regal] = regs.byteregs[regal] + 6;
					regs.byteregs[regah] = regs.byteregs[regah] + 1;
					af = 1; cf = 1;
				} else { af = 0; cf = 0; }
				regs.byteregs[regal] = regs.byteregs[regal] & 0xF;
				break;
			case 0x38: /* 38 CMP Eb Gb */
				modregrm(); oper1b = readrm8(rm); oper2b = getreg8(reg);
				flag_sub8(oper1b, oper2b); break;
			case 0x39: /* 39 CMP Ev Gv */
				modregrm(); oper1 = readrm16(rm); oper2 = getreg16(reg);
				flag_sub16(oper1, oper2); break;
			case 0x3A: /* 3A CMP Gb Eb */
				modregrm(); oper1b = getreg8(reg); oper2b = readrm8(rm);
				flag_sub8(oper1b, oper2b); break;
			case 0x3B: /* 3B CMP Gv Ev */
				modregrm(); oper1 = getreg16(reg); oper2 = readrm16(rm);
				flag_sub16(oper1, oper2); break;
			case 0x3C: /* 3C CMP AL Ib */
				oper1b = regs.byteregs[regal]; oper2b = getmem8(segregs[regcs], ip);
				StepIP(1); flag_sub8(oper1b, oper2b); break;
			case 0x3D: /* 3D CMP AX Iv */
				oper1 = regs.wordregs[regax]; oper2 = getmem16(segregs[regcs], ip);
				StepIP(2); flag_sub16(oper1, oper2); break;
			case 0x3F: /* 3F AAS */
				if (((regs.byteregs[regal] & 0xF) > 9) || (af == 1)) {
					regs.byteregs[regal] = regs.byteregs[regal] - 6;
					regs.byteregs[regah] = regs.byteregs[regah] - 1;
					af = 1; cf = 1;
				} else { af = 0; cf = 0; }
				regs.byteregs[regal] = regs.byteregs[regal] & 0xF;
				break;

			/* 40-47 INC */
			case 0x40: oldcf = cf; oper1 = regs.wordregs[regax]; oper2 = 1; op_add16(); cf = oldcf; regs.wordregs[regax] = res16; break;
			case 0x41: oldcf = cf; oper1 = regs.wordregs[regcx]; oper2 = 1; op_add16(); cf = oldcf; regs.wordregs[regcx] = res16; break;
			case 0x42: oldcf = cf; oper1 = regs.wordregs[regdx]; oper2 = 1; op_add16(); cf = oldcf; regs.wordregs[regdx] = res16; break;
			case 0x43: oldcf = cf; oper1 = regs.wordregs[regbx]; oper2 = 1; op_add16(); cf = oldcf; regs.wordregs[regbx] = res16; break;
			case 0x44: oldcf = cf; oper1 = regs.wordregs[regsp]; oper2 = 1; op_add16(); cf = oldcf; regs.wordregs[regsp] = res16; break;
			case 0x45: oldcf = cf; oper1 = regs.wordregs[regbp]; oper2 = 1; op_add16(); cf = oldcf; regs.wordregs[regbp] = res16; break;
			case 0x46: oldcf = cf; oper1 = regs.wordregs[regsi]; oper2 = 1; op_add16(); cf = oldcf; regs.wordregs[regsi] = res16; break;
			case 0x47: oldcf = cf; oper1 = regs.wordregs[regdi]; oper2 = 1; op_add16(); cf = oldcf; regs.wordregs[regdi] = res16; break;

			/* 48-4F DEC */
			case 0x48: oldcf = cf; oper1 = regs.wordregs[regax]; oper2 = 1; op_sub16(); cf = oldcf; regs.wordregs[regax] = res16; break;
			case 0x49: oldcf = cf; oper1 = regs.wordregs[regcx]; oper2 = 1; op_sub16(); cf = oldcf; regs.wordregs[regcx] = res16; break;
			case 0x4A: oldcf = cf; oper1 = regs.wordregs[regdx]; oper2 = 1; op_sub16(); cf = oldcf; regs.wordregs[regdx] = res16; break;
			case 0x4B: oldcf = cf; oper1 = regs.wordregs[regbx]; oper2 = 1; op_sub16(); cf = oldcf; regs.wordregs[regbx] = res16; break;
			case 0x4C: oldcf = cf; oper1 = regs.wordregs[regsp]; oper2 = 1; op_sub16(); cf = oldcf; regs.wordregs[regsp] = res16; break;
			case 0x4D: oldcf = cf; oper1 = regs.wordregs[regbp]; oper2 = 1; op_sub16(); cf = oldcf; regs.wordregs[regbp] = res16; break;
			case 0x4E: oldcf = cf; oper1 = regs.wordregs[regsi]; oper2 = 1; op_sub16(); cf = oldcf; regs.wordregs[regsi] = res16; break;
			case 0x4F: oldcf = cf; oper1 = regs.wordregs[regdi]; oper2 = 1; op_sub16(); cf = oldcf; regs.wordregs[regdi] = res16; break;

			/* 50-57 PUSH */
			case 0x50: push(regs.wordregs[regax]); break;
			case 0x51: push(regs.wordregs[regcx]); break;
			case 0x52: push(regs.wordregs[regdx]); break;
			case 0x53: push(regs.wordregs[regbx]); break;
			case 0x54: push(regs.wordregs[regsp] - 2); break; /* 8086 style */
			case 0x55: push(regs.wordregs[regbp]); break;
			case 0x56: push(regs.wordregs[regsi]); break;
			case 0x57: push(regs.wordregs[regdi]); break;

			/* 58-5F POP */
			case 0x58: regs.wordregs[regax] = pop(); break;
			case 0x59: regs.wordregs[regcx] = pop(); break;
			case 0x5A: regs.wordregs[regdx] = pop(); break;
			case 0x5B: regs.wordregs[regbx] = pop(); break;
			case 0x5C: regs.wordregs[regsp] = pop(); break;
			case 0x5D: regs.wordregs[regbp] = pop(); break;
			case 0x5E: regs.wordregs[regsi] = pop(); break;
			case 0x5F: regs.wordregs[regdi] = pop(); break;

#ifndef CPU_8086
			case 0x60: /* 60 PUSHA (80186+) */
				oldsp = regs.wordregs[regsp];
				push(regs.wordregs[regax]); push(regs.wordregs[regcx]);
				push(regs.wordregs[regdx]); push(regs.wordregs[regbx]);
				push(oldsp); push(regs.wordregs[regbp]);
				push(regs.wordregs[regsi]); push(regs.wordregs[regdi]);
				break;
			case 0x61: /* 61 POPA (80186+) */
				regs.wordregs[regdi] = pop(); regs.wordregs[regsi] = pop();
				regs.wordregs[regbp] = pop(); dummy = pop();
				regs.wordregs[regbx] = pop(); regs.wordregs[regdx] = pop();
				regs.wordregs[regcx] = pop(); regs.wordregs[regax] = pop();
				break;
			case 0x62: /* 62 BOUND Gv, Ev (80186+) */
				modregrm(); getea(rm);
				if (signext32(getreg16(reg)) < signext32(getmem16(ea >> 4, ea & 15))) {
					intcall86(5);
				} else {
					ea += 2;
					if (signext32(getreg16(reg)) > signext32(getmem16(ea >> 4, ea & 15))) {
						intcall86(5);
					}
				}
				break;
			case 0x68: /* 68 PUSH Iv (80186+) */
				push(getmem16(segregs[regcs], ip)); StepIP(2); break;
			case 0x69: /* 69 IMUL Gv Ev Iv (80186+) */
				modregrm(); temp1 = readrm16(rm);
				temp2 = getmem16(segregs[regcs], ip); StepIP(2);
				if ((temp1 & 0x8000L) == 0x8000L) temp1 |= 0xFFFF0000L;
				if ((temp2 & 0x8000L) == 0x8000L) temp2 |= 0xFFFF0000L;
				temp3 = temp1 * temp2;
				putreg16(reg, temp3 & 0xFFFFL);
				if (temp3 & 0xFFFF0000L) { cf = 1; of = 1; } else { cf = 0; of = 0; }
				break;
			case 0x6A: /* 6A PUSH Ib (80186+) */
				push(getmem8(segregs[regcs], ip)); StepIP(1); break;
			case 0x6B: /* 6B IMUL Gv Eb Ib (80186+) */
				modregrm(); temp1 = readrm16(rm);
				temp2 = signext(getmem8(segregs[regcs], ip)); StepIP(1);
				if ((temp1 & 0x8000L) == 0x8000L) temp1 |= 0xFFFF0000L;
				if ((temp2 & 0x8000L) == 0x8000L) temp2 |= 0xFFFF0000L;
				temp3 = temp1 * temp2;
				putreg16(reg, temp3 & 0xFFFFL);
				if (temp3 & 0xFFFF0000L) { cf = 1; of = 1; } else { cf = 0; of = 0; }
				break;
			case 0x6C: /* 6C INSB */
				if (reptype && (regs.wordregs[regcx] == 0)) break;
				putmem8(useseg, regs.wordregs[regsi], portin(regs.wordregs[regdx]));
				if (df) { regs.wordregs[regsi]--; regs.wordregs[regdi]--; }
				else { regs.wordregs[regsi]++; regs.wordregs[regdi]++; }
				if (reptype) regs.wordregs[regcx]--;
				totalexec++; loopcount++;
				if (!reptype) break;
				ip = firstip; break;
			case 0x6D: /* 6D INSW */
				if (reptype && (regs.wordregs[regcx] == 0)) break;
				putmem16(useseg, regs.wordregs[regsi], portin16(regs.wordregs[regdx]));
				if (df) { regs.wordregs[regsi] -= 2; regs.wordregs[regdi] -= 2; }
				else { regs.wordregs[regsi] += 2; regs.wordregs[regdi] += 2; }
				if (reptype) regs.wordregs[regcx]--;
				totalexec++; loopcount++;
				if (!reptype) break;
				ip = firstip; break;
			case 0x6E: /* 6E OUTSB */
				if (reptype && (regs.wordregs[regcx] == 0)) break;
				portout(regs.wordregs[regdx], getmem8(useseg, regs.wordregs[regsi]));
				if (df) { regs.wordregs[regsi]--; regs.wordregs[regdi]--; }
				else { regs.wordregs[regsi]++; regs.wordregs[regdi]++; }
				if (reptype) regs.wordregs[regcx]--;
				totalexec++; loopcount++;
				if (!reptype) break;
				ip = firstip; break;
			case 0x6F: /* 6F OUTSW */
				if (reptype && (regs.wordregs[regcx] == 0)) break;
				portout16(regs.wordregs[regdx], getmem16(useseg, regs.wordregs[regsi]));
				if (df) { regs.wordregs[regsi] -= 2; regs.wordregs[regdi] -= 2; }
				else { regs.wordregs[regsi] += 2; regs.wordregs[regdi] += 2; }
				if (reptype) regs.wordregs[regcx]--;
				totalexec++; loopcount++;
				if (!reptype) break;
				ip = firstip; break;
#endif

			/* 70-7F conditional jumps */
			case 0x70: temp16 = signext(getmem8(segregs[regcs], ip)); StepIP(1); if (of) ip = ip + temp16; break;
			case 0x71: temp16 = signext(getmem8(segregs[regcs], ip)); StepIP(1); if (!of) ip = ip + temp16; break;
			case 0x72: temp16 = signext(getmem8(segregs[regcs], ip)); StepIP(1); if (cf) ip = ip + temp16; break;
			case 0x73: temp16 = signext(getmem8(segregs[regcs], ip)); StepIP(1); if (!cf) ip = ip + temp16; break;
			case 0x74: temp16 = signext(getmem8(segregs[regcs], ip)); StepIP(1); if (zf) ip = ip + temp16; break;
			case 0x75: temp16 = signext(getmem8(segregs[regcs], ip)); StepIP(1); if (!zf) ip = ip + temp16; break;
			case 0x76: temp16 = signext(getmem8(segregs[regcs], ip)); StepIP(1); if (cf || zf) ip = ip + temp16; break;
			case 0x77: temp16 = signext(getmem8(segregs[regcs], ip)); StepIP(1); if (!cf && !zf) ip = ip + temp16; break;
			case 0x78: temp16 = signext(getmem8(segregs[regcs], ip)); StepIP(1); if (sf) ip = ip + temp16; break;
			case 0x79: temp16 = signext(getmem8(segregs[regcs], ip)); StepIP(1); if (!sf) ip = ip + temp16; break;
			case 0x7A: temp16 = signext(getmem8(segregs[regcs], ip)); StepIP(1); if (pf) ip = ip + temp16; break;
			case 0x7B: temp16 = signext(getmem8(segregs[regcs], ip)); StepIP(1); if (!pf) ip = ip + temp16; break;
			case 0x7C: temp16 = signext(getmem8(segregs[regcs], ip)); StepIP(1); if (sf != of) ip = ip + temp16; break;
			case 0x7D: temp16 = signext(getmem8(segregs[regcs], ip)); StepIP(1); if (sf == of) ip = ip + temp16; break;
			case 0x7E: temp16 = signext(getmem8(segregs[regcs], ip)); StepIP(1); if ((sf != of) || zf) ip = ip + temp16; break;
			case 0x7F: temp16 = signext(getmem8(segregs[regcs], ip)); StepIP(1); if (!zf && (sf == of)) ip = ip + temp16; break;

			case 0x80:
			case 0x82: /* 80/82 GRP1 Eb Ib */
				modregrm(); oper1b = readrm8(rm);
				oper2b = getmem8(segregs[regcs], ip); StepIP(1);
				switch (reg) {
					case 0: op_add8(); break; case 1: op_or8(); break;
					case 2: op_adc8(); break; case 3: op_sbb8(); break;
					case 4: op_and8(); break; case 5: op_sub8(); break;
					case 6: op_xor8(); break;
					case 7: flag_sub8(oper1b, oper2b); break;
					default: break;
				}
				if (reg < 7) writerm8(rm, res8);
				break;

			case 0x81: /* 81 GRP1 Ev Iv */
			case 0x83: /* 83 GRP1 Ev Ib */
				modregrm(); oper1 = readrm16(rm);
				if (opcode == 0x81) { oper2 = getmem16(segregs[regcs], ip); StepIP(2); }
				else { oper2 = signext(getmem8(segregs[regcs], ip)); StepIP(1); }
				switch (reg) {
					case 0: op_add16(); break; case 1: op_or16(); break;
					case 2: op_adc16(); break; case 3: op_sbb16(); break;
					case 4: op_and16(); break; case 5: op_sub16(); break;
					case 6: op_xor16(); break;
					case 7: flag_sub16(oper1, oper2); break;
					default: break;
				}
				if (reg < 7) writerm16(rm, res16);
				break;

			case 0x84: /* 84 TEST Gb Eb */
				modregrm(); oper1b = getreg8(reg); oper2b = readrm8(rm);
				flag_log8(oper1b & oper2b); break;
			case 0x85: /* 85 TEST Gv Ev */
				modregrm(); oper1 = getreg16(reg); oper2 = readrm16(rm);
				flag_log16(oper1 & oper2); break;
			case 0x86: /* 86 XCHG Gb Eb */
				modregrm(); oper1b = getreg8(reg);
				putreg8(reg, readrm8(rm)); writerm8(rm, oper1b); break;
			case 0x87: /* 87 XCHG Gv Ev */
				modregrm(); oper1 = getreg16(reg);
				putreg16(reg, readrm16(rm)); writerm16(rm, oper1); break;
			case 0x88: /* 88 MOV Eb Gb */
				modregrm(); writerm8(rm, getreg8(reg)); break;
			case 0x89: /* 89 MOV Ev Gv */
				modregrm(); writerm16(rm, getreg16(reg)); break;
			case 0x8A: /* 8A MOV Gb Eb */
				modregrm(); putreg8(reg, readrm8(rm)); break;
			case 0x8B: /* 8B MOV Gv Ev */
				modregrm(); putreg16(reg, readrm16(rm)); break;
			case 0x8C: /* 8C MOV Ew Sw */
				modregrm(); writerm16(rm, getsegreg(reg)); break;
			case 0x8D: /* 8D LEA Gv M */
				modregrm(); getea(rm);
				putreg16(reg, ea - segbase(useseg)); break;
			case 0x8E: /* 8E MOV Sw Ew */
				modregrm(); putsegreg(reg, readrm16(rm)); break;
			case 0x8F: /* 8F POP Ev */
				modregrm(); writerm16(rm, pop()); break;

			case 0x90: break; /* NOP */
			case 0x91: oper1 = regs.wordregs[regcx]; regs.wordregs[regcx] = regs.wordregs[regax]; regs.wordregs[regax] = oper1; break;
			case 0x92: oper1 = regs.wordregs[regdx]; regs.wordregs[regdx] = regs.wordregs[regax]; regs.wordregs[regax] = oper1; break;
			case 0x93: oper1 = regs.wordregs[regbx]; regs.wordregs[regbx] = regs.wordregs[regax]; regs.wordregs[regax] = oper1; break;
			case 0x94: oper1 = regs.wordregs[regsp]; regs.wordregs[regsp] = regs.wordregs[regax]; regs.wordregs[regax] = oper1; break;
			case 0x95: oper1 = regs.wordregs[regbp]; regs.wordregs[regbp] = regs.wordregs[regax]; regs.wordregs[regax] = oper1; break;
			case 0x96: oper1 = regs.wordregs[regsi]; regs.wordregs[regsi] = regs.wordregs[regax]; regs.wordregs[regax] = oper1; break;
			case 0x97: oper1 = regs.wordregs[regdi]; regs.wordregs[regdi] = regs.wordregs[regax]; regs.wordregs[regax] = oper1; break;

			case 0x98: /* CBW */
				regs.byteregs[regah] = (regs.byteregs[regal] & 0x80) ? 0xFF : 0; break;
			case 0x99: /* CWD */
				regs.wordregs[regdx] = (regs.byteregs[regah] & 0x80) ? 0xFFFF : 0; break;

			case 0x9A: /* 9A CALL Ap */
				oper1 = getmem16(segregs[regcs], ip); StepIP(2);
				oper2 = getmem16(segregs[regcs], ip); StepIP(2);
				push(segregs[regcs]); push(ip);
				ip = oper1; segregs[regcs] = oper2; break;
			case 0x9B: break; /* WAIT */
			case 0x9C: /* PUSHF */
#ifdef CPU_SET_HIGH_FLAGS
				push(makeflagsword() | 0xF800);
#else
				push(makeflagsword() | 0x0800);
#endif
				break;
			case 0x9D: /* POPF */
				temp16 = pop(); decodeflagsword(temp16); break;
			case 0x9E: /* SAHF */
				decodeflagsword((makeflagsword() & 0xFF00) | regs.byteregs[regah]); break;
			case 0x9F: /* LAHF */
				regs.byteregs[regah] = makeflagsword() & 0xFF; break;

			case 0xA0: /* A0 MOV AL Ob */
				regs.byteregs[regal] = getmem8(useseg, getmem16(segregs[regcs], ip));
				StepIP(2); break;
			case 0xA1: /* A1 MOV AX Ov */
				oper1 = getmem16(useseg, getmem16(segregs[regcs], ip));
				StepIP(2); regs.wordregs[regax] = oper1; break;
			case 0xA2: /* A2 MOV Ob AL */
				putmem8(useseg, getmem16(segregs[regcs], ip), regs.byteregs[regal]);
				StepIP(2); break;
			case 0xA3: /* A3 MOV Ov AX */
				putmem16(useseg, getmem16(segregs[regcs], ip), regs.wordregs[regax]);
				StepIP(2); break;

			case 0xA4: /* A4 MOVSB */
				if (reptype && (regs.wordregs[regcx] == 0)) break;
				putmem8(segregs[reges], regs.wordregs[regdi], getmem8(useseg, regs.wordregs[regsi]));
				if (df) { regs.wordregs[regsi]--; regs.wordregs[regdi]--; }
				else { regs.wordregs[regsi]++; regs.wordregs[regdi]++; }
				if (reptype) regs.wordregs[regcx]--;
				totalexec++; loopcount++;
				if (!reptype) break;
				ip = firstip; break;
			case 0xA5: /* A5 MOVSW */
				if (reptype && (regs.wordregs[regcx] == 0)) break;
				putmem16(segregs[reges], regs.wordregs[regdi], getmem16(useseg, regs.wordregs[regsi]));
				if (df) { regs.wordregs[regsi] -= 2; regs.wordregs[regdi] -= 2; }
				else { regs.wordregs[regsi] += 2; regs.wordregs[regdi] += 2; }
				if (reptype) regs.wordregs[regcx]--;
				totalexec++; loopcount++;
				if (!reptype) break;
				ip = firstip; break;

			case 0xA6: /* A6 CMPSB */
				if (reptype && (regs.wordregs[regcx] == 0)) break;
				oper1b = getmem8(useseg, regs.wordregs[regsi]);
				oper2b = getmem8(segregs[reges], regs.wordregs[regdi]);
				if (df) { regs.wordregs[regsi]--; regs.wordregs[regdi]--; }
				else { regs.wordregs[regsi]++; regs.wordregs[regdi]++; }
				flag_sub8(oper1b, oper2b);
				if (reptype) regs.wordregs[regcx]--;
				if ((reptype == 1) && !zf) break;
				else if ((reptype == 2) && (zf == 1)) break;
				totalexec++; loopcount++;
				if (!reptype) break;
				ip = firstip; break;
			case 0xA7: /* A7 CMPSW */
				if (reptype && (regs.wordregs[regcx] == 0)) break;
				oper1 = getmem16(useseg, regs.wordregs[regsi]);
				oper2 = getmem16(segregs[reges], regs.wordregs[regdi]);
				if (df) { regs.wordregs[regsi] -= 2; regs.wordregs[regdi] -= 2; }
				else { regs.wordregs[regsi] += 2; regs.wordregs[regdi] += 2; }
				flag_sub16(oper1, oper2);
				if (reptype) regs.wordregs[regcx]--;
				if ((reptype == 1) && !zf) break;
				if ((reptype == 2) && (zf == 1)) break;
				totalexec++; loopcount++;
				if (!reptype) break;
				ip = firstip; break;

			case 0xA8: /* A8 TEST AL Ib */
				oper1b = regs.byteregs[regal]; oper2b = getmem8(segregs[regcs], ip);
				StepIP(1); flag_log8(oper1b & oper2b); break;
			case 0xA9: /* A9 TEST AX Iv */
				oper1 = regs.wordregs[regax]; oper2 = getmem16(segregs[regcs], ip);
				StepIP(2); flag_log16(oper1 & oper2); break;

			case 0xAA: /* AA STOSB */
				if (reptype && (regs.wordregs[regcx] == 0)) break;
				putmem8(segregs[reges], regs.wordregs[regdi], regs.byteregs[regal]);
				if (df) regs.wordregs[regdi]--; else regs.wordregs[regdi]++;
				if (reptype) regs.wordregs[regcx]--;
				totalexec++; loopcount++;
				if (!reptype) break;
				ip = firstip; break;
			case 0xAB: /* AB STOSW */
				if (reptype && (regs.wordregs[regcx] == 0)) break;
				putmem16(segregs[reges], regs.wordregs[regdi], regs.wordregs[regax]);
				if (df) regs.wordregs[regdi] -= 2; else regs.wordregs[regdi] += 2;
				if (reptype) regs.wordregs[regcx]--;
				totalexec++; loopcount++;
				if (!reptype) break;
				ip = firstip; break;

			case 0xAC: /* AC LODSB */
				if (reptype && (regs.wordregs[regcx] == 0)) break;
				regs.byteregs[regal] = getmem8(useseg, regs.wordregs[regsi]);
				if (df) regs.wordregs[regsi]--; else regs.wordregs[regsi]++;
				if (reptype) regs.wordregs[regcx]--;
				totalexec++; loopcount++;
				if (!reptype) break;
				ip = firstip; break;
			case 0xAD: /* AD LODSW */
				if (reptype && (regs.wordregs[regcx] == 0)) break;
				oper1 = getmem16(useseg, regs.wordregs[regsi]);
				regs.wordregs[regax] = oper1;
				if (df) regs.wordregs[regsi] -= 2; else regs.wordregs[regsi] += 2;
				if (reptype) regs.wordregs[regcx]--;
				totalexec++; loopcount++;
				if (!reptype) break;
				ip = firstip; break;

			case 0xAE: /* AE SCASB */
				if (reptype && (regs.wordregs[regcx] == 0)) break;
				oper1b = regs.byteregs[regal];
				oper2b = getmem8(segregs[reges], regs.wordregs[regdi]);
				flag_sub8(oper1b, oper2b);
				if (df) regs.wordregs[regdi]--; else regs.wordregs[regdi]++;
				if (reptype) regs.wordregs[regcx]--;
				if ((reptype == 1) && !zf) break;
				else if ((reptype == 2) && (zf == 1)) break;
				totalexec++; loopcount++;
				if (!reptype) break;
				ip = firstip; break;
			case 0xAF: /* AF SCASW */
				if (reptype && (regs.wordregs[regcx] == 0)) break;
				oper1 = regs.wordregs[regax];
				oper2 = getmem16(segregs[reges], regs.wordregs[regdi]);
				flag_sub16(oper1, oper2);
				if (df) regs.wordregs[regdi] -= 2; else regs.wordregs[regdi] += 2;
				if (reptype) regs.wordregs[regcx]--;
				if ((reptype == 1) && !zf) break;
				else if ((reptype == 2) && (zf == 1)) break;
				totalexec++; loopcount++;
				if (!reptype) break;
				ip = firstip; break;

			/* B0-B7 MOV reg8, Ib */
			case 0xB0: regs.byteregs[regal] = getmem8(segregs[regcs], ip); StepIP(1); break;
			case 0xB1: regs.byteregs[regcl] = getmem8(segregs[regcs], ip); StepIP(1); break;
			case 0xB2: regs.byteregs[regdl] = getmem8(segregs[regcs], ip); StepIP(1); break;
			case 0xB3: regs.byteregs[regbl] = getmem8(segregs[regcs], ip); StepIP(1); break;
			case 0xB4: regs.byteregs[regah] = getmem8(segregs[regcs], ip); StepIP(1); break;
			case 0xB5: regs.byteregs[regch] = getmem8(segregs[regcs], ip); StepIP(1); break;
			case 0xB6: regs.byteregs[regdh] = getmem8(segregs[regcs], ip); StepIP(1); break;
			case 0xB7: regs.byteregs[regbh] = getmem8(segregs[regcs], ip); StepIP(1); break;

			/* B8-BF MOV reg16, Iv */
			case 0xB8: regs.wordregs[regax] = getmem16(segregs[regcs], ip); StepIP(2); break;
			case 0xB9: regs.wordregs[regcx] = getmem16(segregs[regcs], ip); StepIP(2); break;
			case 0xBA: regs.wordregs[regdx] = getmem16(segregs[regcs], ip); StepIP(2); break;
			case 0xBB: regs.wordregs[regbx] = getmem16(segregs[regcs], ip); StepIP(2); break;
			case 0xBC: regs.wordregs[regsp] = getmem16(segregs[regcs], ip); StepIP(2); break;
			case 0xBD: regs.wordregs[regbp] = getmem16(segregs[regcs], ip); StepIP(2); break;
			case 0xBE: regs.wordregs[regsi] = getmem16(segregs[regcs], ip); StepIP(2); break;
			case 0xBF: regs.wordregs[regdi] = getmem16(segregs[regcs], ip); StepIP(2); break;

			case 0xC0: /* C0 GRP2 byte imm8 (80186+) */
				modregrm(); oper1b = readrm8(rm);
				oper2b = getmem8(segregs[regcs], ip); StepIP(1);
				writerm8(rm, op_grp2_8(oper2b)); break;
			case 0xC1: /* C1 GRP2 word imm8 (80186+) */
				modregrm(); oper1 = readrm16(rm);
				oper2 = getmem8(segregs[regcs], ip); StepIP(1);
				writerm16(rm, op_grp2_16((uint8_t)oper2)); break;
			case 0xC2: /* C2 RET Iw */
				oper1 = getmem16(segregs[regcs], ip);
				ip = pop(); regs.wordregs[regsp] += oper1; break;
			case 0xC3: ip = pop(); break; /* C3 RET */
			case 0xC4: /* C4 LES Gv Mp */
				modregrm(); getea(rm);
				putreg16(reg, read86(ea) + read86(ea + 1) * 256);
				segregs[reges] = read86(ea + 2) + read86(ea + 3) * 256; break;
			case 0xC5: /* C5 LDS Gv Mp */
				modregrm(); getea(rm);
				putreg16(reg, read86(ea) + read86(ea + 1) * 256);
				segregs[regds] = read86(ea + 2) + read86(ea + 3) * 256; break;
			case 0xC6: /* C6 MOV Eb Ib */
				modregrm(); writerm8(rm, getmem8(segregs[regcs], ip)); StepIP(1); break;
			case 0xC7: /* C7 MOV Ev Iv */
				modregrm(); writerm16(rm, getmem16(segregs[regcs], ip)); StepIP(2); break;

			case 0xC8: /* C8 ENTER (80186+) */
				stacksize = getmem16(segregs[regcs], ip); StepIP(2);
				nestlev = getmem8(segregs[regcs], ip); StepIP(1);
				push(regs.wordregs[regbp]);
				frametemp = regs.wordregs[regsp];
				if (nestlev) {
					for (temp16 = 1; temp16 < nestlev; temp16++) {
						regs.wordregs[regbp] -= 2;
						push(regs.wordregs[regbp]);
					}
					push(regs.wordregs[regsp]);
				}
				regs.wordregs[regbp] = frametemp;
				regs.wordregs[regsp] = regs.wordregs[regbp] - stacksize;
				break;
			case 0xC9: /* C9 LEAVE (80186+) */
				regs.wordregs[regsp] = regs.wordregs[regbp];
				regs.wordregs[regbp] = pop(); break;

			case 0xCA: /* CA RETF Iw */
				oper1 = getmem16(segregs[regcs], ip);
				ip = pop(); segregs[regcs] = pop();
				regs.wordregs[regsp] += oper1; break;
			case 0xCB: /* CB RETF */
				ip = pop(); segregs[regcs] = pop(); break;
			case 0xCC: intcall86(3); break; /* INT 3 */
			case 0xCD: /* CD INT Ib */
				oper1b = getmem8(segregs[regcs], ip); StepIP(1);
				intcall86(oper1b); break;
			case 0xCE: if (of) intcall86(4); break; /* INTO */
			case 0xCF: /* IRET */
				ip = pop(); segregs[regcs] = pop();
				decodeflagsword(pop()); break;

			case 0xD0: /* D0 GRP2 Eb 1 */
				modregrm(); oper1b = readrm8(rm);
				writerm8(rm, op_grp2_8(1)); break;
			case 0xD1: /* D1 GRP2 Ev 1 */
				modregrm(); oper1 = readrm16(rm);
				writerm16(rm, op_grp2_16(1)); break;
			case 0xD2: /* D2 GRP2 Eb CL */
				modregrm(); oper1b = readrm8(rm);
				writerm8(rm, op_grp2_8(regs.byteregs[regcl])); break;
			case 0xD3: /* D3 GRP2 Ev CL */
				modregrm(); oper1 = readrm16(rm);
				writerm16(rm, op_grp2_16(regs.byteregs[regcl])); break;

			case 0xD4: /* D4 AAM */
				oper1 = getmem8(segregs[regcs], ip); StepIP(1);
				if (!oper1) { intcall86(0); break; }
				regs.byteregs[regah] = (regs.byteregs[regal] / oper1) & 255;
				regs.byteregs[regal] = (regs.byteregs[regal] % oper1) & 255;
				flag_szp16(regs.wordregs[regax]); break;
			case 0xD5: /* D5 AAD */
				oper1 = getmem8(segregs[regcs], ip); StepIP(1);
				regs.byteregs[regal] = (regs.byteregs[regah] * oper1 + regs.byteregs[regal]) & 255;
				regs.byteregs[regah] = 0;
				flag_szp16(regs.byteregs[regah] * oper1 + regs.byteregs[regal]);
				sf = 0; break;
			case 0xD6: /* D6 XLAT on V20/V30, SALC on 8086/8088 */
#ifndef CPU_NO_SALC
				regs.byteregs[regal] = cf ? 0xFF : 0x00;
				break;
#endif
			case 0xD7: /* D7 XLAT */
				regs.byteregs[regal] = read86(useseg * 16 + regs.wordregs[regbx] + regs.byteregs[regal]);
				break;

			case 0xD8: case 0xD9: case 0xDA: case 0xDB:
			case 0xDC: case 0xDD: case 0xDE: case 0xDF:
				/* escape to x87 FPU (unsupported) */
				modregrm(); break;

			case 0xE0: /* E0 LOOPNZ Jb */
				temp16 = signext(getmem8(segregs[regcs], ip)); StepIP(1);
				regs.wordregs[regcx]--;
				if (regs.wordregs[regcx] && !zf) ip = ip + temp16;
				break;
			case 0xE1: /* E1 LOOPZ Jb */
				temp16 = signext(getmem8(segregs[regcs], ip)); StepIP(1);
				regs.wordregs[regcx]--;
				if (regs.wordregs[regcx] && (zf == 1)) ip = ip + temp16;
				break;
			case 0xE2: /* E2 LOOP Jb */
				temp16 = signext(getmem8(segregs[regcs], ip)); StepIP(1);
				regs.wordregs[regcx]--;
				if (regs.wordregs[regcx]) ip = ip + temp16;
				break;
			case 0xE3: /* E3 JCXZ Jb */
				temp16 = signext(getmem8(segregs[regcs], ip)); StepIP(1);
				if (!regs.wordregs[regcx]) ip = ip + temp16;
				break;

			case 0xE4: /* E4 IN AL Ib */
				oper1b = getmem8(segregs[regcs], ip); StepIP(1);
				regs.byteregs[regal] = (uint8_t)portin(oper1b); break;
			case 0xE5: /* E5 IN AX Ib */
				oper1b = getmem8(segregs[regcs], ip); StepIP(1);
				regs.wordregs[regax] = portin16(oper1b); break;
			case 0xE6: /* E6 OUT Ib AL */
				oper1b = getmem8(segregs[regcs], ip); StepIP(1);
				portout(oper1b, regs.byteregs[regal]); break;
			case 0xE7: /* E7 OUT Ib AX */
				oper1b = getmem8(segregs[regcs], ip); StepIP(1);
				portout16(oper1b, regs.wordregs[regax]); break;

			case 0xE8: /* E8 CALL Jv */
				oper1 = getmem16(segregs[regcs], ip); StepIP(2);
				push(ip); ip = ip + oper1; break;
			case 0xE9: /* E9 JMP Jv */
				oper1 = getmem16(segregs[regcs], ip); StepIP(2);
				ip = ip + oper1; break;
			case 0xEA: /* EA JMP Ap */
				oper1 = getmem16(segregs[regcs], ip); StepIP(2);
				oper2 = getmem16(segregs[regcs], ip);
				ip = oper1; segregs[regcs] = oper2; break;
			case 0xEB: /* EB JMP Jb */
				oper1 = signext(getmem8(segregs[regcs], ip)); StepIP(1);
				ip = ip + oper1; break;

			case 0xEC: /* EC IN AL DX */
				oper1 = regs.wordregs[regdx];
				regs.byteregs[regal] = (uint8_t)portin(oper1); break;
			case 0xED: /* ED IN AX DX */
				oper1 = regs.wordregs[regdx];
				regs.wordregs[regax] = portin16(oper1); break;
			case 0xEE: /* EE OUT DX AL */
				oper1 = regs.wordregs[regdx];
				portout(oper1, regs.byteregs[regal]); break;
			case 0xEF: /* EF OUT DX AX */
				oper1 = regs.wordregs[regdx];
				portout16(oper1, regs.wordregs[regax]); break;

			case 0xF0: break; /* F0 LOCK */
			case 0xF4: hltstate = 1; break; /* F4 HLT */
			case 0xF5: cf = cf ? 0 : 1; break; /* F5 CMC */

			case 0xF6: /* F6 GRP3a Eb */
				modregrm(); oper1b = readrm8(rm); op_grp3_8();
				if ((reg > 1) && (reg < 4)) writerm8(rm, res8);
				break;
			case 0xF7: /* F7 GRP3b Ev */
				modregrm(); oper1 = readrm16(rm); op_grp3_16();
				if ((reg > 1) && (reg < 4)) writerm16(rm, res16);
				break;

			case 0xF8: cf = 0; break; /* CLC */
			case 0xF9: cf = 1; break; /* STC */
			case 0xFA: ifl = 0; break; /* CLI */
			case 0xFB: ifl = 1; break; /* STI */
			case 0xFC: df = 0; break; /* CLD */
			case 0xFD: df = 1; break; /* STD */

			case 0xFE: /* FE GRP4 Eb */
				modregrm(); oper1b = readrm8(rm); oper2b = 1;
				if (!reg) {
					tempcf = cf; res8 = oper1b + oper2b;
					flag_add8(oper1b, oper2b); cf = tempcf;
					writerm8(rm, res8);
				} else {
					tempcf = cf; res8 = oper1b - oper2b;
					flag_sub8(oper1b, oper2b); cf = tempcf;
					writerm8(rm, res8);
				}
				break;

			case 0xFF: /* FF GRP5 Ev */
				modregrm(); oper1 = readrm16(rm); op_grp5(); break;

			default:
#ifdef CPU_ALLOW_ILLEGAL_OP_EXCEPTION
				intcall86(6); /* invalid opcode exception (80186+) */
#endif
				break;
		}

skipexecution:
		if (!running) return;
	}
}
