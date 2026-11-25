**简体中文** | [English](README.en.md)

# elib-nvs-wear-leveling

面向 flash / EEPROM 类介质的**轻量磨损均衡库**：它只做一件事——**把"最新一条记录"
写到管理域的下一个槽里，写到末尾回绕覆盖最旧的槽**，写磨损由此天然均摊到管理域的
每一个槽。

库把**所有介质都当成字节可写的 EEPROM**：只有 `read` / `write` 两个介质操作，
**不擦除、不判空、不管页**。需要"擦后才能写"的介质（NOR flash）由用户在 `write`
内部用 read-modify-write 抽象成 EEPROM 式写。

## 特性

- **零动态内存**：上下文由用户分配，库内无任何 static / 堆缓冲
- **C99**，单头文件 + 单实现文件，拷进工程即用
- **隐式磨损均衡**：轮转写 + 循环覆盖，无需磨损表、无需坏块管理
- **介质操作极简**：只有 `read` / `write` 两个 op，擦除与判空全部外置到用户的 `write`
- **序号定位**：每条记录带 32 位序号，上电全槽遍历取最大序号
- **擦除极性无关**：用 `magic = 0xA5` 标记"写过"，擦成 0x00 或 0xFF 都判为"未写"
- **单记录模型**：只存"最新一条"，历史记录由轮转覆盖自然回收

## 风险告知与搭配建议

本库只做磨损均衡，**不保证数据完整性**。使用前请明确以下风险：

1. **不做 CRC / 撕裂恢复 / 位翻转防护**：介质上写什么就读回什么；正在写的那一条若
   掉电撕裂，重启后可能被当作有效记录读回。
2. **掉电风险按介质分**：
   - **字节可写（EEPROM / FeRAM / MRAM）**：写覆盖只影响"正在写的那一格"，
     **历史记录（上一条及更早）完好无损**，风险低；
   - **flash（块擦）**：擦除 / 写回可能**整页丢失**，同页的历史记录一并丢失，风险高
     → **必须搭配 elib-safe-storage 双区备份兜底**。
3. **管理区只能由本库操作**：禁止在库外直接读写管理区（外部篡改 = 未定义行为）。
4. **介质 op 的并发安全（含 ISR）由用户保证**：库不做加锁。
5. 配置须通过 `elib_nvs_init` 校验，见 §校验清单。

**搭配建议**：flash 介质**必须**搭配 [elib-safe-storage](https://gitee.com/chenyananee/elib-safe-storage)
（双区 + CRC）使用；字节可写介质**建议**搭配（至少补上 CRC / 撕裂校验，防止撕裂记录
被误用）。

## 目录结构

```
include/elib_nvs.h      唯一头文件（错误码 + ops + ctx + 4 个 API）
src/elib_nvs_core.c     唯一实现文件
test/test_elib_nvs.c    RAM 模拟介质用例（随库编译检查）
docs/superpowers/specs/ v3 设计稿
```

没有驱动模板文件：你的介质驱动就是下面那 2 个函数，照着写即可。

## 快速上手

### 第 1 步：实现你的 2 个介质操作

```c
#include "elib_nvs.h"

/* 约定：返回 0 = 成功；非 0 = 你自己的错误码（库转报 ELIB_NVS_ERR_IO）*/

static int my_read(void *ud, uint32_t addr, uint8_t *buf, uint32_t len) {
    /* 从 addr 起读 len 字节到 buf；memcpy / DMA / 驱动调用均可 */
    return 0;                                   /* 0 = 成功 */
}

static int my_write(void *ud, uint32_t addr, const uint8_t *buf, uint32_t len) {
    /* 把 len 字节写到 addr。
     * EEPROM：直接写即可。
     * NOR flash：必须在这里做 read-modify-write——读出所在页、合并新内容、
     *   擦除该页、整页写回。库不会替你擦。 */
    return 0;
}

const elib_nvs_ops_t my_ops = {
    .user_data = &my_hw_ctx,   /* 原样转发给你的每个 op */
    .read      = my_read,
    .write     = my_write,
};
```

### 第 2 步：分配上下文并初始化

```c
static elib_nvs_ctx_t nvs_ctx;   /* 用户静态分配，库不再申请任何内存 */

#define USER_SIZE    64u     /* 用户数据字节数 */
#define SLOT_SIZE    80u     /* 槽大小：5 字节头部 + 64 用户 + 11 填充 */
#define SLOT_COUNT   8u
#define MANAGED_SIZE (SLOT_SIZE * SLOT_COUNT)

void app_setup(void) {
    elib_nvs_err_t err = elib_nvs_init(&nvs_ctx, &my_ops,
                                       0x00100000,    /* start_addr，无页对齐要求 */
                                       MANAGED_SIZE,
                                       SLOT_SIZE,
                                       USER_SIZE);
    /* err 非 ELIB_NVS_OK 时按 §校验清单 排查 */
}
```

### 第 3 步：读写

```c
static uint8_t latest[USER_SIZE];

/* 读最新一条（全域还没写过任何记录时返回 ELIB_NVS_ERR_EMPTY） */
err = elib_nvs_read(&nvs_ctx, latest);

/* 追加一条：写到下一个槽，到末尾回绕覆盖最旧槽 */
err = elib_nvs_write(&nvs_ctx, new_record);

/* 重新扫描定位（不做任何介质写；一般不用） */
err = elib_nvs_reset(&nvs_ctx);
```

## 工作原理

### 槽布局与头部

管理域被划分为 `N = managed_size / slot_size` 个**槽**，每个槽 `slot_size` 字节：

```
┌──────────────┬────────────┬──────────────────────┬──────────┐
│ seq (4B)     │ magic (1B) │ 用户数据 user_size  │ 填充      │
└──────────────┴────────────┴──────────────────────┴──────────┘
  └── 库占用头部（5 字节，在槽头）──┘
  约束：5 + user_size <= slot_size
```

- `seq`：32 位单调序号，用来判断"谁最新"；
- `magic`：`0xA5` 表示这槽写过；擦成全 0（0x00）或全 1（0xFF）都 ≠ 0xA5，
  所以**擦除极性无关**；
- 填充不写不读，恒保持擦除值。

### 写流程

```
目标槽 = (最新槽 + 1) mod N
① 写用户数据（槽内 offset 5 起）
② 写头部数组 [seq][magic]（槽头 offset 0..4），一次写、magic 殿后
```

magic 是**提交点**：头部写一半掉电时 magic 还没落盘，这槽判为"未写"，定位时跳过。

### 定位（init / reset，全槽遍历）

扫全部槽：`magic == 0xA5` 的才是候选，取其中 `seq` 最大者（模序比较，回绕安全）；
一个候选都没有 → 空域。

**必须全槽遍历，不能碰到无效头就停**：回绕后撕裂可能发生在槽 0，其后还有有效旧记录，
提前停会误判成空域。

### 轮转与磨损

写指针单调前进、到尾回绕；每次写覆盖最旧的槽，写磨损均摊到 N 个槽，无需磨损表。

### 掉电安全（按介质分）

- **字节可写（EEPROM / FeRAM / MRAM）**：写是字节级覆盖，掉电只伤"正在写的槽"，
  其余槽（含上一条）无损。本库"数据先写、头部后写"的协议自身保证：掉电后要么读到
  本次完整记录、要么回退上一条完整记录。
- **flash（块擦）**：用户 `write` 内的 read-modify-write 可能整页丢失，同页的历史
  记录一并丢，必须由 safe-storage 双区兜底；本库只保证"能定位"。

### 序号回绕

32 位序号用模序比较 `(int32_t)(a - b) > 0` 判"更新"，存活槽数 << 2^31，回绕天然正确。

## 校验清单（init 拒绝即 `ELIB_NVS_ERR_INVALID_PARAM`）

- `ctx`、`ops` 非空，`ops` 内 `read` / `write` 两个函数指针全非空
- `user_size != 0`
- `5 + user_size <= slot_size`（头部 + 用户数据装得进一个槽）
- `N = managed_size / slot_size >= 2`
- `start_addr + managed_size` 不得越过 32 位地址空间（回绕即拒绝）

## API 一览

| 函数 | 行为 |
|---|---|
| `elib_nvs_init` | 校验配置、全槽扫描定位最新记录 |
| `elib_nvs_read` | 读最新一条（恒定 user_size 字节） |
| `elib_nvs_write` | 追加一条；写到下一个槽，末尾回绕覆盖最旧槽 |
| `elib_nvs_reset` | 重新扫描定位，不做介质写 |

返回值：`ELIB_NVS_OK` / `ELIB_NVS_ERR_INVALID_PARAM` / `ELIB_NVS_ERR_NOT_INITIALIZED`
/ `ELIB_NVS_ERR_IO`（你的 op 返回了非 0）/ `ELIB_NVS_ERR_EMPTY`（全域还没有记录）。

## RAM 占用

- **flash（代码）**：两个文件的实现体，极小
- **RAM**：仅 `elib_nvs_ctx_t`（约 40 字节，32 位平台上含 ops 2 指针 + user_data
  + 配置 4 字段 + 运行时状态）——由**你**分配，库零内存
- **栈**：无深递归、无大缓冲，单函数栈深

## 与 elib-safe-storage 的分层

本库不做完整性：写到介质上的是什么内容，读回来就是什么——**撕裂写、位翻转、
CRC 校验都在这一层之外**。把任意"记录"包在 safe-storage 的信封里再用本库持久
化，即可得到完整性与持久性"两全"的存储栈：

```
应用层
  │  写入记录
  ▼
elib-safe-storage  ← 撕裂写 / CRC / 恢复
  │  安全载荷
  ▼
elib-nvs-wear-leveling  ← 顺序写 / 磨损均衡 / 循环覆盖
  │
  ▼
你的介质驱动（read / write 两个 op）
```

## 集成建议

1. 把 `include/`、`src/` 拷进工程（或作为子模块引用）。
2. 按上文实现 2 个 op（本库不提供驱动模板，方法全在本文）。
3. 编译选项示例：`-std=c99 -Wall`（可在 `-Werror -Wextra` 下干净编译）；
   MSVC 遇到中文字符需加 `/utf-8`。
4. `test/test_elib_nvs.c` 用 RAM 数组模拟介质——直接把它连同库编进你方便的
   宿主/目标工具链即可在真实运行时中检验正确性。
