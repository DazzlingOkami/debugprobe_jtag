#ifndef _IIC_SHARD_H
#define _IIC_SHARD_H
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>

/* Maximum wait for the bus lock; on timeout it returns failure instead of blocking forever */
#define IIC_SHARD_LOCK_TIMEOUT_MS   1000

/**
 * @brief  Initialize the IIC1 bus and the bus lock
 * @return 0 on success; -1 on failure
 */
int iic_shard_init(void);

/**
 * @brief  Release the IIC1 bus and the bus lock
 * @return 0 on success; -1 on failure
 */
int iic_shard_deinit(void);

/**
 * @brief  Acquire the IIC bus lock
 * @return 0 on success; -1 on timeout or if not initialized
 */
int iic_shard_lock(void);

/**
 * @brief  Release the IIC bus lock
 * @return 0 on success; -1 if not initialized
 */
int iic_shard_unlock(void);

/**
 * @brief  Write data to an IIC slave
 * @param  [in] addr   7-bit slave address
 * @param  [in] src    Data to write
 * @param  [in] len    Data length
 * @param  [in] nostop true to omit the stop bit after the transfer (for repeated start)
 * @return Number of bytes written; a negative value is an error code
 */
int iic_shard_write_block(uint8_t addr, const uint8_t *src, size_t len, bool nostop);

/**
 * @brief  Read data from an IIC slave
 * @param  [in]  addr 7-bit slave address
 * @param  [out] dst  Receive buffer
 * @param  [in]  len  Read length
 * @param  [in]  nostop true to omit the stop bit after the read
 * @return Number of bytes read; a negative value is an error code
 */
int iic_shard_read_block(uint8_t addr, uint8_t *dst, size_t len, bool nostop);

#endif
