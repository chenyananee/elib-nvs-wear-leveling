# elib-nvs-wear-leveling v3 设计稿：纯磨损均衡（read/write 两 op，介质一律当 EEPROM）

日期：2026-11-25
状态：设计定稿（尚未实现）
取代：`2026-10-27-elib-nvs-wear-leveling-design.md`（v2 追加式 + 整域擦）

## 0. 定位

本库**只做磨损均衡**：把"最新一条记录"往下一个槽轮转写、满了循环覆盖，写磨损
均摊到每个槽。数据合法性、掉电恢复不归本库。

## 1. 分层（与 safe-storage 的关系，因介质而异）

```
应用
  ↓
safe-storage（双区 + CRC）
  ↓
NVS（本库，只均衡）
  ↓
介质驱动（read / write，两 op）
```

- **flash（块擦）**：write 内 read-modify-write 可能整页丢失 → **safe-storage 必需**
  （双区兜底"上一条"）。
- **字节可写（EEPROM/FeRAM/MRAM）**：写覆盖字节级、不丢历史 → **safe-storage 可选**
  （只补 CRC/位翻转）。

## 2. 介质抽象

- ops 只有 `read`、`write`，**无 `erase`**。
- 库把**所有介质当 EEPROM**：不判空、不擦除、不管页、不管擦除极性。
- flash 的"擦 + 写"由**用户 write 内部** read-modify-write 抽象成 EEPROM 式写。
- 代价（文档化）：flash 磨损/延迟由用户承担；EEPROM/FeRAM 零代价。

## 3. 布局

```
管理区 = N 个槽，槽大小 slot_size（遍历步长），N = managed_size / slot_size
一个槽：
┌──────────────┬────────────┬──────────────────────────┬──────────┐
│ seq (4B)     │ magic (1B) │ 用户数据                  │ 填充      │
└──────────────┴────────────┴──────────────────────────┴──────────┘
  └── 库占用头部（5 字节，槽头 offset 0..4）──┘
  约束：5 + user_size ≤ slot_size
```

头部数组顺序 `[seq(4B)][magic(1B)]`，magic 是头部最后一字节。

## 4. 头部语义

- **magic（1B，0xA5）**：区分"写过 / 没写过"。擦成全 0（0x00）或全 1（0xFF）都
  ≠ 0xA5 → "未写"，擦除极性无关。
- **seq（4B）**：区分"谁更新"。单调递增，任意值合法（有效性由 magic 管），回绕用
  模序比较 `(int32_t)(a - b) > 0`。

## 5. 写流程

```
目标槽 = (newest + 1) mod N
① 写用户数据（offset 5..）
② 写头部数组 [seq][magic]（offset 0..4）——一次 write，magic 殿后
```

- 每写一条 = 2 次 write；头部在 5 字节栈缓冲组好，无动态内存。
- 轮转覆盖旧槽，库不擦除。
- 前提（文档化）：用户 write 按数组顺序落字节（或保证 magic 最后生效）。

## 6. 读流程

读最新槽的用户数据（offset 5 起）返回；空域返回 `ELIB_NVS_ERR_EMPTY`。

## 7. 定位流程（init / reset，全槽遍历）

```
扫全部槽：magic == 0xA5 → 候选；newest = 候选里 seq 模序最大者；无候选 → 空域
写位置 = (newest + 1) mod N
```

**全槽遍历、不提前停**：回绕后撕裂可能发生在槽 0，其后还有有效旧记录，提前停
会误判空域。

## 8. 轮转与磨损

写指针单调前进、到尾回绕；每次写覆盖最旧槽，写磨损均摊到 N 槽，无需磨损表。

## 9. 掉电安全（按介质分两类）

- **字节可写（EEPROM/FeRAM/MRAM）**：写是字节级覆盖，掉电只伤"正在写的槽"，
  其余槽（含上一条）无损。本库"数据先写、头部后写"协议**自身保证**：掉电后要么
  读到本次完整记录、要么回退上一条完整记录，不丢历史、不返回撕裂数据。
- **flash（块擦）**：用户 write 内 read-modify-write 擦 + 重写整页，掉电可能整页
  丢失，同页"上一条"一并丢 → **必须 safe-storage 双区兜底**；本库只保证"能定位"。

## 10. 配置与约束

```
start_addr, managed_size, slot_size, user_size
N = managed_size / slot_size
```

- `5 + user_size ≤ slot_size`
- `N ≥ 2`（目标槽 ≠ 最新槽）
- 无页对齐、无 `erase_page_size`（页概念已下推进用户 write）。

## 11. 边界情况

| 情况 | 行为 |
|---|---|
| 空域 | read → EMPTY；write → 槽 0 |
| N=1 | 每次写覆盖唯一槽（字节可写由字节原子性保证；flash 由 safe-storage 兜） |
| seq 回绕 | 模序比较兜底 |
| 头部撕裂 | magic 无效 → 跳过 |
| 擦除极性 0 / 1 | magic 0xA5 与两者都不同 |

## 12. 对外 API（概念）

```
init(ctx, ops, start_addr, managed_size, slot_size, user_size)
read(ctx, buf) / write(ctx, buf) / reset(ctx)
```

- `ops = { read, write, user_data }`（无 erase）。
- 错误码：OK / INVALID_PARAM / NOT_INITIALIZED / IO / EMPTY。
- 删除 `erase_all`（库无擦除能力）。

## 13. 不做什么

不做 CRC/撕裂/位翻转；不做多记录/目录/磨损表/坏块管理/NAND；不做 ISR 安全
（介质 op 并发由用户保证）。

## 14. README 风险告知与配合建议（必写）

1. 本库**不保证数据完整性**：无 CRC、无撕裂恢复、无位翻转防护。
2. **掉电风险按介质分**：flash（read-modify-write 可能整页丢）→ **必配 safe-storage
   双区**；字节可写介质本库自身保证"回退上一条"，风险低。
3. flash 用户**必须自己在 `write` 里实现 read-modify-write**：磨损放大、写延迟自担。
4. **管理区只能由库操作**（禁止库外读写）。
5. **介质 op 并发（含 ISR）由用户保证**。
6. 配置约束：`N ≥ 2`、`5 + user_size ≤ slot_size`。
7. 搭配建议：flash 必配 safe-storage（双区 + CRC）；字节可写可选。

## 交付物与验收

**交付物**：
- 本设计稿落盘 `docs/superpowers/specs/`（替换旧 v2 稿，不写代码）。
- README（中英）新增"风险告知与 safe-storage 配合"章节，内容对齐 §14。

**验收要点**：
1. 布局/头部/约束与本文一致。
2. 定位 = 全槽遍历 + 最大 seq（通过"槽 0 撕裂、其余有效"用例）。
3. 写 = "用户数据 → 头部数组"两次 write，无擦除、无判空、无页概念。
4. 掉电矩阵：字节可写任意截断后 read ∈ {上一条, 本次} 或 EMPTY，绝不返回撕裂数据；
   flash 定位仍正确、完整性交 safe-storage。
5. README 风险点与 §14 完全对齐。
