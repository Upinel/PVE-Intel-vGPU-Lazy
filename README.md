# Intel Gen 12/13 vGPU (SR-IOV) on Proxmox One-Click Installer Script (Beta)
This Auto Installer is designed to help you virtualize the 12th-generation Intel integrated GPU (iGPU) and share it as a virtual GPU (vGPU) with hardware acceleration and video encoding/decoding capabilities across multiple VMs.

### Introduction

Although not suited for gaming due to the limited performance of Intel’s iGPU, especially when shared among multiple VMs, this setup excels at video decoding tasks like streaming YouTube and accelerating RDP sessions without burdening the CPU.  
Once you complete this setup, consider enhancing your RDP experience with my project [Upinel/BetterRDP](https://github.com/Upinel/BetterRDP), which leverages vGPU capabilities.

### WARNING & Disclaimer

This is a **highly experimental** One-Click Lazy Autoinstaller version of [Upinel/PVE-Intel-vGPU](https://github.com/Upinel/PVE-Intel-vGPU) and only suitable for Fresh Installation copy of Proxmox 8.1.4 or newer, which means **No Important Data** on the Proxmox Server. This workaround is not officially supported by Proxmox. Use at your own risk.  
I also assume you had enough knowledge to handle any possibly issues during the usage.  
I highly recommend use manual install guide at [Upinel/PVE-Intel-vGPU](https://github.com/Upinel/PVE-Intel-vGPU) if ever possible. Again, Use at your own risk.  

### Environment
The environment used for this guide:  
• Model: Intel NUC12 Pro Wall Street Canyon (NUC12WSKi5)  
• CPU: Intel 12th Gen i5 1240P (12 Cores, 16 Threads)  
• RAM: 64GB DDR4 by Samsung  
• Storage: 2TB Samsung 980 Pro NVMe SSD and 4TB Samsung 870 Evo SATA SSD  
• Graphics: Intel Iris Xe Graphics (80 Execution Units)  

### Prerequisites
Before proceeding, ensure the following:  
• VT-d (IOMMU) and SR-IOV are enabled in BIOS.  
• Proxmox Virtual Environment (VE) version 8.1.4 or newer is **installed with GRUB bootloader**.  
• EFI is **enabled**, and Secure Boot is **disabled**.  
**IMPORTANT: If any of them you are unsure, back to the manual guide at [Upinel/PVE-Intel-vGPU](https://github.com/Upinel/PVE-Intel-vGPU)**

## Proxmox vGPU Install
SSH into your Proxmox Host as root, and execute:  
```bash
cd ~
git clone https://github.com/Upinel/PVE-Intel-vGPU-Lazy
cd ~/PVE-Intel-vGPU-Lazy
chmod +x *.sh
./install.sh
```
**As easy as that. Now your host is well prepared, and you can set up Windows 10/11 VMs with SR-IOV vGPU support.**

(Optional) If you have an older version of Proxmox, you may want to use the following one-click script to update the kernel, but again, **that means you are possilbly not using this script on fresh installation**, use at your own risk:  
```bash
./upgrade_kernel.sh
```

## Install Windows VM
For Windows Client VM Installation, please refer to [Upinel/PVE-Intel-vGPU?tab=readme-ov-file#windows-installation](https://github.com/Upinel/PVE-Intel-vGPU?tab=readme-ov-file#windows-installation).

## Conclusion
By completing this guide, you should be able to share your Intel Gen 12 iGPU across multiple VMs for enhanced video processing and virtualized graphical tasks within a Proxmox environment.

## Credits

The DKMS module by Strongz is instrumental in making this possible ([i915-sriov-dkms GitHub repository](https://github.com/strongtz/i915-sriov-dkms?ref=michaels-tinkerings)).  
Additionally, Derek Seaman and Michael's blog post was an inspirational resource ([Derek Seaman’s Proxmox vGPU Guide](https://www.derekseaman.com/2023/11/proxmox-ve-8-1-windows-11-vgpu-vt-d-passthrough-with-intel-alder-lake.html) & [vGPU (SR-IOV) with Intel 12th Gen iGPU](https://www.michaelstinkerings.org/gpu-virtualization-with-intel-12th-gen-igpu-uhd-730/)).  
*Because this is a lazy installer, to reduce variances, this pack also included the archive of strongtz/i915-sriov-dkms driver on the date of 21 Feb 2024.*  
This installer is tailored for Proxmox 8, aiming to streamline the installation process. 

## License
This project is licensed under Apache License, Version 2.0.

## Author
Nova Upinel Chow  
Email: dev@upinel.com

## Buy me a coffee
If you wish to donate us, please donate to [https://paypal.me/Upinel](https://paypal.me/Upinel), it will be really lovely.