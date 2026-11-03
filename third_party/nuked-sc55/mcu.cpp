/*
 * Copyright (C) 2021, 2024 nukeykt
 *
 * This file is part of Nuked-SC55.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
/*
 * mw2port: the emulator core only, built as a library for the MechWarrior 2 port's menu music. Removed from upstream's
 * mcu.cpp: SDL (audio output, the work thread and its lock, the button atomics), the LCD front end (the LCD writes go to
 * the no-op stubs below), MIDI input (rtmidi / win32) and main(); the ROM loading and the work thread's step loop are
 * the API at the end of this file (sc55_core.h). The emulation itself is unchanged.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mcu.h"
#include "mcu_opcodes.h"
#include "mcu_interrupt.h"
#include "mcu_timer.h"
#include "pcm.h"
#include "submcu.h"
#include "sc55_core.h"

/* mw2port: no LCD - the firmware's display writes are ignored */
static void LCD_Write(uint32_t address, uint8_t data) { (void)address; (void)data; }
static void LCD_Enable(uint32_t enable) { (void)enable; }

const char* rs_name[ROM_SET_COUNT] = {
    "SC-55mk2",
    "SC-55st",
    "SC-55mk1",
    "CM-300/SCC-1",
    "JV-880",
    "SCB-55",
    "RLP-3237",
    "SC-155",
    "SC-155mk2"
};

static const int ROM_SET_N_FILES = 6;

const char* roms[ROM_SET_COUNT][ROM_SET_N_FILES] =
{
    "rom1.bin",
    "rom2.bin",
    "waverom1.bin",
    "waverom2.bin",
    "rom_sm.bin",
    "",

    "rom1.bin",
    "rom2_st.bin",
    "waverom1.bin",
    "waverom2.bin",
    "rom_sm.bin",
    "",

    "sc55_rom1.bin",
    "sc55_rom2.bin",
    "sc55_waverom1.bin",
    "sc55_waverom2.bin",
    "sc55_waverom3.bin",
    "",

    "cm300_rom1.bin",
    "cm300_rom2.bin",
    "cm300_waverom1.bin",
    "cm300_waverom2.bin",
    "cm300_waverom3.bin",
    "",

    "jv880_rom1.bin",
    "jv880_rom2.bin",
    "jv880_waverom1.bin",
    "jv880_waverom2.bin",
    "jv880_waverom_expansion.bin",
    "jv880_waverom_pcmcard.bin",

    "scb55_rom1.bin",
    "scb55_rom2.bin",
    "scb55_waverom1.bin",
    "scb55_waverom2.bin",
    "",
    "",

    "rlp3237_rom1.bin",
    "rlp3237_rom2.bin",
    "rlp3237_waverom1.bin",
    "",
    "",
    "",

    "sc155_rom1.bin",
    "sc155_rom2.bin",
    "sc155_waverom1.bin",
    "sc155_waverom2.bin",
    "sc155_waverom3.bin",
    "",

    "rom1.bin",
    "rom2.bin",
    "waverom1.bin",
    "waverom2.bin",
    "rom_sm.bin",
    "",
};

int romset = ROM_SET_MK2;

static const int ROM1_SIZE = 0x8000;
static const int ROM2_SIZE = 0x80000;
static const int RAM_SIZE = 0x400;
static const int SRAM_SIZE = 0x8000;
static const int NVRAM_SIZE = 0x8000; // JV880 only
static const int CARDRAM_SIZE = 0x8000; // JV880 only
static const int ROMSM_SIZE = 0x1000;


/* mw2port: the samples go to the caller's buffer (nsc55_render), the overflow of the last step kept for the next call */
static int16_t *sink;
static int sink_cap, sink_n;
static int16_t sink_carry[8];
static int sink_ncarry;

void MCU_ErrorTrap(void)
{
    printf("%.2x %.4x\n", mcu.cp, mcu.pc);
}

int mcu_mk1 = 0; // 0 - SC-55mkII, SC-55ST. 1 - SC-55, CM-300/SCC-1
int mcu_cm300 = 0; // 0 - SC-55, 1 - CM-300/SCC-1
int mcu_st = 0; // 0 - SC-55mk2, 1 - SC-55ST
int mcu_jv880 = 0; // 0 - SC-55, 1 - JV880
int mcu_scb55 = 0; // 0 - sub mcu (e.g SC-55mk2), 1 - no sub mcu (e.g SCB-55)
int mcu_sc155 = 0; // 0 - SC-55(MK2), 1 - SC-155(MK2)

static int ga_int[8];
static int ga_int_enable = 0;
static int ga_int_trigger = 0;
static int ga_lcd_counter = 0;


uint8_t dev_register[0x80];

static uint16_t ad_val[4];
static uint8_t ad_nibble = 0x00;
static uint8_t sw_pos = 3;
static uint8_t io_sd = 0x00;

uint32_t mcu_button_pressed = 0;   /* mw2port: no front panel */

uint8_t RCU_Read(void)
{
    return 0;
}

enum {
    ANALOG_LEVEL_RCU_LOW = 0,
    ANALOG_LEVEL_RCU_HIGH = 0,
    ANALOG_LEVEL_SW_0 = 0,
    ANALOG_LEVEL_SW_1 = 0x155,
    ANALOG_LEVEL_SW_2 = 0x2aa,
    ANALOG_LEVEL_SW_3 = 0x3ff,
    ANALOG_LEVEL_BATTERY = 0x2a0,
};

uint16_t MCU_SC155Sliders(uint32_t index)
{
    // 0 - 1/9
    // 1 - 2/10
    // 2 - 3/11
    // 3 - 4/12
    // 4 - 5/13
    // 5 - 6/14
    // 6 - 7/15
    // 7 - 8/16
    // 8 - ALL
    return 0x0;
}

uint16_t MCU_AnalogReadPin(uint32_t pin)
{
    if (mcu_cm300)
        return 0;
    if (mcu_jv880)
    {
        if (pin == 1)
            return ANALOG_LEVEL_BATTERY;
        return 0x3ff;
    }
    if (0)
    {
READ_RCU:
        uint8_t rcu = RCU_Read();
        if (rcu & (1 << pin))
            return ANALOG_LEVEL_RCU_HIGH;
        else
            return ANALOG_LEVEL_RCU_LOW;
    }
    if (mcu_mk1)
    {
        if (mcu_sc155 && (dev_register[DEV_P9DR] & 1) != 0)
        {
            return MCU_SC155Sliders(pin);
        }
        if (pin == 7)
        {
            if (mcu_sc155 && (dev_register[DEV_P9DR] & 2) != 0)
                return MCU_SC155Sliders(8);
            else
                return ANALOG_LEVEL_BATTERY;
        }
        else
            goto READ_RCU;
    }
    else
    {
        if (mcu_sc155 && (io_sd & 16) != 0)
        {
            return MCU_SC155Sliders(pin);
        }
        if (pin == 7)
        {
            if (mcu_mk1)
                return ANALOG_LEVEL_BATTERY;
            switch ((io_sd >> 2) & 3)
            {
            case 0: // Battery voltage
                return ANALOG_LEVEL_BATTERY;
            case 1: // NC
                if (mcu_sc155)
                    return MCU_SC155Sliders(8);
                return 0;
            case 2: // SW
                switch (sw_pos)
                {
                case 0:
                default:
                    return ANALOG_LEVEL_SW_0;
                case 1:
                    return ANALOG_LEVEL_SW_1;
                case 2:
                    return ANALOG_LEVEL_SW_2;
                case 3:
                    return ANALOG_LEVEL_SW_3;
                }
            case 3: // RCU
                goto READ_RCU;
            }
        }
        else
            goto READ_RCU;
    }
}

void MCU_AnalogSample(int channel)
{
    int value = MCU_AnalogReadPin(channel);
    int dest = (channel << 1) & 6;
    dev_register[DEV_ADDRAH + dest] = value >> 2;
    dev_register[DEV_ADDRAL + dest] = (value << 6) & 0xc0;
}

int adf_rd = 0;

uint64_t analog_end_time;

int ssr_rd = 0;

uint32_t uart_write_ptr;
uint32_t uart_read_ptr;
uint8_t uart_buffer[uart_buffer_size];

static uint8_t uart_rx_byte;
static uint64_t uart_rx_delay;
static uint64_t uart_tx_delay;

void MCU_DeviceWrite(uint32_t address, uint8_t data)
{
    address &= 0x7f;
    if (address >= 0x10 && address < 0x40)
    {
        TIMER_Write(address, data);
        return;
    }
    if (address >= 0x50 && address < 0x55)
    {
        TIMER2_Write(address, data);
        return;
    }
    switch (address)
    {
    case DEV_P1DDR: // P1DDR
        break;
    case DEV_P5DDR:
        break;
    case DEV_P6DDR:
        break;
    case DEV_P7DDR:
        break;
    case DEV_SCR:
        break;
    case DEV_WCR:
        break;
    case DEV_P9DDR:
        break;
    case DEV_RAME: // RAME
        break;
    case DEV_P1CR: // P1CR
        break;
    case DEV_DTEA:
        break;
    case DEV_DTEB:
        break;
    case DEV_DTEC:
        break;
    case DEV_DTED:
        break;
    case DEV_SMR:
        break;
    case DEV_BRR:
        break;
    case DEV_IPRA:
        break;
    case DEV_IPRB:
        break;
    case DEV_IPRC:
        break;
    case DEV_IPRD:
        break;
    case DEV_PWM1_DTR:
        break;
    case DEV_PWM1_TCR:
        break;
    case DEV_PWM2_DTR:
        break;
    case DEV_PWM2_TCR:
        break;
    case DEV_PWM3_DTR:
        break;
    case DEV_PWM3_TCR:
        break;
    case DEV_P7DR:
        break;
    case DEV_TMR_TCNT:
        break;
    case DEV_TMR_TCR:
        break;
    case DEV_TMR_TCSR:
        break;
    case DEV_TMR_TCORA:
        break;
    case DEV_TDR:
        break;
    case DEV_ADCSR:
    {
        dev_register[address] &= ~0x7f;
        dev_register[address] |= data & 0x7f;
        if ((data & 0x80) == 0 && adf_rd)
        {
            dev_register[address] &= ~0x80;
            MCU_Interrupt_SetRequest(INTERRUPT_SOURCE_ANALOG, 0);
        }
        if ((data & 0x40) == 0)
            MCU_Interrupt_SetRequest(INTERRUPT_SOURCE_ANALOG, 0);
        return;
    }
    case DEV_SSR:
    {
        if ((data & 0x80) == 0 && (ssr_rd & 0x80) != 0)
        {
            dev_register[address] &= ~0x80;
            uart_tx_delay = mcu.cycles + 3000;
            MCU_Interrupt_SetRequest(INTERRUPT_SOURCE_UART_TX, 0);
        }
        if ((data & 0x40) == 0 && (ssr_rd & 0x40) != 0)
        {
            uart_rx_delay = mcu.cycles + 3000;
            dev_register[address] &= ~0x40;
            MCU_Interrupt_SetRequest(INTERRUPT_SOURCE_UART_RX, 0);
        }
        if ((data & 0x20) == 0 && (ssr_rd & 0x20) != 0)
        {
            dev_register[address] &= ~0x20;
        }
        if ((data & 0x10) == 0 && (ssr_rd & 0x10) != 0)
        {
            dev_register[address] &= ~0x10;
        }
        break;
    }
    default:
        address += 0;
        break;
    }
    dev_register[address] = data;
}

uint8_t MCU_DeviceRead(uint32_t address)
{
    address &= 0x7f;
    if (address >= 0x10 && address < 0x40)
    {
        return TIMER_Read(address);
    }
    if (address >= 0x50 && address < 0x55)
    {
        return TIMER_Read2(address);
    }
    switch (address)
    {
    case DEV_ADDRAH:
    case DEV_ADDRAL:
    case DEV_ADDRBH:
    case DEV_ADDRBL:
    case DEV_ADDRCH:
    case DEV_ADDRCL:
    case DEV_ADDRDH:
    case DEV_ADDRDL:
        return dev_register[address];
    case DEV_ADCSR:
        adf_rd = (dev_register[address] & 0x80) != 0;
        return dev_register[address];
    case DEV_SSR:
        ssr_rd = dev_register[address];
        return dev_register[address];
    case DEV_RDR:
        return uart_rx_byte;
    case 0x00:
        return 0xff;
    case DEV_P7DR:
    {
        if (!mcu_jv880) return 0xff;

        uint8_t data = 0xff;
        uint32_t button_pressed = mcu_button_pressed;

        if (io_sd == 0b11111011)
            data &= ((button_pressed >> 0) & 0b11111) ^ 0xFF;
        if (io_sd == 0b11110111)
            data &= ((button_pressed >> 5) & 0b11111) ^ 0xFF;
        if (io_sd == 0b11101111)
            data &= ((button_pressed >> 10) & 0b1111) ^ 0xFF;

        data |= 0b10000000;
        return data;
    }
    case DEV_P9DR:
    {
        int cfg = 0;
        if (!mcu_mk1)
            cfg = mcu_sc155 ? 0 : 2; // bit 1: 0 - SC-155mk2 (???), 1 - SC-55mk2

        int dir = dev_register[DEV_P9DDR];

        int val = cfg & (dir ^ 0xff);
        val |= dev_register[DEV_P9DR] & dir;
        return val;
    }
    case DEV_SCR:
    case DEV_TDR:
    case DEV_SMR:
        return dev_register[address];
    case DEV_IPRC:
    case DEV_IPRD:
    case DEV_DTEC:
    case DEV_DTED:
    case DEV_FRT2_TCSR:
    case DEV_FRT1_TCSR:
    case DEV_FRT1_TCR:
    case DEV_FRT1_FRCH:
    case DEV_FRT1_FRCL:
    case DEV_FRT3_TCSR:
    case DEV_FRT3_OCRAH:
    case DEV_FRT3_OCRAL:
        return dev_register[address];
    }
    return dev_register[address];
}

void MCU_DeviceReset(void)
{
    // dev_register[0x00] = 0x03;
    // dev_register[0x7c] = 0x87;
    dev_register[DEV_RAME] = 0x80;
    dev_register[DEV_SSR] = 0x80;
}

void MCU_UpdateAnalog(uint64_t cycles)
{
    int ctrl = dev_register[DEV_ADCSR];
    int isscan = (ctrl & 16) != 0;

    if (ctrl & 0x20)
    {
        if (analog_end_time == 0)
            analog_end_time = cycles + 200;
        else if (analog_end_time < cycles)
        {
            if (isscan)
            {
                int base = ctrl & 4;
                for (int i = 0; i <= (ctrl & 3); i++)
                    MCU_AnalogSample(base + i);
                analog_end_time = cycles + 200;
            }
            else
            {
                MCU_AnalogSample(ctrl & 7);
                dev_register[DEV_ADCSR] &= ~0x20;
                analog_end_time = 0;
            }
            dev_register[DEV_ADCSR] |= 0x80;
            if (ctrl & 0x40)
                MCU_Interrupt_SetRequest(INTERRUPT_SOURCE_ANALOG, 1);
        }
    }
    else
        analog_end_time = 0;
}

mcu_t mcu;

uint8_t rom1[ROM1_SIZE];
uint8_t rom2[ROM2_SIZE];
uint8_t ram[RAM_SIZE];
uint8_t sram[SRAM_SIZE];
uint8_t nvram[NVRAM_SIZE];
uint8_t cardram[CARDRAM_SIZE];

int rom2_mask = ROM2_SIZE - 1;

uint8_t MCU_Read(uint32_t address)
{
    uint32_t address_rom = address & 0x3ffff;
    if (address & 0x80000 && !mcu_jv880)
        address_rom |= 0x40000;
    uint8_t page = (address >> 16) & 0xf;
    address &= 0xffff;
    uint8_t ret = 0xff;
    switch (page)
    {
    case 0:
        if (!(address & 0x8000))
            ret = rom1[address & 0x7fff];
        else
        {
            if (!mcu_mk1)
            {
                uint16_t base = mcu_jv880 ? 0xf000 : 0xe000;
                if (address >= base && address < (base | 0x400))
                {
                    ret = PCM_Read(address & 0x3f);
                }
                else if (!mcu_scb55 && address >= 0xec00 && address < 0xf000)
                {
                    ret = SM_SysRead(address & 0xff);
                }
                else if (address >= 0xff80)
                {
                    ret = MCU_DeviceRead(address & 0x7f);
                }
                else if (address >= 0xfb80 && address < 0xff80
                    && (dev_register[DEV_RAME] & 0x80) != 0)
                    ret = ram[(address - 0xfb80) & 0x3ff];
                else if (address >= 0x8000 && address < 0xe000)
                {
                    ret = sram[address & 0x7fff];
                }
                else if (address == (base | 0x402))
                {
                    ret = ga_int_trigger;
                    ga_int_trigger = 0;
                    MCU_Interrupt_SetRequest(mcu_jv880 ? INTERRUPT_SOURCE_IRQ0 : INTERRUPT_SOURCE_IRQ1, 0);
                }
                else
                {
                    printf("Unknown read %x\n", address);
                    ret = 0xff;
                }
                //
                // e402:2-0 irq source
                //
            }
            else
            {
                if (address >= 0xe000 && address < 0xe040)
                {
                    ret = PCM_Read(address & 0x3f);
                }
                else if (address >= 0xff80)
                {
                    ret = MCU_DeviceRead(address & 0x7f);
                }
                else if (address >= 0xfb80 && address < 0xff80
                    && (dev_register[DEV_RAME] & 0x80) != 0)
                {
                    ret = ram[(address - 0xfb80) & 0x3ff];
                }
                else if (address >= 0x8000 && address < 0xe000)
                {
                    ret = sram[address & 0x7fff];
                }
                else if (address >= 0xf000 && address < 0xf100)
                {
                    io_sd = address & 0xff;

                    if (mcu_cm300)
                        return 0xff;

                    LCD_Enable((io_sd & 8) != 0);

                    uint8_t data = 0xff;
                    uint32_t button_pressed = mcu_button_pressed;

                    if ((io_sd & 1) == 0)
                        data &= ((button_pressed >> 0) & 255) ^ 255;
                    if ((io_sd & 2) == 0)
                        data &= ((button_pressed >> 8) & 255) ^ 255;
                    if ((io_sd & 4) == 0)
                        data &= ((button_pressed >> 16) & 255) ^ 255;
                    if ((io_sd & 8) == 0)
                        data &= ((button_pressed >> 24) & 255) ^ 255;
                    return data;
                }
                else if (address == 0xf106)
                {
                    ret = ga_int_trigger;
                    ga_int_trigger = 0;
                    MCU_Interrupt_SetRequest(INTERRUPT_SOURCE_IRQ1, 0);
                }
                else
                {
                    printf("Unknown read %x\n", address);
                    ret = 0xff;
                }
                //
                // f106:2-0 irq source
                //
            }
        }
        break;
#if 0
    case 3:
        ret = rom2[address | 0x30000];
        break;
    case 4:
        ret = rom2[address];
        break;
    case 10:
        ret = rom2[address | 0x60000]; // FIXME
        break;
    case 1:
        ret = rom2[address | 0x10000];
        break;
#endif
    case 1:
        ret = rom2[address_rom & rom2_mask];
        break;
    case 2:
        ret = rom2[address_rom & rom2_mask];
        break;
    case 3:
        ret = rom2[address_rom & rom2_mask];
        break;
    case 4:
        ret = rom2[address_rom & rom2_mask];
        break;
    case 8:
        if (!mcu_jv880)
            ret = rom2[address_rom & rom2_mask];
        else
            ret = 0xff;
        break;
    case 9:
        if (!mcu_jv880)
            ret = rom2[address_rom & rom2_mask];
        else
            ret = 0xff;
        break;
    case 14:
    case 15:
        if (!mcu_jv880)
            ret = rom2[address_rom & rom2_mask];
        else
            ret = cardram[address & 0x7fff]; // FIXME
        break;
    case 10:
    case 11:
        if (!mcu_mk1)
            ret = sram[address & 0x7fff]; // FIXME
        else
            ret = 0xff;
        break;
    case 12:
    case 13:
        if (mcu_jv880)
            ret = nvram[address & 0x7fff]; // FIXME
        else
            ret = 0xff;
        break;
    case 5:
        if (mcu_mk1)
            ret = sram[address & 0x7fff]; // FIXME
        else
            ret = 0xff;
        break;
    default:
        ret = 0x00;
        break;
    }
    return ret;
}

uint16_t MCU_Read16(uint32_t address)
{
    address &= ~1;
    uint8_t b0, b1;
    b0 = MCU_Read(address);
    b1 = MCU_Read(address+1);
    return (b0 << 8) + b1;
}

uint32_t MCU_Read32(uint32_t address)
{
    address &= ~3;
    uint8_t b0, b1, b2, b3;
    b0 = MCU_Read(address);
    b1 = MCU_Read(address+1);
    b2 = MCU_Read(address+2);
    b3 = MCU_Read(address+3);
    return (b0 << 24) + (b1 << 16) + (b2 << 8) + b3;
}

void MCU_Write(uint32_t address, uint8_t value)
{
    uint8_t page = (address >> 16) & 0xf;
    address &= 0xffff;
    if (page == 0)
    {
        if (address & 0x8000)
        {
            if (!mcu_mk1)
            {
                uint16_t base = mcu_jv880 ? 0xf000 : 0xe000;
                if (address >= (base | 0x400) && address < (base | 0x800))
                {
                    if (address == (base | 0x404) || address == (base | 0x405))
                        LCD_Write(address & 1, value);
                    else if (address == (base | 0x401))
                    {
                        io_sd = value;
                        LCD_Enable((value & 1) == 0);
                    }
                    else if (address == (base | 0x402))
                        ga_int_enable = (value << 1);
                    else
                        printf("Unknown write %x %x\n", address, value);
                    //
                    // e400: always 4?
                    // e401: SC0-6?
                    // e402: enable/disable IRQ?
                    // e403: always 1?
                    // e404: LCD
                    // e405: LCD
                    // e406: 0 or 40
                    // e407: 0, e406 continuation?
                    //
                }
                else if (address >= (base | 0x000) && address < (base | 0x400))
                {
                    PCM_Write(address & 0x3f, value);
                }
                else if (!mcu_scb55 && address >= 0xec00 && address < 0xf000)
                {
                    SM_SysWrite(address & 0xff, value);
                }
                else if (address >= 0xff80)
                {
                    MCU_DeviceWrite(address & 0x7f, value);
                }
                else if (address >= 0xfb80 && address < 0xff80
                    && (dev_register[DEV_RAME] & 0x80) != 0)
                {
                    ram[(address - 0xfb80) & 0x3ff] = value;
                }
                else if (address >= 0x8000 && address < 0xe000)
                {
                    sram[address & 0x7fff] = value;
                }
                else
                {
                    printf("Unknown write %x %x\n", address, value);
                }
            }
            else
            {
                if (address >= 0xe000 && address < 0xe040)
                {
                    PCM_Write(address & 0x3f, value);
                }
                else if (address >= 0xff80)
                {
                    MCU_DeviceWrite(address & 0x7f, value);
                }
                else if (address >= 0xfb80 && address < 0xff80
                    && (dev_register[DEV_RAME] & 0x80) != 0)
                {
                    ram[(address - 0xfb80) & 0x3ff] = value;
                }
                else if (address >= 0x8000 && address < 0xe000)
                {
                    sram[address & 0x7fff] = value;
                }
                else if (address >= 0xf000 && address < 0xf100)
                {
                    io_sd = address & 0xff;
                    LCD_Enable((io_sd & 8) != 0);
                }
                else if (address == 0xf105)
                {
                    LCD_Write(0, value);
                    ga_lcd_counter = 500;
                }
                else if (address == 0xf104)
                {
                    LCD_Write(1, value);
                    ga_lcd_counter = 500;
                }
                else if (address == 0xf107)
                {
                    io_sd = value;
                }
                else
                {
                    printf("Unknown write %x %x\n", address, value);
                }
            }
        }
        else if (mcu_jv880 && address >= 0x6196 && address <= 0x6199)
        {
            // nop: the jv880 rom writes into the rom at 002E77-002E7D
        }
        else
        {
            printf("Unknown write %x %x\n", address, value);
        }
    }
    else if (page == 5 && mcu_mk1)
    {
        sram[address & 0x7fff] = value; // FIXME
    }
    else if (page == 10 && !mcu_mk1)
    {
        sram[address & 0x7fff] = value; // FIXME
    }
    else if (page == 12 && mcu_jv880)
    {
        nvram[address & 0x7fff] = value; // FIXME
    }
    else if (page == 14 && mcu_jv880)
    {
        cardram[address & 0x7fff] = value; // FIXME
    }
    else
    {
        printf("Unknown write %x %x\n", (page << 16) | address, value);
    }
}

void MCU_Write16(uint32_t address, uint16_t value)
{
    address &= ~1;
    MCU_Write(address, value >> 8);
    MCU_Write(address + 1, value & 0xff);
}

void MCU_ReadInstruction(void)
{
    uint8_t operand = MCU_ReadCodeAdvance();

    MCU_Operand_Table[operand](operand);

    if (mcu.sr & STATUS_T)
    {
        MCU_Interrupt_Exception(EXCEPTION_SOURCE_TRACE);
    }
}

void MCU_Init(void)
{
    memset(&mcu, 0, sizeof(mcu_t));
}

void MCU_Reset(void)
{
    mcu.r[0] = 0;
    mcu.r[1] = 0;
    mcu.r[2] = 0;
    mcu.r[3] = 0;
    mcu.r[4] = 0;
    mcu.r[5] = 0;
    mcu.r[6] = 0;
    mcu.r[7] = 0;

    mcu.pc = 0;

    mcu.sr = 0x700;

    mcu.cp = 0;
    mcu.dp = 0;
    mcu.ep = 0;
    mcu.tp = 0;
    mcu.br = 0;

    uint32_t reset_address = MCU_GetVectorAddress(VECTOR_RESET);
    mcu.cp = (reset_address >> 16) & 0xff;
    mcu.pc = reset_address & 0xffff;

    mcu.exception_pending = -1;

    MCU_DeviceReset();

    if (mcu_mk1)
    {
        ga_int_enable = 255;
    }
}

void MCU_PostUART(uint8_t data)
{
    uart_buffer[uart_write_ptr] = data;
    uart_write_ptr = (uart_write_ptr + 1) % uart_buffer_size;
}

void MCU_UpdateUART_RX(void)
{
    if ((dev_register[DEV_SCR] & 16) == 0) // RX disabled
        return;
    if (uart_write_ptr == uart_read_ptr) // no byte
        return;

    if (dev_register[DEV_SSR] & 0x40)
        return;

    if (mcu.cycles < uart_rx_delay)
        return;

    uart_rx_byte = uart_buffer[uart_read_ptr];
    uart_read_ptr = (uart_read_ptr + 1) % uart_buffer_size;
    dev_register[DEV_SSR] |= 0x40;
    MCU_Interrupt_SetRequest(INTERRUPT_SOURCE_UART_RX, (dev_register[DEV_SCR] & 0x40) != 0);
}

// dummy TX
void MCU_UpdateUART_TX(void)
{
    if ((dev_register[DEV_SCR] & 32) == 0) // TX disabled
        return;

    if (dev_register[DEV_SSR] & 0x80)
        return;

    if (mcu.cycles < uart_tx_delay)
        return;

    dev_register[DEV_SSR] |= 0x80;
    MCU_Interrupt_SetRequest(INTERRUPT_SOURCE_UART_TX, (dev_register[DEV_SCR] & 0x80) != 0);

    // printf("tx:%x\n", dev_register[DEV_TDR]);
}

/* mw2port: one step of upstream's work thread (work_thread): an instruction, then the chips to its cycle count */
static void MCU_Step(void)
{
    if (!mcu.ex_ignore)
        MCU_Interrupt_Handle();
    else
        mcu.ex_ignore = 0;

    if (!mcu.sleep)
        MCU_ReadInstruction();

    mcu.cycles += 12; // FIXME: assume 12 cycles per instruction

    PCM_Update(mcu.cycles);

    TIMER_Clock(mcu.cycles);

    if (!mcu_mk1 && !mcu_jv880 && !mcu_scb55)
        SM_Update(mcu.cycles);
    else
    {
        MCU_UpdateUART_RX();
        MCU_UpdateUART_TX();
    }

    MCU_UpdateAnalog(mcu.cycles);

    if (mcu_mk1)
    {
        if (ga_lcd_counter)
        {
            ga_lcd_counter--;
            if (ga_lcd_counter == 0)
            {
                MCU_GA_SetGAInt(1, 0);
                MCU_GA_SetGAInt(1, 1);
            }
        }
    }
}

void MCU_PatchROM(void)
{
    //rom2[0x1333] = 0x11;
    //rom2[0x1334] = 0x19;
    //rom1[0x622d] = 0x19;
}

uint8_t mcu_p0_data = 0x00;
uint8_t mcu_p1_data = 0x00;

uint8_t MCU_ReadP0(void)
{
    return 0xff;
}

uint8_t MCU_ReadP1(void)
{
    uint8_t data = 0xff;
    uint32_t button_pressed = mcu_button_pressed;

    if ((mcu_p0_data & 1) == 0)
        data &= ((button_pressed >> 0) & 255) ^ 255;
    if ((mcu_p0_data & 2) == 0)
        data &= ((button_pressed >> 8) & 255) ^ 255;
    if ((mcu_p0_data & 4) == 0)
        data &= ((button_pressed >> 16) & 255) ^ 255;
    if ((mcu_p0_data & 8) == 0)
        data &= ((button_pressed >> 24) & 255) ^ 255;

    return data;
}

void MCU_WriteP0(uint8_t data)
{
    mcu_p0_data = data;
}

void MCU_WriteP1(uint8_t data)
{
    mcu_p1_data = data;
}


void unscramble(uint8_t *src, uint8_t *dst, int len)
{
    for (int i = 0; i < len; i++)
    {
        int address = i & ~0xfffff;
        static const int aa[] = {
            2, 0, 3, 4, 1, 9, 13, 10, 18, 17, 6, 15, 11, 16, 8, 5, 12, 7, 14, 19
        };
        for (int j = 0; j < 20; j++)
        {
            if (i & (1 << j))
                address |= 1<<aa[j];
        }
        uint8_t srcdata = src[address];
        uint8_t data = 0;
        static const int dd[] = {
            2, 0, 4, 5, 7, 6, 3, 1
        };
        for (int j = 0; j < 8; j++)
        {
            if (srcdata & (1 << dd[j]))
                data |= 1<<j;
        }
        dst[i] = data;
    }
}

void MCU_PostSample(int *sample)
{
    sample[0] >>= 15;
    if (sample[0] > INT16_MAX)
        sample[0] = INT16_MAX;
    else if (sample[0] < INT16_MIN)
        sample[0] = INT16_MIN;
    sample[1] >>= 15;
    if (sample[1] > INT16_MAX)
        sample[1] = INT16_MAX;
    else if (sample[1] < INT16_MIN)
        sample[1] = INT16_MIN;
    if (sink && sink_n < sink_cap)
    {
        sink[sink_n * 2 + 0] = (int16_t)sample[0];
        sink[sink_n * 2 + 1] = (int16_t)sample[1];
        sink_n++;
    }
    else if (sink_ncarry < 4)
    {
        sink_carry[sink_ncarry * 2 + 0] = (int16_t)sample[0];
        sink_carry[sink_ncarry * 2 + 1] = (int16_t)sample[1];
        sink_ncarry++;
    }
}

void MCU_GA_SetGAInt(int line, int value)
{
    // guesswork
    if (value && !ga_int[line] && (ga_int_enable & (1 << line)) != 0)
        ga_int_trigger = line;
    ga_int[line] = value;

    if (mcu_jv880)
        MCU_Interrupt_SetRequest(INTERRUPT_SOURCE_IRQ0, ga_int_trigger != 0);
    else
        MCU_Interrupt_SetRequest(INTERRUPT_SOURCE_IRQ1, ga_int_trigger != 0);
}

void MCU_EncoderTrigger(int dir)
{
    if (!mcu_jv880) return;
    MCU_GA_SetGAInt(dir == 0 ? 3 : 4, 0);
    MCU_GA_SetGAInt(dir == 0 ? 3 : 4, 1);
}

/* ---- mw2port: the library API (sc55_core.h) - upstream's main() ROM loading and reset sequence, without SDL ---- */

static int nsc55_loaded;

int nsc55_model_count(void)
{
    return ROM_SET_COUNT;
}

const char *nsc55_model_name(int model)
{
    return model >= 0 && model < ROM_SET_COUNT ? rs_name[model] : "";
}

const char *nsc55_rom_name(int model, int i)
{
    return model >= 0 && model < ROM_SET_COUNT && i >= 0 && i < ROM_SET_N_FILES ? roms[model][i] : "";
}

int nsc55_model_is_gm(int model)
{
    return model >= 0 && model < ROM_SET_COUNT && model != ROM_SET_JV880;   /* the JV-880 is no General MIDI module */
}

static int load_file(const char *path, uint8_t *dst, size_t want, size_t *got)
{
    FILE *f = path && path[0] ? fopen(path, "rb") : NULL;
    size_t n;
    if (!f) return -1;
    n = fread(dst, 1, want, f);
    fclose(f);
    if (got) *got = n;
    return 0;
}

/* a wave ROM: read (exactly len bytes) into a scratch buffer and unscrambled into dst */
static int load_wave(const char *path, uint8_t *tmp, uint8_t *dst, int len)
{
    size_t got = 0;
    if (load_file(path, tmp, (size_t)len, &got) != 0 || got != (size_t)len)
    {
        fprintf(stderr, "Nuked SC-55: cannot read the wave ROM %s (%d bytes expected)\n", path ? path : "", len);
        return -1;
    }
    unscramble(tmp, dst, len);
    return 0;
}

int nsc55_open(int model, const char *const paths[NSC55_FILES])
{
    size_t got = 0;
    uint8_t *tmp;
    int r = -1;
    if (nsc55_loaded || !nsc55_model_is_gm(model) || !paths) return -1;
    romset = model;
    mcu_mk1 = false;
    mcu_cm300 = false;
    mcu_st = false;
    mcu_jv880 = false;
    mcu_scb55 = false;
    mcu_sc155 = false;
    switch (romset)
    {
        case ROM_SET_MK2:
        case ROM_SET_SC155MK2:
            if (romset == ROM_SET_SC155MK2)
                mcu_sc155 = true;
            break;
        case ROM_SET_ST:
            mcu_st = true;
            break;
        case ROM_SET_MK1:
        case ROM_SET_SC155:
            mcu_mk1 = true;
            mcu_st = false;
            if (romset == ROM_SET_SC155)
                mcu_sc155 = true;
            break;
        case ROM_SET_CM300:
            mcu_mk1 = true;
            mcu_cm300 = true;
            break;
        case ROM_SET_SCB55:
        case ROM_SET_RLP3237:
            mcu_scb55 = true;
            break;
    }

    memset(&mcu, 0, sizeof(mcu_t));

    if (load_file(paths[0], rom1, ROM1_SIZE, &got) != 0 || got != (size_t)ROM1_SIZE)
    {
        fprintf(stderr, "Nuked SC-55: cannot read the mcu ROM1 %s\n", paths[0] ? paths[0] : "");
        return -1;
    }
    if (load_file(paths[1], rom2, ROM2_SIZE, &got) != 0 || (got != (size_t)ROM2_SIZE && got != (size_t)ROM2_SIZE / 2))
    {
        fprintf(stderr, "Nuked SC-55: cannot read the mcu ROM2 %s\n", paths[1] ? paths[1] : "");
        return -1;
    }
    rom2_mask = (int)got - 1;

    tmp = (uint8_t *)malloc(0x200000);
    if (!tmp) return -1;
    if (mcu_mk1)
    {
        if (load_wave(paths[2], tmp, waverom1, 0x100000) != 0 ||
            load_wave(paths[3], tmp, waverom2, 0x100000) != 0 ||
            load_wave(paths[4], tmp, waverom3, 0x100000) != 0)
            goto out;
    }
    else
    {
        if (load_wave(paths[2], tmp, waverom1, 0x200000) != 0)
            goto out;
        if (paths[3] && paths[3][0] && load_wave(paths[3], tmp, mcu_scb55 ? waverom3 : waverom2, 0x100000) != 0)
            goto out;
        if (paths[4] && paths[4][0] && (load_file(paths[4], sm_rom, ROMSM_SIZE, &got) != 0 || got != (size_t)ROMSM_SIZE))
        {
            fprintf(stderr, "Nuked SC-55: cannot read the sub mcu ROM %s\n", paths[4]);
            goto out;
        }
    }

    uart_write_ptr = uart_read_ptr = 0;
    MCU_Init();
    MCU_PatchROM();
    MCU_Reset();
    SM_Reset();
    PCM_Reset();
    nsc55_loaded = 1;
    r = 0;
out:
    free(tmp);
    return r;
}

void nsc55_close(void)
{
    nsc55_loaded = 0;
}

int nsc55_rate(void)
{
    return (mcu_mk1 || mcu_jv880) ? 64000 : 66207;   /* upstream's MCU_OpenAudio */
}

void nsc55_midi(const uint8_t *data, int len)
{
    int i;
    for (i = 0; i < len; i++)
        MCU_PostUART(data[i]);
}

int nsc55_render(int16_t *out, int frames)
{
    uint64_t limit;
    int i;
    if (!nsc55_loaded || !out || frames <= 0) return 0;
    sink = out; sink_cap = frames; sink_n = 0;
    for (i = 0; i < sink_ncarry && sink_n < sink_cap; i++, sink_n++)
    {
        out[sink_n * 2 + 0] = sink_carry[i * 2 + 0];
        out[sink_n * 2 + 1] = sink_carry[i * 2 + 1];
    }
    if (i < sink_ncarry) memmove(sink_carry, sink_carry + i * 2, (size_t)(sink_ncarry - i) * 2 * sizeof(int16_t));
    sink_ncarry -= i;
    /* a bound on the work (a broken ROM set must not hang the caller): 24 MHz / 8000 Hz per sample */
    limit = mcu.cycles + (uint64_t)frames * 3000 + 100000;
    while (sink_n < sink_cap && mcu.cycles < limit)
        MCU_Step();
    for (i = sink_n; i < frames; i++) out[i * 2] = out[i * 2 + 1] = 0;
    sink = NULL;
    return sink_n;
}

void nsc55_test_all_voices(void)
{
    pcm.config_reg_3c = 0x40;   /* oversampling, as the SC-55mk2 firmware sets it */
    pcm.config_reg_3d = 27;     /* 28 voice slots */
    pcm.voice_mask = pcm.voice_mask_pending = 0xfffffff;
}
