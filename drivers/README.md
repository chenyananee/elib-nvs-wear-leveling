# NVS HAL Driver Development Guide

## 驱动开发步骤

1. 复制 `driver_template.c` 为你的驱动文件 (如 `flash_spi_driver.c`)
2. 填充 `my_storage_read/write/erase/get_info` 函数
3. 注册驱动到 NVS 核心

## 支持的设备类型

### SPI NOR Flash
- 典型擦除大小：4KB
- 寿命：约 10 万次擦除

### I2C EEPROM
- 典型擦除大小：1-256 字节
- 寿命：约 10 万 -100 万次擦除
