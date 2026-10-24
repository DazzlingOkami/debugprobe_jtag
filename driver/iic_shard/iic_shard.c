#include "iic_shard.h"
#include <hardware/i2c.h>
#include <hardware/gpio.h>
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "dap_board.h"

static SemaphoreHandle_t iic_lock;

int iic_shard_init(void){
    /* Use a mutex rather than a counting semaphore to get priority inheritance */
    iic_lock = xSemaphoreCreateMutex();
    if(iic_lock == NULL){
        return -1;
    }

    i2c_init(IIC_INTERFACE, 400 * 1000);
    gpio_set_function(IIC_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(IIC_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(IIC_SDA_PIN);
    gpio_pull_up(IIC_SCL_PIN);
    return 0;
}

int iic_shard_deinit(void){
    i2c_deinit(IIC_INTERFACE);
    gpio_deinit(IIC_SDA_PIN);
    gpio_deinit(IIC_SCL_PIN);
    if(iic_lock != NULL){
        vSemaphoreDelete(iic_lock);
        iic_lock = NULL;
    }
    return 0;
}

int iic_shard_lock(void){
    if(iic_lock == NULL){
        return -1;
    }
    return (xSemaphoreTake(iic_lock, pdMS_TO_TICKS(IIC_SHARD_LOCK_TIMEOUT_MS)) == pdTRUE) ? 0 : -1;
}

int iic_shard_unlock(void){
    if(iic_lock == NULL){
        return -1;
    }
    return (xSemaphoreGive(iic_lock) == pdTRUE) ? 0 : -1;
}

int iic_shard_write_block(uint8_t addr, const uint8_t *src, size_t len, bool nostop){
    return i2c_write_blocking(IIC_INTERFACE, addr, src, len, nostop);
}

int iic_shard_read_block(uint8_t addr, uint8_t *dst, size_t len, bool nostop){
    return i2c_read_blocking(IIC_INTERFACE, addr, dst, len, nostop);
}
