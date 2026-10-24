/**
 * @file    mcp4716.c
 * @brief   MCP4716A0 reference voltage DAC driver implementation.
 *
 * The device loads its volatile DAC register and configuration bits from EEPROM
 * on power-up. Initialization reads that state without writing anything. Voltage
 * updates use the write-volatile-memory command, or the write-all-memory command
 * when the new setting must also be stored in EEPROM.
 */

#include <pico/stdlib.h>

#include "mcp4716.h"
#include "iic_shard/iic_shard.h"
#include "log/log.h"

/* Command codes C2:C0, carried in bits 7:5 of the first byte */
#define MCP4716_CMD_WRITE_VOLATILE  (0x40U) /* 010: volatile configuration and DAC */
#define MCP4716_CMD_WRITE_ALL       (0x60U) /* 011: volatile memory and EEPROM */

/*
 * Read command response:
 *   byte 0: volatile status (RDY/BSY, POR) and configuration bits
 *   bytes 1:2: volatile DAC data, b15:b00
 *   byte 3: nonvolatile status and configuration bits
 *   bytes 4:5: nonvolatile DAC data, b15:b00
 */
#define MCP4716_READ_LEN            (6U)
#define MCP4716_STATUS_RDY          (0x80U)
#define MCP4716_STATUS_POR          (0x40U)
#define MCP4716_STATUS_NV           (0x20U)
#define MCP4716_STATUS_PD_MASK      (0x06U)

/* Configuration bit positions in the first byte of commands 2/3 */
#define MCP4716_REF_SHIFT           (3U)
#define MCP4716_PD_SHIFT            (1U)
#define MCP4716_GAIN_SHIFT          (0U)

/* Position of the 10-bit data inside the 16-bit data field of commands 2/3 */
#define MCP4716_DATA_SHIFT          (6U)

/* Time allowed for the device to leave reset, or to finish an EEPROM write, after power-up */
#define MCP4716_INIT_RETRY_CNT      (30)
#define MCP4716_INIT_RETRY_MS       (2)

/* EEPROM write time, datasheet maximum; the device ignores commands during this time */
#define MCP4716_EEPROM_WRITE_MS     (50)

/* Mapping from reference voltage enumeration values to register fields */
static const uint8_t ref_field[3] = {
    0x00U,      /* 0: VDD, unbuffered */
    0x02U,      /* 1: VREF pin, unbuffered */
    0x03U,      /* 2: VREF pin, buffered */
};

/* Mapping from power-down mode enumeration values to register fields */
static const uint8_t pd_field[4] = {
    0x00U,      /* 0: normal */
    0x01U,      /* 1: 1k to ground */
    0x02U,      /* 2: 125k to ground */
    0x03U,      /* 3: 640k to ground */
};

/* Cache of the volatile and nonvolatile state read or written by this driver */
static uint16_t s_code;
static uint16_t s_nv_code;
static uint8_t s_pd_mode;
static uint8_t s_nv_pd_mode;
static bool s_initted;

static uint16_t mv_to_code(uint32_t mv){
    uint32_t full_mv = (uint32_t) DAC_MCP4716_VREF_MV * DAC_MCP4716_GAIN;
    uint32_t code = (mv * 1024U + full_mv / 2U) / full_mv;

    if(code > DAC_MCP4716_CODE_MAX){
        code = DAC_MCP4716_CODE_MAX;
    }
    return (uint16_t) code;
}

static uint16_t code_to_mv(uint16_t code){
    uint32_t full_mv = (uint32_t) DAC_MCP4716_VREF_MV * DAC_MCP4716_GAIN;

    return (uint16_t) (((uint32_t) code * full_mv + 512U) / 1024U);
}

static uint16_t data_to_code(uint8_t high, uint8_t low){
    return (uint16_t) ((((uint16_t) high << 8) | low) >> MCP4716_DATA_SHIFT);
}

static uint8_t status_to_pd(uint8_t status){
    return (uint8_t) ((status & MCP4716_STATUS_PD_MASK) >> MCP4716_PD_SHIFT);
}

/*
 * Commands 2/3 write the configuration bits and a 16-bit data field:
 *   cmd: MCP4716_CMD_WRITE_VOLATILE (no EEPROM) or MCP4716_CMD_WRITE_ALL
 *   The 10-bit code value occupies b15:b06 of the data field.
 */
static int write_memory(uint8_t cmd, uint16_t code, uint8_t pd_mode){
    uint8_t buf[3];
    uint16_t data;
    int ret;

    buf[0] = cmd
           | (uint8_t) (ref_field[DAC_MCP4716_VREF_SEL] << MCP4716_REF_SHIFT)
           | (uint8_t) (pd_field[pd_mode] << MCP4716_PD_SHIFT)
           | (uint8_t) ((DAC_MCP4716_GAIN == 2 ? 1U : 0U) << MCP4716_GAIN_SHIFT);

    data = (uint16_t) (code << MCP4716_DATA_SHIFT);
    buf[1] = (uint8_t) (data >> 8);
    buf[2] = (uint8_t) (data & 0xFFU);

    if(iic_shard_lock() != 0){
        return -1;
    }
    ret = (iic_shard_write_block(DAC_MCP4716_I2C_ADDR, buf, sizeof(buf), false) == (int) sizeof(buf)) ? 0 : -1;
    iic_shard_unlock();
    return ret;
}

int dac_mcp4716_init(void){
    uint8_t buf[MCP4716_READ_LEN];
    int i;
    int ret;

    if(s_initted){
        return 0;
    }

    /* A read command acknowledges only when the device is present;
     * check the ready flag, POR flag and nonvolatile marker to validate the response */
    for(i = 0; i < MCP4716_INIT_RETRY_CNT; i++){
        ret = -1;
        if(iic_shard_lock() == 0){
            ret = iic_shard_read_block(DAC_MCP4716_I2C_ADDR, buf, sizeof(buf), false);
            iic_shard_unlock();
        }
        if((ret == (int) sizeof(buf)) &&
           ((buf[0] & MCP4716_STATUS_RDY) != 0U) &&
           ((buf[0] & MCP4716_STATUS_POR) != 0U) &&
           ((buf[3] & MCP4716_STATUS_NV) != 0U)){
            break;
        }
        sleep_ms(MCP4716_INIT_RETRY_MS);
    }
    if(i >= MCP4716_INIT_RETRY_CNT){
        LOG("mcp4716: read failed at 0x%02x\n", DAC_MCP4716_I2C_ADDR);
        return -2;
    }

    s_code = data_to_code(buf[1], buf[2]);
    s_pd_mode = status_to_pd(buf[0]);
    s_nv_code = data_to_code(buf[4], buf[5]);
    s_nv_pd_mode = status_to_pd(buf[3]);
    s_initted = true;

    LOG("mcp4716: %umV (code %u, pd %u)\n",
        (s_pd_mode == 0U) ? code_to_mv(s_code) : 0U, s_code, s_pd_mode);
    return 0;
}

int dac_mcp4716_set_mv(uint32_t mv, bool save_to_eeprom){
    uint16_t code;
    uint8_t pd_mode;
    uint8_t cmd;

    if(!s_initted){
        return -1;
    }

    if(mv == 0U){
        /* Power down while keeping the current DAC code value */
        code = s_code;
        pd_mode = DAC_MCP4716_PD_RESISTOR;
    } else {
        code = mv_to_code(mv);
        pd_mode = 0;
    }

    /* Skip the bus access when the volatile state and, if requested, the EEPROM state are current */
    if((code == s_code) && (pd_mode == s_pd_mode) &&
       (!save_to_eeprom || ((code == s_nv_code) && (pd_mode == s_nv_pd_mode)))){
        return 0;
    }

    cmd = save_to_eeprom ? MCP4716_CMD_WRITE_ALL : MCP4716_CMD_WRITE_VOLATILE;
    if(write_memory(cmd, code, pd_mode) != 0){
        return -1;
    }

    s_code = code;
    s_pd_mode = pd_mode;

    if(save_to_eeprom){
        s_nv_code = code;
        s_nv_pd_mode = pd_mode;
        /* The device ignores commands until the EEPROM write cycle finishes */
        sleep_ms(MCP4716_EEPROM_WRITE_MS);
    }
    return 0;
}

uint16_t dac_mcp4716_get_mv(void){
    if(s_pd_mode != 0U){
        return 0;
    }
    return code_to_mv(s_code);
}
