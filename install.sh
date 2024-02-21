#!/bin/sh
echo "+----------------------------------------------------------------------+"
echo "| Intel Gen 12/13 vGPU SR-IOV on Proxmox Host Enabler (Step 2)         |"
echo "| https://github.com/Upinel/Intel-vGPU-SRIOV-on-Proxmox                |"
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
echo "| Date:   20/Feb/2024                                                  |"
echo "+----------------------------------------------------------------------+"
echo "Enable IO-MMU on PVE Server"

# Kernel version check
kernel_version=$(uname -r | awk -F '[.-]' '{print $1"."$2}')
target_version="6.5"
if [[ $kernel_version < $target_version ]]; then
    echo "Current version ($kernel_version) lower than $target_version. Please run ./upgrade_kernel.sh first and back here"
    exit 0
fi

# Clean up
apt update && apt install git pve-headers pve-headers-$(uname -r) mokutil -y
rm -rf /var/lib/dkms/i915-sriov-dkms*
rm -rf /usr/src/i915-sriov-dkms*
rm -rf ~/i915-sriov-dkms
KERNEL=$(uname -r); KERNEL=${KERNEL%-pve}

apt install git dkms build-* unzip -y

cd ~/PVE-Intel-vGPU-Lazy/i915-sriov-dkms
cp -a ~/PVE-Intel-vGPU-Lazy/i915-sriov-dkms/dkms.conf{,.bak}
sed -i 's/"@_PKGBASE@"/"i915-sriov-dkms"/g' ~/PVE-Intel-vGPU-Lazy/i915-sriov-dkms/dkms.conf
sed -i 's/"@PKGVER@"/"'"$KERNEL"'"/g' ~/PVE-Intel-vGPU-Lazy/i915-sriov-dkms/dkms.conf
sed -i 's/ -j$(nproc)//g' ~/PVE-Intel-vGPU-Lazy/i915-sriov-dkms/dkms.conf
cat ~/PVE-Intel-vGPU-Lazy/i915-sriov-dkms/dkms.conf

apt install  dkms -y

dkms add .
cd /usr/src/i915-sriov-dkms-$KERNEL

dkms install -m i915-sriov-dkms -v $KERNEL -k $(uname -r) --force -j 1
dkms status

echo ""
echo "********************************************"
echo "***  Enable IO-MMU on proxmox host       ***"
echo "********************************************"
# /etc/default/grub update
cp -a /etc/default/grub{,.bak}
sed -i 's/GRUB_CMDLINE_LINUX_DEFAULT="quiet"/GRUB_CMDLINE_LINUX_DEFAULT="quiet intel_iommu=on i915.enable_guc=3 i915.max_vfs=7"/g' /etc/default/grub

echo ""
echo "    Update grub .... "
update-grub
update-initramfs -u -k all
pve-efiboot-tool refresh

echo ""
echo "    Install sysfsutils ,set sriov_numvfs=7"
apt install sysfsutils -y
echo "devices/pci0000:00/0000:00:02.0/sriov_numvfs = 7" > /etc/sysfs.conf

echo ""
echo "   Please Verify SR-IOV by lspci |grep VGA after reboot ...."
reboot

