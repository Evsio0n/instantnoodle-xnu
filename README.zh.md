# instantnoodle-xnu

**OnePlus 8 上的 iOS 16.5。** 在 OnePlus 8（IN2010，代號 *instantnoodle*，Snapdragon 865）上執行 Apple 開源的 XNU
核心（`xnu-8796.121.2`）和未修改的 iOS 16.5 使用者空間。

[English](README.md)

> 這是研究專案，與 Apple、OnePlus、Qualcomm 無關，也未經它們授權或背書。**本 repository 不含任何 Apple 檔案、
> 金鑰或裝置備份。** 建置腳本會從你自己下載的 IPSW 和你自己的手機取出需要的檔案。收錄的第三方檔案見
> [NOTICE](NOTICE)。

本專案基於 OOSEX GANG 的 [HackiOS](https://github.com/oosexgang/hakios-xnu-kernel)（POCO X5 Pro *redwood* 和 QEMU）
以及 [haxi0/iphont-xnu-kernel](https://github.com/haxi0/iphont-xnu-kernel) 的 OnePlus 8 工作。HackiOS 原本的 README
保留為 [README.hackios.md](README.hackios.md)。授權為 Apple Public Source License 2.0（[LICENSE](LICENSE)）。

這是單一 repository：核心和使用者空間（`userland/`）都在這裡。上游文件提到的兩個 git 目錄的做法
（`common/setup.sh`、`common/ugit`）在這裡用不到。

---

## 狀態

以下是在**一台**手機（IN2010，面板 AMB655UV01）上實際驗證的結果，截至 **2026-11-11**。「已安裝」= 包含在拔掉 USB 也能從手機
自己的儲存開機的版本裡；「RAM」= 只在 RAM 測試開機跑過。多數檢查是在 RAM 開機上做的，核心和函式庫與已安裝版相同，
有差別的地方會在備註說明。

### 可以用

| 功能 | 狀態 | 備註 |
|---|---|---|
| 開機到主畫面，可獨立使用 | ✅ 已安裝 | 開機選單含 USB 開發者模式；連續 3 次開不起來會退回上一版 |
| 儲存（UFS），使用者資料在 HFS+ | ✅ 已安裝 | 重開機資料還在 |
| Metal over Vulkan（Mesa turnip），整個系統 | ✅ 已安裝 | 現在是預設：合成器、主畫面、設定、Safari、照片、地圖、相機。仍有卡頓和記憶體增長，見已知問題 |
| 自己的顯示驅動（MDP 圖層），90 Hz | ✅ 已安裝 | 盡量用硬體圖層取代 GPU 合成；三角形撕裂已解決；看過的測試裡 0 次欠載、0 次 SMMU 錯誤 |
| 觸控、電源鍵、音量下鍵 | ✅ 已安裝 | |
| 手動亮度、EDR 與 HBM | ✅ 已安裝 | 還沒有自動亮度 |
| HDR 相片 | ✅ 已安裝 | 透過 Metal 上真正的 CoreImage 產生 gain map；在 RAM 開機上親眼確認 |
| 地圖 | ✅ 已安裝 | 真實地圖資料畫得正確，手勢不夠跟手；在 RAM 開機上親眼確認 |
| Wi-Fi 2.4/5 GHz（QCA6390）、Safari | ✅ 已安裝 | WPA2；開機自動連上存在建置用 `fw-wifi/network` 裡的網路。這個檔案要自己準備，不在本 repo |
| 三顆後鏡頭和前鏡頭的預覽 | ✅ 已安裝 | 預覽重複幾張畫面的問題已修。暗處預覽每秒 15 張 |
| 手電筒與相機閃光燈 | ✅ 已安裝 | 預設開啟 |
| 藍牙 | ✅ 已安裝 | 預設開啟，控制器會啟動；這個版本還沒重新確認音訊裝置 |
| 行動網路：電信商名稱、訊號、行動數據 | ✅ 已安裝 | 驍龍 X55 數據機。驅動移植自 OnePlus 8 Pro 的專案（[dekefake/hackios-op8pro](https://github.com/dekefake/hackios-op8pro)），見 [tools/nf/cell](tools/nf/cell/README.md)。數據可和 Wi-Fi 並存 |
| 簡訊 | ✅ 已安裝 | 「訊息」App 收發皆可：中文、表情符號、長簡訊、鎖定畫面通知。沒有 MMS |
| 電話（VoLTE） | ✅ 已安裝 | 用 iOS 自己的「電話」App 撥出和接聽，聽筒有聲音。通話音量還不能調；擴音鍵和靜音還沒有用耳朵確認 |
| 喇叭、主麥克風、震動 | 🧪 已安裝，部分重新確認 | 這個版本的麥克風和聽筒已用於真實通話。iOS 自己的音效經喇叭播放是早期 RAM 開機確認的，這次沒有重測 |
| Wi-Fi 定位 | 🧪 RAM | RAM 開機時地圖定位正確。保留的 `/var` 上開關是手動寫入一次的，已安裝版還沒確認。需要 Wi-Fi，沒有 GPS |

### 尚未完成

| 功能 | 備註 |
|---|---|
| 系統休眠 | **已安裝版關閉**，電源鍵只會鎖屏，比較耗電。關 Wi-Fi 時 RAM 開機可休眠和喚醒；開著 Wi-Fi 時韌體在恢復時當掉，要手動重開 |
| GPS | 數據機的定位服務有回應也會回報，但還沒有在戶外定位成功，也還沒接到 iOS 的定位服務 |
| 感測器、自動亮度 | 需要感測器 DSP（SLPI）和整條路徑；DSP 能啟動並在探測中，還沒有感測資料。沒有距離感測器，所以**通話貼耳時螢幕不會熄**；也沒有自動旋轉 |
| 電話的其餘功能 | 保留、插撥、按鍵音、MMS、語音信箱、個人熱點都還沒做。藍牙耳機通話和緊急電話沒有試過 |
| 第三方 App | 第一步：未加密的 App 能抓取、註冊並顯示圖示，也看過它啟動，但還沒看到它畫出畫面。加密（FairPlay）的 App 跑不了 |
| App Store、Apple ID、iCloud、推播、FairPlay | 受限：Apple 的伺服器要求用真 iPhone 的 Secure Enclave 裡的金鑰簽名，而本專案不偽造裝置身分。所以沒有 iMessage 和 FaceTime，App 關閉時也收不到通知。電話、簡訊和手機本機的通知不受影響 |
| 微距鏡頭（GC2375） | 只偵測到 |
| 人像景深 | 需要兩顆鏡頭同時串流 |

### 已知問題

- **合成器的記憶體只漲不回**，重度使用 12 分鐘約漲 2.5 GB。約 10 分鐘後背景 App 會被殺，畫面可能重啟一次
- **日常使用時約每 10 幀有 1 幀遲到**（90 Hz 面板），裝行動網路版本之前就是這樣。GPU 大多閒著（忙碌率中位數 3%）；最明顯的線索是繪圖管線在畫圖時才編譯，而且沒有磁碟快取。目前只從記錄分析過，還沒修：[docs/KNOWN_ISSUES.md](docs/KNOWN_ISSUES.md) K35，計畫在 [docs/TODO.md](docs/TODO.md) T40
- **地圖手勢不夠跟手。** 地圖加合成器的 GPU 每幀約 15 毫秒，90 Hz 的預算是 11 毫秒；第一次用到某個畫面時會因為建管線頓一下
- 發熱：開機頭幾分鐘 CPU 感測器最高約 87 °C，重度使用時曾在約 95 °C 降速
- 相機：暗處每秒 15 張，暗處對焦可能跑出範圍，人像沒有景深效果
- 無法設定桌布。鎖定畫面本身的桌布有畫出來，但長按換桌布的畫面被本專案關掉了：改用 Metal 之前它是全黑的，之後沒有重試過
- 地圖的兩個管線（`GlowAlpha`、`PatternedRibbon`）和 Siri 光球被拒絕（4 倍取樣下的 framebuffer fetch），這些效果畫不出來。Metal 其他被拒絕或用替身的項目見 K42
- 行動網路版的核心還不是由一般建置流程產生的：它是疊加式建置，需要開發者自己建置容器裡的目的檔（[tools/nf/cell/README.md](tools/nf/cell/README.md)）
- 鑰匙圈不提供真正的保護（沒有 Secure Enclave，用的是替身）：不要在這台手機上放重要帳號

---

## 建置與執行

詳見 [docs/devices/instantnoodle.md](docs/devices/instantnoodle.md)、[docs/BUILD.md](docs/BUILD.md)、
[docs/INSTALL.md](docs/INSTALL.md)（英文）。簡單說：在電腦上建置核心和映像，讓手機進入開機選單的 USB 開發模式，
再用 `xl` 載入記憶體執行（不寫入手機）。安裝到儲存會改寫手機的 `userdata`，Android 會被移除。

> ⚠️ 刷寫可能讓手機無法使用。安裝前請先了解救援方法（[docs/INSTALL.md](docs/INSTALL.md)、
> [docs/research/boot-chain-and-recovery.md](docs/research/boot-chain-and-recovery.md)）。

## 路線圖

1. Metal：日常使用的卡頓和合成器記憶體增長（先做管線快取；修之前先量測）
2. Wi-Fi 跨休眠，讓已安裝版可以休眠
3. 透過 SLPI 做感測器：電話能打了，所以先做距離感測器，再做自動亮度和旋轉
4. 行動網路：把驅動併入一般建置；通話音量、保留、按鍵音、MMS
5. GPS：戶外定位成功，再接上 iOS 的定位服務
6. 第三方 App；之後是以 TrustZone 保護的鑰匙圈和 Titan 480 硬體 ISP
