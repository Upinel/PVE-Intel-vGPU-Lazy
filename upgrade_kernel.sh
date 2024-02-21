#!/bin/sh
echo "+----------------------------------------------------------------------+"
echo "| Intel Gen 12/13 vGPU SR-IOV on Proxmox Host Enabler (Step 1)         |"
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
echo "Install Kernel Update"

CODENAME=`cat /etc/os-release |grep PRETTY_NAME |cut -f 2 -d "(" |cut -f 1 -d ")"`


cp /etc/apt/sources.list /etc/apt/sources.list.bak
echo "deb http://download.proxmox.com/debian/pve $CODENAME pve-no-subscription" | sudo tee -a /etc/apt/sources.list
mv /etc/apt/sources.list.d/pve-enterprise.list /etc/apt/sources.list.d/pve-enterprise.list.bak
echo "deb http://download.proxmox.com/debian/pve $CODENAME pve-no-subscription" > /etc/apt/sources.list.d/pve-no-subscription.list

#update
apt update -y 
apt dist-upgrade -y 

#Install pve-kernel-6.5 for lower kernel
kernel_version=$(uname -r | awk -F '[.-]' '{print $1"."$2}')
target_version="6.5"
if [[ $kernel_version < $target_version ]]; then
    echo "Current version ($kernel_version) is lower than $target_version. Installing pve-kernel-$target_version..."
    apt install pve-kernel-$target_version -y
else
    echo "Current version ($kernel_version) is not lower than $target_version. No action needed. Please run ./install.sh to setup the vGPU"
fi

reboot

