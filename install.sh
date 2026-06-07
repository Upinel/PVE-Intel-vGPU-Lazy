#!/bin/sh
echo "+----------------------------------------------------------------------+"
echo "| Intel Gen 12/13 vGPU SR-IOV on Proxmox Host Enabler (Installer)      |"
echo "| https://github.com/Upinel/PVE-Intel-vGPU                             |"
echo "+----------------------------------------------------------------------+"
echo "| This source file is subject to version 2.0 of the Apache license,    |"
echo "| that is bundled with this package in the file LICENSE, and is        |"
echo "| available through the world-wide-web at the following url:           |"
echo "| http://www.apache.org/licenses/LICENSE-2.0.html                      |"
echo "| If you did not receive a copy of the Apache2.0 license and are unable|"
echo "| to obtain it through the world-wide-web, please send a note to       |"
echo "| license@swoole.com so we can mail you a copy immediately.            |"
echo "+----------------------------------------------------------------------+"
echo "| Author: Nova Upinel Chow  <dev@upinel.com>                           |"
echo "| Maintainer: Gloria (Nova's personal assistant)                       |"
echo "| Updated: June 2026 for Proxmox 8.2+/9.x                              |"
echo "+----------------------------------------------------------------------+"
echo "Enable IO-MMU on PVE Server"

# Kernel version check
kernel_version=$(uname -r | awk -F '[.-]' '{print $1"."$2}')
target_version="6.5"
if [ "$(printf '%s\n' "$target_version" "$kernel_version" | sort -V | head -n1)" != "$target_version" ]; then
    echo "Current kernel ($kernel_version) is lower than $target_version. Please run ./upgrade_kernel.sh first and come back."
    exit 1
fi

echo "Kernel version $kernel_version detected — OK"

# Check for root
if [ "$(id -u)" -ne 0 ]; then
    echo "This script must be run as root!"
    exit 1
fi

# Clean up previous installs
echo "Cleaning up previous DKMS installs..."
rm -rf /var/lib/dkms/i915-sriov-dkms*
rm -rf /usr/src/i915-sriov-dkms*
rm -rf ~/i915-sriov-dkms
KERNEL=$(uname -r); KERNEL=${KERNEL%-pve}

# Install prerequisites
echo "Installing prerequisites..."
apt update -qq
apt install -y pve-headers-$(uname -r) git dkms mokutil sysfsutils 2>/dev/null

# Build and install DKMS module
echo "Building i915-sriov-dkms module..."
cd ~/PVE-Intel-vGPU-Lazy/i915-sriov-dkms
cp -a ~/PVE-Intel-vGPU-Lazy/i915-sriov-dkms/dkms.conf{,.bak}
sed -i 's/"@_PKGBASE@"/"i915-sriov-dkms"/g' ~/PVE-Intel-vGPU-Lazy/i915-sriov-dkms/dkms.conf
sed -i 's/"@PKGVER@"/"'"$KERNEL"'"/g' ~/PVE-Intel-vGPU-Lazy/i915-sriov-dkms/dkms.conf
sed -i 's/ -j$(nproc)//g' ~/PVE-Intel-vGPU-Lazy/i915-sriov-dkms/dkms.conf
cat ~/PVE-Intel-vGPU-Lazy/i915-sriov-dkms/dkms.conf

dkms add .
cd /usr/src/i915-sriov-dkms-$KERNEL

dkms install -m i915-sriov-dkms -v $KERNEL -k $(uname -r) --force -j 1
dkms_status=$?
dkms status

if [ $dkms_status -ne 0 ]; then
    echo "ERROR: DKMS build failed. Check the output above for details."
    echo "You may need a newer version of the DKMS module for your kernel."
    exit 1
fi

echo ""
echo "********************************************"
echo "***  Enrolling MOK key for module signing ***"
echo "***  (Required for Proxmox 8.2+ !)       ***"
echo "********************************************"

# Enroll MOK key — needed even with Secure Boot OFF on Proxmox 8.2+
if [ -f /var/lib/dkms/mok.pub ]; then
    mokutil --import /var/lib/dkms/mok.pub
    echo ""
    echo "MOK key enrolled. After reboot, you MUST:"
    echo "  1. Select 'Enroll MOK' at the blue screen"
    echo "  2. Select 'Continue'"
    echo "  3. Select 'Yes'"
    echo "  4. Select 'Reboot'"
    echo ""
    echo "If you miss this, the module won't load. Re-run: mokutil --import /var/lib/dkms/mok.pub"
fi

echo ""
echo "********************************************"
echo "***  Configuring kernel command line      ***"
echo "********************************************"

# /etc/default/grub update
cp -a /etc/default/grub{,.bak}
sed -i 's/GRUB_CMDLINE_LINUX_DEFAULT="quiet"/GRUB_CMDLINE_LINUX_DEFAULT="quiet intel_iommu=on iommu=pt i915.enable_guc=3 i915.max_vfs=7"/g' /etc/default/grub

# Also update systemd-boot cmdline if present
if [ -d /boot/efi/loader ]; then
    echo "systemd-boot detected, updating kernel cmdline..."
    echo "intel_iommu=on iommu=pt i915.enable_guc=3 i915.max_vfs=7" > /etc/kernel/cmdline
    proxmox-boot-tool refresh 2>/dev/null
fi

echo ""
echo "    Updating bootloader..."
update-grub
update-initramfs -u -k all
pve-efiboot-tool refresh 2>/dev/null

echo ""
echo "    Setting sriov_numvfs=7 in sysfs.conf"
echo "devices/pci0000:00/0000:00:02.0/sriov_numvfs = 7" > /etc/sysfs.conf
cat /etc/sysfs.conf

echo ""
echo "+----------------------------------------------------------------------+"
echo "|  Installation complete!                                              |"
echo "+----------------------------------------------------------------------+"
echo "|                                                                      |"
echo "|  AFTER REBOOT:                                                       |"
echo "|  1. Enroll MOK at the blue screen (if shown)                         |"
echo "|  2. Verify with: lspci | grep VGA                                    |"
echo "|     You should see 00:02.0 plus 7 VFs (00:02.1 through 00:02.7)     |"
echo "|  3. If only 00:02.0 shows, run troubleshoot:                        |"
echo "|     dmesg | grep -E 'i915|DMAR|IOMMU' | tail -20                     |"
echo "|                                                                      |"
echo "|  For Windows VM setup, see the full guide:                           |"
echo "|  https://github.com/Upinel/PVE-Intel-vGPU                            |"
echo "+----------------------------------------------------------------------+"

echo ""
echo "   Rebooting in 10 seconds... Press Ctrl+C to cancel."
sleep 10
reboot

