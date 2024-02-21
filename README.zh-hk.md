[![en-GB](https://img.shields.io/badge/lang-en--gb-green.svg)](https://github.com/Upinel/PVE-Intel-vGPU-Lazy/blob/main/README.md)
[![zh-HK](https://img.shields.io/badge/lang-zh--hk-blue.svg)](https://github.com/Upinel/PVE-Intel-vGPU-Lazy/blob/main/README.zh-hk.md)
# Intel Gen 12/13 代 vGPU (SR-IOV) 在 Proxmox 的一鍵安裝腳本（測試版）
*本README以英文版為準*  
此自動安裝器旨在幫助您虛擬化第 12 代英特爾集成 GPU (iGPU)，並將其作為具有硬件加速和視頻編解碼能力的虛擬 GPU (vGPU) 與多個虛擬機共享。

### 介紹

雖然由於英特爾 iGPU 的性能有限，特別是在與多個虛擬機共享時，並不適合遊戲，但這套設置在像 YouTube 流媒體播放和加速 RDP 會話等視頻解碼任務上表現出色，不會增加 CPU 的負擔。完成這個設置後，考慮使用我的另一個項目 [Upinel/BetterRDP](https://github.com/Upinel/BetterRDP) 來增強您的 RDP 體驗，該項目利用了 vGPU 的能力。

### 警告和免責聲明

這是 [Upinel/PVE-Intel-vGPU](https://github.com/Upinel/PVE-Intel-vGPU) 的一個高度實驗性的一鍵懶人安裝版本，僅適用於 Proxmox 8.1.4 或更新版本的全新安裝，**這意味著 Proxmox 服務器上沒有重要數據**。這個解決方案未經 Proxmox 官方支持。風險自負。我還假設您有足夠的知識在使用過程中處理任何可能的問題。如果可能的話，我強烈建議使用手動安裝指南，詳情請訪問 [Upinel/PVE-Intel-vGPU](https://github.com/Upinel/PVE-Intel-vGPU)。同樣，再三強調風險自負。

### 測試環境
The environment I used for this guide:  
• Model: Intel NUC12 Pro Wall Street Canyon (NUC12WSKi5)  
• CPU: Intel 12th Gen i5 1240P (12 Cores, 16 Threads)  
• RAM: 64GB DDR4 by Samsung  
• Storage: 2TB Samsung 980 Pro NVMe SSD and 4TB Samsung 870 Evo SATA SSD  
• Graphics: Intel Iris Xe Graphics (80 Execution Units)  

### 先決條件
在繼續之前，確保滿足以下條件：  
• BIOS 中啟用了 VT-d (IOMMU) 和 SR-IOV。  
• 已安裝帶有 **GRUB 啟動加載器**的 Proxmox 虛擬環境（VE）版本 8.1.4 或更新版本。  
• 已**啟用 EFI**，並**禁用**了 Secure Boot。  
**重要提示**：如果您對其中任何一項不確定，請回到手動指南 [Upinel/PVE-Intel-vGPU](https://github.com/Upinel/PVE-Intel-vGPU)**

## Proxmox vGPU DKMS 安裝
以 root 身份 SSH 進入您的 Proxmox 主機，並執行： 
```bash
cd ~
git clone https://github.com/Upinel/PVE-Intel-vGPU-Lazy
cd ~/PVE-Intel-vGPU-Lazy
chmod +x *.sh
./install.sh
```
**就這麼簡單。現在您的主機已準備就緒，您可以使用 SR-IOV vGPU 支援來設置 Windows 10/11 VM**

（可選）如果您有舊版本的 Proxmox，您可能希望使用以下一鍵腳本來更新內核，但是再次提醒，**這意味著您可能沒有在全新安裝上使用這個腳本**，風險自負： 
```bash
./upgrade_kernel.sh
```

## 安裝 Windows 虛擬機
有關 Windows 客戶端虛擬機安裝，請參閱 [Upinel/PVE-Intel-vGPU?tab=readme-ov-file#windows-installation](https://github.com/Upinel/PVE-Intel-vGPU?tab=readme-ov-file#windows-installation).

## 結論

通過完成這份指南，您應該能夠在 Proxmox 環境中共享您的英特爾第 12 代 iGPU，用於增強視頻處理和虛擬化圖形任務。

## 鳴謝

The DKMS module by Strongz is instrumental in making this possible ([i915-sriov-dkms GitHub repository](https://github.com/strongtz/i915-sriov-dkms?ref=michaels-tinkerings)).  
Additionally, Derek Seaman and Michael's blog post was an inspirational resource ([Derek Seaman’s Proxmox vGPU Guide](https://www.derekseaman.com/2023/11/proxmox-ve-8-1-windows-11-vgpu-vt-d-passthrough-with-intel-alder-lake.html) & [vGPU (SR-IOV) with Intel 12th Gen iGPU](https://www.michaelstinkerings.org/gpu-virtualization-with-intel-12th-gen-igpu-uhd-730/)).  
*Because this is a lazy installer, to reduce variances, this pack also included the archive of strongtz/i915-sriov-dkms driver on the date of 21 Feb 2024.*  
This installer is tailored for Proxmox 8, aiming to streamline the installation process. 

## 授權
該項目在 Apache License，版本 2.0 下授權。

## 作者
Nova Upinel Chow  
Email: dev@upinel.com

## 請我喝咖啡
If you wish to donate us, please donate to [https://paypal.me/Upinel](https://paypal.me/Upinel), it will be really lovely.