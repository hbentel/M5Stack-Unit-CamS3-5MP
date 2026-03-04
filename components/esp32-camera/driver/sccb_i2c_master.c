/*
 * This file is part of the OpenMV project.
 * Copyright (c) 2013/2014 Ibrahim Abdelkader <i.abdalkader@gmail.com>
 * This work is licensed under the MIT license, see the file LICENSE for details.
 *
 * SCCB (I2C-like) driver — new I2C master API (ESP-IDF 6.0+)
 *
 * Drop-in replacement for sccb.c. Selected at compile time by CMakeLists.txt
 * when IDF_VERSION_MAJOR >= 6. Implements the same public API (sccb.h) using
 * driver/i2c_master.h (handle-based) instead of the removed driver/i2c.h
 * (port/command-link-based).
 */
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include "sccb.h"
#include "sensor.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "driver/i2c_master.h"

static const char *TAG = "sccb";

#define LITTLETOBIG(x)      ((x<<8)|(x>>8))
#define SCCB_FREQ           CONFIG_SCCB_CLK_FREQ
#define SCCB_TIMEOUT_MS     1000

#if CONFIG_SCCB_HARDWARE_I2C_PORT1
const int SCCB_I2C_PORT_DEFAULT = 1;
#else
const int SCCB_I2C_PORT_DEFAULT = 0;
#endif

static i2c_master_bus_handle_t s_bus_handle = NULL;
static i2c_master_dev_handle_t s_dev_handle = NULL;
static bool                    s_owns_bus   = false;

int SCCB_Init(int pin_sda, int pin_scl)
{
    ESP_LOGI(TAG, "pin_sda %d pin_scl %d", pin_sda, pin_scl);
    ESP_LOGI(TAG, "sccb_i2c_port=%d", SCCB_I2C_PORT_DEFAULT);

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port                   = SCCB_I2C_PORT_DEFAULT,
        .sda_io_num                 = pin_sda,
        .scl_io_num                 = pin_scl,
        .clk_source                 = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt          = 7,
        .flags.enable_internal_pullup = true,
    };

    esp_err_t ret = i2c_new_master_bus(&bus_cfg, &s_bus_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %d", ret);
        return ret;
    }
    s_owns_bus = true;
    return ESP_OK;
}

/* Use a pre-initialized I2C bus (pin_sccb_sda == -1 path in esp_camera.c).
 * Retrieves the bus handle from the port number so the public signature is
 * unchanged from the legacy driver. */
int SCCB_Use_Port(int i2c_num)
{
    if (s_owns_bus && s_bus_handle) {
        SCCB_Deinit();
    }
    esp_err_t ret = i2c_master_get_bus_handle((i2c_port_num_t)i2c_num, &s_bus_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_get_bus_handle(%d) failed: %d", i2c_num, ret);
        return ret;
    }
    s_owns_bus = false;
    return ESP_OK;
}

int SCCB_Deinit(void)
{
    if (s_dev_handle) {
        i2c_master_bus_rm_device(s_dev_handle);
        s_dev_handle = NULL;
    }
    if (s_owns_bus && s_bus_handle) {
        i2c_del_master_bus(s_bus_handle);
        s_bus_handle = NULL;
        s_owns_bus = false;
    }
    return ESP_OK;
}

/* Probe all known sensor addresses. On success, register the device so
 * subsequent read/write calls have a handle to work with. */
uint8_t SCCB_Probe(void)
{
    uint8_t slave_addr = 0x0;

    for (size_t i = 0; i < CAMERA_MODEL_MAX; i++) {
        if (slave_addr == camera_sensor[i].sccb_addr) {
            continue;
        }
        slave_addr = camera_sensor[i].sccb_addr;

        if (i2c_master_probe(s_bus_handle, slave_addr, SCCB_TIMEOUT_MS) == ESP_OK) {
            i2c_device_config_t dev_cfg = {
                .dev_addr_length = I2C_ADDR_BIT_LEN_7,
                .device_address  = slave_addr,
                .scl_speed_hz    = SCCB_FREQ,
            };
            esp_err_t ret = i2c_master_bus_add_device(s_bus_handle, &dev_cfg, &s_dev_handle);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "i2c_master_bus_add_device failed: %d", ret);
                return 0;
            }
            return slave_addr;
        }
    }
    return 0;
}

/* Read 8-bit register (8-bit address) */
uint8_t SCCB_Read(uint8_t slv_addr, uint8_t reg)
{
    uint8_t data = 0;
    esp_err_t ret = i2c_master_transmit_receive(s_dev_handle,
                        &reg, 1, &data, 1, SCCB_TIMEOUT_MS);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SCCB_Read Failed addr:0x%02x, reg:0x%02x, ret:%d",
                 slv_addr, reg, ret);
        return 0xFF;
    }
    return data;
}

/* Write 8-bit register (8-bit address) */
int SCCB_Write(uint8_t slv_addr, uint8_t reg, uint8_t data)
{
    uint8_t buf[2] = {reg, data};
    esp_err_t ret = i2c_master_transmit(s_dev_handle, buf, sizeof(buf), SCCB_TIMEOUT_MS);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SCCB_Write Failed addr:0x%02x, reg:0x%02x, data:0x%02x, ret:%d",
                 slv_addr, reg, data, ret);
        return -1;
    }
    return 0;
}

/* Read 8-bit value from 16-bit register address */
uint8_t SCCB_Read16(uint8_t slv_addr, uint16_t reg)
{
    uint8_t data = 0;
    uint16_t reg_htons = LITTLETOBIG(reg);
    uint8_t *reg_u8 = (uint8_t *)&reg_htons;
    uint8_t tx[2] = {reg_u8[0], reg_u8[1]};

    esp_err_t ret = i2c_master_transmit_receive(s_dev_handle,
                        tx, sizeof(tx), &data, 1, SCCB_TIMEOUT_MS);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SCCB_Read16 [%04x] fail", reg);
        return 0xFF;
    }
    return data;
}

/* Write 8-bit value to 16-bit register address */
int SCCB_Write16(uint8_t slv_addr, uint16_t reg, uint8_t data)
{
    static uint16_t i = 0;
    uint16_t reg_htons = LITTLETOBIG(reg);
    uint8_t *reg_u8 = (uint8_t *)&reg_htons;
    uint8_t buf[3] = {reg_u8[0], reg_u8[1], data};

    esp_err_t ret = i2c_master_transmit(s_dev_handle, buf, sizeof(buf), SCCB_TIMEOUT_MS);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SCCB_Write16 [%04x]=%02x %d fail", reg, data, i++);
        return -1;
    }
    return 0;
}

/* Read 16-bit value from 16-bit register address.
 * Wire order: reg MSB, reg LSB, then data MSB, data LSB (big-endian).
 * Byte assignment mirrors the legacy driver:
 *   data_u8[1] ← first received byte (MSB)
 *   data_u8[0] ← second received byte (LSB) */
uint16_t SCCB_Read_Addr16_Val16(uint8_t slv_addr, uint16_t reg)
{
    uint16_t data = 0;
    uint8_t *data_u8 = (uint8_t *)&data;
    uint16_t reg_htons = LITTLETOBIG(reg);
    uint8_t *reg_u8 = (uint8_t *)&reg_htons;
    uint8_t tx[2] = {reg_u8[0], reg_u8[1]};
    uint8_t rx[2];

    esp_err_t ret = i2c_master_transmit_receive(s_dev_handle,
                        tx, sizeof(tx), rx, sizeof(rx), SCCB_TIMEOUT_MS);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SCCB_Read_Addr16_Val16 [%04x] fail", reg);
        return 0xFFFF;
    }
    data_u8[1] = rx[0]; /* MSB */
    data_u8[0] = rx[1]; /* LSB */
    return data;
}

/* Write 16-bit value to 16-bit register address */
int SCCB_Write_Addr16_Val16(uint8_t slv_addr, uint16_t reg, uint16_t data)
{
    uint16_t reg_htons  = LITTLETOBIG(reg);
    uint8_t *reg_u8     = (uint8_t *)&reg_htons;
    uint16_t data_htons = LITTLETOBIG(data);
    uint8_t *data_u8    = (uint8_t *)&data_htons;
    uint8_t buf[4] = {reg_u8[0], reg_u8[1], data_u8[0], data_u8[1]};

    esp_err_t ret = i2c_master_transmit(s_dev_handle, buf, sizeof(buf), SCCB_TIMEOUT_MS);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SCCB_Write_Addr16_Val16 [%04x]=%04x fail", reg, data);
        return -1;
    }
    return 0;
}
