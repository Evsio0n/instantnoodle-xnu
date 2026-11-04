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

以下是在**一台**手機（IN2010，面板 AMB655UV01）上實際驗證的結果，截至 **2026-11-04**。「已安裝」= 拔掉 USB
也能從手機自己的儲存開機。

### 可以用

| 功能 | 狀態 | 說明 |
|---|---|---|
| 獨立開機到桌面 | ✅ 已安裝 | 開機選單可進 USB 開發模式；連續 3 次開不起來會自動退回上一版 |
| 儲存（UFS）、使用者資料（HFS+） | ✅ 已安裝 | 重開機後資料保留 |
| 顯示、90 Hz 面板模式、無撕裂 | ✅ 已安裝 | 實際幀率常只有約 45 fps（見已知問題） |
| GPU 合成（Adreno 650、Mesa freedreno） | ✅ 已安裝 | Metal 仍在實驗階段 |
| 觸控、電源鍵、音量下 | ✅ 已安裝 | |
| 手動亮度 | ✅ 已安裝 | 尚無自動亮度 |
| Wi-Fi 2.4/5 GHz（QCA6390）、Safari | ✅ 已安裝 | 已驗證 WPA2 |
| Camera.app 前鏡頭（IMX471） | ✅ 已安裝 | 30 fps 預覽，影像處理在 GPU 上 |
| 主鏡頭（IMX586，4800 萬像素）1x 與自動對焦 | ✅ 已安裝 | 使用手機本身的色彩校正資料 |
| 2x（IMX586 感光元件裁切；暗處改數位變焦） | ✅ 已安裝 | 1x ↔ 2x 切換約 0.3 秒 |
| 超廣角（IMX481）0.5x 與自動對焦 | ✅ 已安裝 | 跟 iPhone 11 Pro 一樣的 .5 / 1 / 2 按鈕；尚未做畸變校正 |

### 尚未完成

| 功能 | 說明 |
|---|---|
| 聲音、震動 | 這台尚未可用 |
| 藍牙 | 部分完成 |
| 感測器、自動亮度、GPS | 進行中或尚未開始 |
| 行動網路 | 尚未開始 |
| App Store、Apple ID、iCloud、推播、FairPlay | 受阻（需要 Secure Enclave） |
| 微距鏡頭（GC2375） | 只偵測到 |
| 人像模式景深 | 需要兩顆鏡頭同時出流 |

### 已知問題

- 實際幀率常只有約 45 fps，不是 90
- 從儲存開機慢：開機程式把 log 逐字畫到螢幕
- 相機：換鏡頭要 0.5–1 秒；人像模式有列出但沒有景深
- 無法設定桌布
- 設定裡開粗體字會卡住
- 時間慢約 2 秒
- 從儲存開機時無法截圖
- Wi-Fi 列表刷新不完整、狀態列沒有訊號格

---

## 建置與執行

詳見 [docs/devices/instantnoodle.md](docs/devices/instantnoodle.md)、[docs/BUILD.md](docs/BUILD.md)、
[docs/INSTALL.md](docs/INSTALL.md)（英文）。簡單說：在電腦上建置核心和映像，讓手機進入開機選單的 USB 開發模式，
再用 `xl` 載入記憶體執行（不寫入手機）。安裝到儲存會改寫手機的 `userdata`，Android 會被移除。

> ⚠️ 刷寫可能讓手機無法使用。安裝前請先了解救援方法（[docs/INSTALL.md](docs/INSTALL.md)、
> [docs/research/boot-chain-and-recovery.md](docs/research/boot-chain-and-recovery.md)）。

## 路線圖

1. 上述已知問題、聲音、震動、感測器、GPS、Metal、安裝第三方 app
2. 依賴 TrustZone 的服務、Titan 480 硬體 ISP、相機啟動速度
