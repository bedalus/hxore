    echo -e "Making HOX+ zImage\n"
    export PATH=$PATH:/opt/toolchain_linaro2/bin/
    export ARCH=arm
    export SUBARCH=arm
    export CROSS_COMPILE=arm-eabi-
    make -j3 
## > /home/dave/android/history.txt

    cp /home/dave/android/hox+/arch/arm/boot/zImage /home/dave/android/kernelinjector.oxp_stock/zImage.new/
    cp /home/dave/android/hox+/drivers/scsi/scsi_wait_scan.ko /home/dave/android/kernelinjector.oxp_stock/structure.new/modules/
    ls -l /home/dave/android/hox+/arch/arm/boot/zImage 

    cd /home/dave/android/kernelinjector.oxp_stock/
    rm /home/dave/android/kernelinjector.oxp_stock/bootimg.out/boot.img
    sh /home/dave/android/kernelinjector.oxp_stock/compile

    cp /home/dave/android/kernelinjector.oxp_stock/bootimg.out/boot.img /media/74C420EEC420B470/Documents\ and\ Settings/Administrator/
    ls -l /media/74C420EEC420B470/Documents\ and\ Settings/Administrator/

    echo -e "Check zImage timestamp is correct\n"
##
