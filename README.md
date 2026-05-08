# elib-nvs-wear-leveling

Flash/EEPROM wear leveling library for embedded systems.

## Features

- **跨扇区写入支持** - 数据可跨越扇区边界，自动查找连续空闲空间
- **简化空间管理** - 纯按可用空间管理，无需磨损表
- **零配置文件** - 所有参数由用户初始化时传入，无硬编码配置
- **零动态内存** - 用户分配所有资源，无 malloc/free
- **插件式驱动** - 支持 SPI Flash、I2C EEPROM 等多种介质
- **MTD 线性接口** - 简单的线性地址读写 API
- **掉电恢复** - 重启后自动扫描恢复写入位置

## Wear Leveling Principle

本库采用**动态磨损均衡**策略，延长 Flash/EEPROM 存储介质寿命。

### 基本原理

Flash/EEPROM 的每个擦除单元（扇区/块）有擦除次数限制（典型值：SLC Flash 约 10 万次，EEPROM 约 100 万次）。当某个块达到寿命上限后，继续写入会导致数据不可靠。

本库通过以下机制实现磨损均衡：

```
管理区布局：
┌─────────────┬─────────────┬─────────────┬─────────────┬─────────────┬─────────────┐
│  Block 0    │  Block 1    │  Block 2    │  Block 3    │  Block 4    │  Block 5    │
│  (4KB)      │  (4KB)      │  (4KB)      │  (4KB)      │  (4KB)      │  (4KB)      │
└─────────────┴─────────────┴─────────────┴─────────────┴─────────────┴─────────────┘
│                                        │
│← 可用空间扫描 →                        │
│                                        ↓
│                               下一次写入位置
```

### 写入策略

1. **首次写入** → 从起始位置（Block 0）开始
2. **连续写入** → 按 `data_unit_size` 对齐，依次向后写入
3. **空间不足** → 擦除所有块，从 Block 0 重新开始

### 扫描算法

```c
// 从前往后扫描，找到第一个空闲位置
for (offset = 0; offset < total_size; offset += data_unit_size) {
    if (is_region_free(offset)) {
        // 找到第一个空闲位置，前一个位置是最后写入的数据
        last_write_offset = offset - data_unit_size;
        next_write_offset = offset;
        break;
    }
}
```

**优势：**
- 无需磨损表，降低内存开销
- 启动扫描一次即可定位写入/读取位置
- 数据自然分散到不同物理块，实现磨损均衡

### 寿命计算示例

假设配置：
- 总空间：12KB（3 个 4KB 块）
- 写入单位：5KB
- Flash 擦除寿命：10 万次

```
单块寿命：100,000 次擦除
写入分散到 3 个块，系统总寿命 ≈ 3 × 100,000 = 300,000 次写入
```

---

## Quick Start

```c
#include "elib_nvs.h"

/* 1. 定义上下文 (静态分配) */
static elib_nvs_ctx_t g_ctx;

/* 2. 实现 HAL 驱动 */
static elib_nvs_hal_driver_t g_driver = {
    .user_data = &my_hw,
    .read = my_read,
    .write = my_write,
    .erase = my_erase,
};

/* 3. 初始化 (V3 API - 无配置，参数由用户传入，移除了 resources 参数) */
elib_nvs_err_t err = elib_nvs_init(
    &g_ctx, 
    &g_driver, 
    0x08000000,     /* start_addr: Flash 起始地址 */
    12 * 1024,      /* total_size: 12KB 总空间 */
    4 * 1024,       /* block_size: 4KB 扇区 */
    5 * 1024        /* data_unit_size: 5KB 写入单位 */
);
if (err != ELIB_NVS_OK) {
    /* 错误处理 */
}

/* 4. 使用标准 API (基于地址) */
err = elib_nvs_write(&g_ctx, 0x1000, data, sizeof(data));  /* 在地址 0x1000 处写入 */
err = elib_nvs_read(&g_ctx, 0x1000, buf, sizeof(buf));    /* 从地址 0x1000 读取 */

/* 5. 使用 MTD API (无需指定地址，自动管理 - 推荐用于简单场景) */
err = elib_nvs_mtd_write(&g_ctx, data, sizeof(data));     /* 写入到下一个可用位置 */
err = elib_nvs_mtd_read(&g_ctx, buf, sizeof(buf));        /* 自动读取最后一次写入的数据 */
err = elib_nvs_mtd_erase(&g_ctx, 0, 12 * 1024);          /* 全擦除 */

/* 6. 获取用户数据 */
void *user_data = elib_nvs_get_user_data(&g_ctx);

/* 7. 反初始化 */
elib_nvs_deinit(&g_ctx);
```

## API Reference

### Lifecycle
- `elib_nvs_init(ctx, driver, start_addr, total_size, block_size, data_unit_size)` - 初始化 NVS 实例 (V3 API - 纯运行时参数化，移除了resources参数)
- `elib_nvs_deinit(ctx)` - 反初始化 NVS 实例

### Standard Operations (Address-based)
- `elib_nvs_write(ctx, addr, data, len)` - 在指定逻辑地址处写入数据
- `elib_nvs_read(ctx, addr, out, len)` - 从指定逻辑地址读取数据
- `elib_nvs_erase(ctx, addr, size)` - 擦除指定逻辑地址范围

### MTD Operations (Address-free, Recommended)
- `elib_nvs_mtd_write(ctx, data, len)` - 写入数据到下一个可用位置 (MTD模式)
- `elib_nvs_mtd_read(ctx, out, len)` - 读取最近一次写入的数据 (MTD模式)
- `elib_nvs_mtd_erase(ctx, addr, size)` - 擦除指定位置 (MTD模式)

### Utility Functions
- `elib_nvs_get_user_data(ctx)` - 获取与上下文关联的用户数据指针

## See Also

- [Driver Template](drivers/driver_template.c) - HAL 驱动开发模板

## License

MIT License
