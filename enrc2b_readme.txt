--Please follow below command to download the official android toolchain: (arm-eabi-4.6)
        
                git clone https://android.googlesource.com/platform/prebuilt

                NOTE: the tool ¡¥git¡¦ will need to be installed first; for example, on Ubuntu, the installation command would be: apt-get install git

--Modify the .bashrc to add the toolchain path, like bellowing example:

								PATH=/usr/local/share/toolchain-eabi-4.6/bin:$PATH 

--Start 
$make ARCH=arm CROSS_COMPILE=$TOP/prebuilts/gcc/linux-x86/arm/arm-eabi-4.6/bin/arm-eabi- ap37_android_defconfig
$make ARCH=arm CROSS_COMPILE=$TOP/prebuilts/gcc/linux-x86/arm/arm-eabi-4.6/bin/arm-eabi- -j8

$TOP is an absolute path to android JB code base

 



--Clean
								$make clean

--Files path
> arch/arm/boot/zImage

> Please remember to update kernel modules also by the following commands before full function testing
adb remount
adb push bcmdhd.ko system/lib/modules/
adb push raw_ip_net.ko system/lib/modules/
adb push cdc-acm.ko system/lib/modules/
adb push baseband_usb_chr.ko system/lib/modules/
adb push tcrypt.ko system/lib/modules/
adb push baseband-xmm-power2.ko system/lib/modules/
adb shell "chmod 0644 system/lib/modules/"
adb reboot