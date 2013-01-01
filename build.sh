    echo -e "Making HOX+ zImage\n"
    export PATH=$PATH:/opt/toolchain_linaro2/bin/
    export ARCH=arm
    export SUBARCH=arm
    export CROSS_COMPILE=arm-eabi-
    make -j3 
## > /home/dave/android/history.txt

    cp /home/dave/android/hox+/arch/arm/boot/zImage /home/dave/android/kernelinjector.oxp_stock/zImage.new/
    cp /home/dave/android/hox+/drivers/scsi/scsi_wait_scan.ko /home/dave/android/kernelinjector.oxp_stock/structure.new/modules/

    cp /home/dave/android/hox+/arch/arm/mach-tegra/baseband-xmm-power2.ko /home/dave/android/kernelinjector.oxp_stock/structure.new/modules/

    cp /home/dave/android/hox+/drivers/net/usb/raw_ip_net.ko /home/dave/android/kernelinjector.oxp_stock/structure.new/modules/

    cp /home/dave/android/hox+/drivers/usb/class/cdc-acm.ko /home/dave/android/kernelinjector.oxp_stock/structure.new/modules/

    cp /home/dave/android/hox+/drivers/usb/serial/baseband_usb_chr.ko /home/dave/android/kernelinjector.oxp_stock/structure.new/modules/

    ls -l /home/dave/android/hox+/arch/arm/boot/zImage 

    cd /home/dave/android/kernelinjector.oxp_stock/
    rm /home/dave/android/kernelinjector.oxp_stock/bootimg.out/boot.img
    sh /home/dave/android/kernelinjector.oxp_stock/compile
    echo -e "Check zImage timestamp is correct\n"
##
