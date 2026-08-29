# 交易所（Exchange）封包研究

> 來源：GB 客戶端 `SoulWorker64.dll`（revision **511199**，檔案日期 2026-08-24，MD5 `fa67afcd59eae28e614928e2d1eb5407`）。
> 方法：純靜態分析（Python + capstone），沒有抓任何實際流量；欄位型別與順序來自客戶端自己的
> 序列化 / 反序列化函式，**欄位名稱**則是從 UI 程式碼的用法推回來的，未經實際封包驗證的欄位以「?」標示。
> 完整的收/送 opcode 對照表（全部命令，不只交易所）在同目錄的 `recv_opcodes.txt`、`send_opcodes.txt`，
> 可用 `tools/sw_opcodes.py` 對任何版本的 `SoulWorker64.dll` 重新產生。

## 1. 封包框架（meter 看到的樣子）

Hook 送到 meter 的每個 frame 都被正規化成 `SWHEADER`（9 bytes）+ payload：

| offset | 大小 | 內容 |
|---|---|---|
| 0 | u16 | magic |
| 2 | u16 | frame 總長（含 header） |
| 4 | u8 | 方向：1 = 收（server→client），2 = 送（client→server） |
| 5 | u8 | key / seq |
| 6 | u8 | **mainCmd**（分類） |
| 7 | u8 | **subCmd** |
| 8 | u8 | padLen |
| 9 + padLen | … | payload |

`SWPacketMaker` 以 `_byteswap_ushort(_op)` 取得 `(mainCmd << 8) | subCmd`，交易所全部落在 **mainCmd = 0x2B**。
現有解析器都直接從 `_data + 9` 讀 payload（假設 padLen = 0），新增解析器請沿用同一假設，或用 `9 + _data[8]`。

所有整數皆 little-endian；字串是「u16 長度（bytes）+ 內容」，沒有 null 結尾。

## 2. Opcode 對照（mainCmd 0x2B）

客戶端在每個 handler / sender 開頭都會 log 自己的名字（`receive_eSUB_CMD_*` / `send_eSUB_CMD_*`），
opcode 則來自 handler 註冊表與 `XPacket(cat, cmd)` 建構子的常數，兩邊一致。

| opcode | client → server (`send_…`) | server → client (`receive_…`) | 用途 |
|---|---|---|---|
| `0x2B01` | `EXCHANGE_SEARCH` | `EXCHANGE_SEARCH` | 搜尋掛單 → 回傳一頁掛單列表 |
| `0x2B02` | `EXCHANGE_PRICE_HISTORY` | `EXCHANGE_PRICE_HISTORY` | 某道具成交歷史 |
| `0x2B03` | `EXCHANGE_INTEREST_LIST` | `EXCHANGE_INTEREST_LIST` | 關注（書籤）清單 |
| `0x2B04` | `EXCHANGE_INTEREST_ITEM` | `EXCHANGE_INTEREST_ITEM` | 新增 / 移除關注道具 |
| `0x2B05` | `EXCHANGE_SELL_REGISTER` | `EXCHANGE_SELL_REGISTER` | 上架 |
| `0x2B06` | `EXCHANGE_ITEM_BUY` | `EXCHANGE_ITEM_BUY` | 購買 |
| `0x2B07` | `EXCHANGE_ITEM_RECALL` | `EXCHANGE_ITEM_RECALL` | 下架（取回） |
| `0x2B08` | `EXCHANGE_MY_LIST` | `EXCHANGE_MY_LIST` | 我的掛單 |
| `0x2B10` | `EXCHANGE_MY_INFO` | （未命名，handler `0x180e47e80`） | 貨幣交易所（`CMoneyExChangeManager` / `MoneyChangeDialog`） |
| `0x2B11` | `EXCHANGE_PRICE` | （未命名，`0x180e48d20`） | 貨幣交易所：目前價格 |
| `0x2B12` | `EXCHANGE_TRADE` | （未命名，`0x180e47fa0`） | 貨幣交易所：下單 |
| `0x2B13` | `EXCHANGE_CANCEL` | （未命名，`0x180e48430`） | 貨幣交易所：取消訂單 |
| `0x2B14` | `EXCHANGE_CONFIRM` | （未命名，`0x180e48720`） | 貨幣交易所：確認訂單 |
| `0x2B15` | — | （未命名，`0x180e48c70`） | 貨幣交易所：伺服器主動通知? |

相關但不是交易所的分類：`0x0A` = 玩家對玩家交易（`TRADE_REQ/ACCEPT/...`）與個人商店（`PRIVATE_SHOP_*`），
`0x1803` = `ITEM_EXCHANGE`（道具兌換，NPC 用），`0x1830` = `ITEM_SOCKET_EXCHANGE`。

## 3. 共用子結構

### 3.1 `ITEM_INFO`（反序列化函式 `0x18001dea0`，記憶體大小 0xA8）

交易所掛單、我的掛單都內嵌一份。wire 長度 = **113 + len** bytes。

| # | wire 型別 | 記憶體 offset | 推測意義 |
|---|---|---|---|
| 1 | u32 | +0x00 | **itemID**（道具表 ID；購買請求會原樣送回、書籤比對用它） |
| 2 | u64 | +0x08 | **serial**（道具唯一序號；`ITEM_RECALL` 用它下架） |
| 3 | u16 | +0x10 | **count**（堆疊數量；購買後伺服器用它扣減） |
| 4 | u8 | +0x12 | ? |
| 5 | 5 × { u16, u32 } | +0x14 … +0x3B | 5 個 (type, value) 對，應為附加屬性 / 插槽 |
| 6 | u8 | +0x3C | **強化等級**（UI 顯示成「+N」） |
| 7 | u8 | +0x13 | ? |
| 8 | u8 | +0x3E | ? |
| 9 | u64 | +0x40 | ? |
| 10 | u8 | +0x48 | ? |
| 11 | u8 | +0x49 | ? |
| 12 | u8 | +0x3D | ? |
| 13 | u32 | +0x4C | ? |
| 14 | u16 len + `len` bytes | +0x50（char[21]） | 短字串，len 必須 1..20（否則整包標記錯誤），推測為製作者名 / 標題 |
| 15 | u8, u8, u8 | +0x65, +0x66, +0x67 | ? |
| 16 | u32, u32, u32 | +0x68, +0x6C, +0x70 | ? |
| 17 | u8 | +0x74 | ? |
| 18 | u32 | +0x78 | ?（UI 拿 `> 0` 當旗標） |
| 19 | u8, u8 | +0x7C, +0x7D | ? |
| 20 | 4 × u32 | +0x80 … +0x8F | ? |
| 21 | u8 | +0x90 | ? |
| 22 | u64 | +0x98 | ? |
| 23 | u8 | +0xA0 | ? |

### 3.2 `SubA`（`0x1800219c0`，記憶體 0x38，wire **43** bytes）

`u64, u32, u8, 5 × { u16, u32 }`。以 `u16 count` 開頭的陣列形式出現（`vector<SubA>`）。

### 3.3 `SubB`（`0x180021c90`，wire **88** bytes）

`u64, 20 × u32`。記憶體以 `-1`/`0xFF` 初始化，像是一組固定長度的屬性表。

### 3.4 `SubC`（`0x180024610`）

`u64, s32 n, n × { u64, u32, u32 }`（每項 16 bytes）。

### 3.5 寬字串（`0x18000e310`）

`u16 byteLen`，接著 `byteLen` bytes 的 UTF-16LE；上限由呼叫端給（交易所都是 21 wchar → byteLen ≤ 40）。

## 4. 各封包內容

### 4.1 `0x2B01` EXCHANGE_SEARCH

**請求**（序列化 `0x18004d2d0`，固定 48 bytes）— 呼叫端 `0x180855510` 從搜尋條件結構填入：

| # | 型別 | 來源（搜尋條件結構 offset） | 推測 |
|---|---|---|---|
| 1 | u32 | 呼叫端第 2 參數 | page? |
| 2 | u32 | cond+0x04 | 類別（category）? |
| 3 | u32 | cond+0x08 | 子類別? |
| 4 | u32 | cond+0x0C | ? |
| 5 | u32 | cond+0x10 | ? |
| 6 | u32 | cond+0x14，為 0 時送 `-1` | 職業（class）篩選? |
| 7 | u32 | cond+0x18，為 0 時送 `0x55` | 等級/階段（grade/step）篩選? |
| 8 | u32 | cond+0x2C | ? |
| 9 | u32 | 常數 `6` | 排序 / 每頁筆數? |
| 10 | u64 | cond+0x20，為 0 時送 `-1` | 指定 itemID / 書籤? |
| 11 | u32 | cond+0x28 | ? |

**回應**（反序列化 `0x18004d3d0`）：

```
u32   a          -> SetExchangeSearchPage(a, b)  → 推測 目前頁
u32   b                                          → 推測 總頁數 / 總筆數
u16   count
count × SearchEntry
```

`SearchEntry`（`0x18004cf40`，記憶體 0x198 bytes）：

| # | wire 型別 | 記憶體 offset | 意義（依 UI 用法） |
|---|---|---|---|
| 1 | u32 | +0x00 | **listingKey** — 購買請求原樣送回；`ITEM_BUY` 回應用它找回這筆掛單 |
| 2 | u64 | +0x08 | **price**（UI 以 double 顯示的金額） |
| 3 | u64 | +0x10 | **expireAt**（unix 秒；UI 算 `(expireAt - now) / 60` 顯示剩餘分鐘） |
| 4 | u8 | +0x18 | flag?（購買請求會原樣送回） |
| 5 | `ITEM_INFO` | +0x20 | 道具 |
| 6 | u16 n + n × `SubA` | +0xC8 | ? |
| 7 | `SubB` | +0xE8 | ? |
| 8 | `SubC` | +0x140 | ? |
| 9 | 寬字串（≤ 20 wchar） | +0x168 | **賣家名稱** |

Handler 流程：`ExchangeManager::AddSearchResult()` 逐筆加入（有一個以 `itemID` 查道具表、依道具旗標過濾的檢查），
再呼叫 `ExchangeDialog` 重畫列表，每列顯示：道具名、強化 `+N`、數量、單價 (double)、剩餘時間、賣家、是否已關注。

### 4.2 `0x2B02` EXCHANGE_PRICE_HISTORY

**請求**：`u32 charID, u32 itemID`（`charID` 取自 `GetMyPlayer()`；`itemID` 取自被點的道具物件 +0xD8）。

**回應**（`0x18004d5a0`）：

```
u32   charID        -> 與自己的 charID 比對，不同就丟棄
u32   itemID?
u16   count
count × { u32 ?, u16 ?, u64 ?, u64 ?, wstr(≤20) }   ; 推測 {時間, 數量, 價格, ?, 對方名稱}
u64   ?
u64   ?
u64   ?
```

### 4.3 `0x2B03` EXCHANGE_INTEREST_LIST

**請求**：`u32 charID`。**回應**（`0x180049660`）：`u32 charID, u16 n, n × u32 itemID`。
（進入遊戲 / 開啟交易所時客戶端會連發 `INTEREST_LIST` 與 `MY_LIST`。）

### 4.4 `0x2B04` EXCHANGE_INTEREST_ITEM

**請求**：`u32 charID, u32 itemID, u8 add`（1 = 加入關注，0 = 移除）。
**回應**（`0x1800566a0`）：`u32 charID, u32 itemID, u32 result, u8 add`；`result == 0` 時依 `add` 更新本地關注清單。

### 4.5 `0x2B05` EXCHANGE_SELL_REGISTER

**請求**（`0x18004d7a0`）：

| # | 型別 | 來源 | 推測 |
|---|---|---|---|
| 1 | u32 | item+0xD8 | itemID |
| 2 | u16 | UI 輸入 | count |
| 3 | u8 | item+0x180 | 背包類型? |
| 4 | u16 | item+0x7C | 背包格 index? |
| 5 | u64 | UI 輸入 | **price** |
| 6 | u8 | UI | 上架時長選項? |
| 7 | u8 | UI | ? |
| 8 | u8 | UI | ? |

**回應**：`u32 result`（0 = 成功；`0xE3BE` 有特別處理分支，估計是「已達上架上限 / 需要手續費」類錯誤）。

### 4.6 `0x2B06` EXCHANGE_ITEM_BUY

**請求**（`0x18004d860`）：`u32 listingKey, u32 itemID, u16 count, u8 flag`（前三者/flag 直接來自 `SearchEntry` 的 +0x00 / ITEM_INFO.itemID / +0x18）。
**回應**（`0x18004d8c0`）：`u32 result, u32 listingKey, u16 boughtCount`。
`result == 0` 時：找到該 listingKey 的掛單，若 `entry.count > boughtCount` 則扣減，否則整筆移除。

### 4.7 `0x2B07` EXCHANGE_ITEM_RECALL

**請求**：`u64 itemSerial`（取自我的掛單項目 ITEM_INFO.serial）。
**回應**（`0x180055130`）：`u32 result, u64 itemSerial`；`result == 0` 時從「我的掛單」移除 serial 相符的一筆（`0x1804c5a50` 比對 `MyEntry+0x30` = ITEM_INFO.serial）；`result == 0x54` 另有分支。

### 4.8 `0x2B08` EXCHANGE_MY_LIST

**請求**：`u32 charID`。**回應**（`0x18004d960`）：

```
u16   count
count × MyEntry
u8    ?
```

`MyEntry`（`0x18004d080`，記憶體 0x170）：

| # | wire 型別 | 記憶體 offset | 推測 |
|---|---|---|---|
| 1 | u32 | +0x00 | listingKey? |
| 2 | u64 | +0x08 | price? |
| 3 | u64 | +0x10 | expireAt? |
| 4 | u8 | +0x18 | 狀態（販售中 / 已售出 / 過期）? |
| 5 | u16 | +0x1A | ? |
| 6 | u64 | +0x20 | ? |
| 7 | `ITEM_INFO` | +0x28 | 道具（+0x30 = serial，下架用） |
| 8 | u16 n + n × `SubA` | +0xD0 | ? |
| 9 | `SubB` | +0xF0 | ? |
| 10 | `SubC` | +0x148 | ? |

### 4.9 `0x2B10`–`0x2B15` 貨幣交易所（`CMoneyExChangeManager`）

與道具交易所不同系統，UI 是 `MoneyChangeDialog`；log 字串顯示欄位語意：
`send_eSUB_CMD_EXCHANGE_TRADE - Type: %d, Price: %d, Amount: %d`、`… CANCEL - OrderID: %lld`、`… CONFIRM - OrderID: %lld`。

| opcode | 請求 | 回應 |
|---|---|---|
| `0x2B10` MY_INFO | 無 payload | （`0x18004dec0`，未展開） |
| `0x2B11` PRICE | 無 payload | 三個 u64（透過 `0x18000e440` 讀），推測 買價 / 賣價 / ? |
| `0x2B12` TRADE | `u8 type, u32 price, u32 amount` | `u32 result, u8, u64, u32, u32, u32, u64, u64, u32, u32, u32, u64, u64`（`0x18004e260`） |
| `0x2B13` CANCEL | `u64 orderID` | 失敗時 log `[Exchange] Cancel Request Failed: ErrorCode=%d` |
| `0x2B14` CONFIRM | `u64 orderID` | 失敗時 log `[Exchange] Confirm Request Failed: ErrorCode=%d` |
| `0x2B15` | — | 伺服器主動推送，會刷新 `MoneyChangeDialog` / `QuickMenuDialog` |

## 5. 接進 meter 的做法

1. `PacketType.h` 的 `RecvOPcode` 加上 `EXCHANGE_SEARCH = 0x2b01` 等（以及 `SendOPCode` 對應項），`SWPacketMaker::CreateSWPacket` 加 case。
2. 解析 `0x2B01` 回應時依 §4.1 逐欄位讀（不能用固定 offset 的 struct cast：`ITEM_INFO` 內有變長字串，賣家名稱也是變長）。
   一個安全的作法是寫一個 cursor reader（`u8/u16/u32/u64/str`），每讀一次檢查不超過 `_swheader->_size`。
3. 最有價值的資料是 `SearchEntry.{ITEM_INFO.itemID, ITEM_INFO.count, ITEM_INFO.+0x3C 強化, price, expireAt, seller}`，
   足以做「市價記錄 / 價格追蹤」。
4. **請先抓真實封包驗證**：`SWPacketMaker.h` 把 `DEBUG_RECV_DISPLAY_ALL_PKT` 設為 1 會把每個 frame 以 hex 寫進 log；
   在遊戲裡開交易所搜尋一次，就能對照 §4.1 的順序核對每個欄位（特別是 `price` 與 `expireAt` 兩個 u64 的順序，
   以及 §4.1 請求欄位的名稱）。

## 6. 尚未確認 / 已知限制

- 欄位名稱除了表中標明「依 UI 用法」者之外都是推測；`ITEM_INFO` 只確定 itemID / serial / count / 強化四個欄位。
- `SubA`/`SubB`/`SubC` 的語意不明（可能是附魔、Akashic、Soul Stone 之類的道具附加資料）。
- 本文件依 GB 511199；KR / JP 客戶端的 subCmd 可能不同，改版後請用 `tools/sw_opcodes.py` 重新產表，
  handler 位址則需重新反組譯。
- 分析時用到的暫存腳本（handler 註冊表解析、XPacket 反序列化追蹤）已整併成 `tools/sw_opcodes.py`；
  結構解析的部分是手動閱讀反組譯得到的，沒有自動化。
