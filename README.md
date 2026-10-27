# elib-nvs-wear-leveling

面向 flash / EEPROM 类介质的**轻量磨损均衡库**：不信 CRC、不做磨损表、不建目录树——
它只做一件事：**把"最新一条记录"追加式地写到管理域的下一个单元里，写满全域擦除后
从头再来**，擦除磨损由此天然均摊到管理域的每一个页面。

与 [elib-safe-storage](https://gitee.com/chenyananee/elib-safe-storage)（若已发布）分层配合：
本库只保证"定位 + 顺序写 + 循环擦"，**数据完整性（撕裂写、CRC）不在本库范围**——
撕裂写读回什么就是什么，完整性由上一层的 safe-storage 兜底。

## 特性

- **零动态内存**：上下文由用户分配，库内无任何 static / 堆缓冲（连判空缓冲都没有）
- **C99**，单头文件 + 单实现文件，拷进工程即用
- **隐式磨损均衡**：追加写 + 全域循环擦，无需磨损表、无需坏块管理
- **判空外置**：介质是否为空由用户 op（`is_empty`）判定——flash 用户可用 mmio 比对、
  EEPROM 用户可自选轮询或后台扫描，库侧零 RAM 占用
- **惰性擦除**：全域擦除只在"下一次写入找不到合法槽位"时发生，最后一个单元的
  数据在被下一次写入替换之前一直可读，**零数据浪费**
- **单记录模型**：只存"最新一条"，历史记录由擦除自然回收

## 目录结构

```
include/elib_nvs.h     唯一头文件（错误码 + ops + ctx + 5 个 API）
src/elib_nvs_core.c    唯一实现文件
test/test_elib_nvs.c   RAM 模拟介质用例（随库编译检查）
```

没有驱动模板文件：你的介质驱动就是本文 §接入 下面那 4 个函数，照着写即可。

## 快速上手

### 第 1 步：实现你的 4 个介质操作

```c
#include "elib_nvs.h"

/* 库对每个 op 的约定：返回 0 = 成功；非 0 = 你自己的错误码（库转报 ELIB_NVS_ERR_IO）*/

static int my_read(void *ud, uint32_t addr, uint8_t *buf, uint32_t len) {
    /* 从 addr 起读 len 字节到 buf；memcpy / DMA / 驱动调用均可 */
    return 0;                                   /* 0 = 成功 */
}

static int my_write(void *ud, uint32_t addr, const uint8_t *buf, uint32_t len) {
    /* 向 addr 写入 len 字节（库只会写到"已确认为空"的单元，flash 无须先擦） */
    return 0;
}

static int my_erase(void *ud, uint32_t addr, uint32_t len) {
    /* 擦除 [addr, addr+len)：调用点只有两处——
     *   a) 全域一次擦（addr = start，len = managed_size）
     *   b) elib_nvs_erase_all 同样恒为全域擦
     * 所以最小实现只需支持"整域擦"即可；
     * erase_page_size 在这里的意义是 init 校验对齐用的，不单独驱动擦调用 */
    return 0;
}

static uint8_t my_is_empty(void *ud, uint32_t addr, uint32_t len) {
    /* 关键约定：[addr, addr+len) 按你自己的"空"定义判空——
     * 典型实现：每个 byte 都等于擦除后的值（如 0xFF）才返回 1，否则返回 0。
     * 库不读任何记录内容，所以"空"的定义完全由你说了算；
     * 但基准必须与你的 my_erase 擦出来的值一致——不然掉电重定位会钉错位置 */
    return 1;  /* 按介质最优方式实现：mmio 直接比对 / 字宽扫 / 驱动辅助皆可 */
}

const elib_nvs_ops_t my_ops = {
    .user_data = &my_hw_ctx,   /* 原样转发给你的每个 op */
    .read      = my_read,
    .write     = my_write,
    .erase     = my_erase,
    .is_empty  = my_is_empty,
};
```

### 第 2 步：分配上下文并初始化

```c
static elib_nvs_ctx_t nvs_ctx;   /* 用户静态分配，库不再申请任何内存 */

#define RECORD_SIZE       64u      /* 每条记录的定长字节数（单元大小） */
#define ERASE_PAGE_SIZE   4096u    /* 介质最小擦除页 */
#define MANAGED_SIZE      (ERASE_PAGE_SIZE * 16u)  /* 管理域大小 */

void app_setup(void) {
    elib_nvs_err_t err = elib_nvs_init(&nvs_ctx, &my_ops,
                                       0x00100000,    /* start_addr，按擦除页对齐 */
                                       MANAGED_SIZE,  /* 管理域大小，页整数倍 */
                                       ERASE_PAGE_SIZE,
                                       RECORD_SIZE);
    /* err 非 ELIB_NVS_OK 时按 §校验清单 排查 */
}
```

### 第 3 步：读写

```c
static uint8_t latest[RECORD_SIZE];

/* 读最新一条（全域还没写过任何记录时返回 ELIB_NVS_ERR_EMPTY） */
err = elib_nvs_read(&nvs_ctx, latest);

/* 追加一条新记录（自动前进一格；写满时先惰性全域擦除再从头写） */
err = elib_nvs_write(&nvs_ctx, new_record);

/* 重扫描读/写位置（不做任何物理擦除；一般不用，主要在介质内容被外部工具动过
 * 之后你想让库"重新读局面"时使用） */
err = elib_nvs_reset(&nvs_ctx);

/* 主动物理擦除整个管理域，回到空介质状态 */
err = elib_nvs_erase_all(&nvs_ctx);
```

## 工作原理

### 数据单元与追加式写

管理域被划分为 `N = managed_size / unit_size` 个**数据单元**（unit_size 可跨擦除页，
不要求整除；`managed_size / unit_size` 的零头永远闲置）。每次 `elib_nvs_write`
把一个定长单元写到"写指针"处，然后：

- 后面相邻单元已确认全空 → 写指针指到它；
- 后面只剩零头 / 相邻非空 → 写指针卷回**头（unit 0）**。

这样一场擦写周期是 `0, 1, 2, …, N-1` 的顺序填充，每轮都用上了全域页面，**擦除
磨损天然均摊**，无需磨损表。

### 三分支定位（同一个 seek 谓词，init / reset / write 三处共用）

seek 从给定单元开始向后找第一个"合法槽位"：

1. **常规态** `(k, k+1)` 双连续全空（2×unit 窗口）→ 槽位 = k；
   扫描版本：第一个这样的 k > 0 → `read = k-1`（最新数据），`write = k`
2. **尾单槽**：k 是最后一个单元且为空 → 槽位 = k
   （"空间不足两个时一个也合法"，保证最后一个单元不浪费）
3. **写满态**：滑到头仍无槽位 → `read = N-1`（尾部最后单元 = 最新数据）、
   `write = 头`；下一次写入的预判在这里触发全域擦除

全空新介质 → 无读位置（`elib_nvs_read` 返 `ELIB_NVS_ERR_EMPTY`）、写指针在头。

### 为什么"双连续全空"是实现裸命

要求 `(k, k+1)` 双空而不是只看 `k` 一个，是为了让**任何时刻的断电重启都能钉对
位置**：写指针落点之前经过的每一扇窗要么双空成了落点、要么含非空单元（外部
篡改的孤儿数据也算）而"被封死"。重启扫描用同一谓词滑窗，孤儿所在的窗必被判
非法、绕过——**扫描永远钉在真实最新数据的后面**，无需任何磁盘标记，也不需要
回写什么脏数据。

**库外直接篡改管理域内容 = 未定义行为**。运行时态由窗口 seek 自愈（非法窗绕过
→ 写满态收敛到"读尾单元"），但不承诺恢复出"正确的那条"记录。

### n == 1 退化形态

域只容一个单元：`lib_nvs_write` 每次都走"写满 → 惰性全域擦除 → 从头写"，等价于
一个滚动单槽。

寿命模型：总写入寿命 ≈ `N × 单页擦除寿命`（每次全域擦严均磨损所有页面一次）。

## 校验清单（init 拒绝即 `ELIB_NVS_ERR_INVALID_PARAM`）

- `ctx`、`ops` 非空，`ops` 内 `read/write/erase/is_empty` 四个函数指针全非空
- `erase_page_size != 0`，`unit_size != 0`
- `start_addr % erase_page_size == 0`（起始地址按擦除页对齐）
- `managed_size % erase_page_size == 0`，且 `managed_size >= erase_page_size`
- `1 <= unit_size <= managed_size`
- `start_addr + managed_size` 不得越过 32 位地址空间（回绕即拒绝）

## API 一览

| 函数 | 行为 |
|---|---|
| `elib_nvs_init` | 校验配置、扫描定位读/写位置 |
| `elib_nvs_read` | 读最新一条（恒定 unit_size 字节） |
| `elib_nvs_write` | 追加一条；写满时先惰性全域擦除再从头写 |
| `elib_nvs_reset` | 重新扫描定位读/写位置，不做物理擦除 |
| `elib_nvs_erase_all` | 物理擦除整个管理域，回到空介质状态 |

返回值：`ELIB_NVS_OK` / `ELIB_NVS_ERR_INVALID_PARAM` / `ELIB_NVS_ERR_NOT_INITIALIZED`
/ `ELIB_NVS_ERR_IO`（你的 op 返回了非 0）/ `ELIB_NVS_ERR_EMPTY`（全域还没有记录）。

## RAM 占用

- **flash（代码）**：两个文件的实现体，极小
- **RAM**：仅 `elib_nvs_ctx_t`（约 40 字节，32 位平台上含 ops 4 指针 + user_data
  + 配置 5 字段 + 运行时状态）——由**你**分配，库零内存
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
elib-nvs-wear-leveling  ← 追加写 / 磨损均衡 / 循环擦除
  │
  ▼
你的介质驱动（read / write / erase / is_empty 四个 op）
```

## 集成建议

1. 把 `include/`、`src/` 拷进工程（或作为子模块引用）。
2. 按上文实现 4 个 op（本库不提供驱动模板，方法全在本文）。
3. 编译选项示例：`-std=c99 -Wall`（可在 `-Werror -Wextra` 下干净编译）。
4. `test/test_elib_nvs.c` 用 RAM 数组模拟介质——直接把它连同库编进你方便的
   宿主/目标工具链即可在真实运行时中检验正确性。
