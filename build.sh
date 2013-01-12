    echo -e "Making HOX+ zImage\n"
    export PATH=$PATH:/opt/toolchain_linaro2/bin/
    export ARCH=arm
    export SUBARCH=arm
    export CROSS_COMPILE=arm-eabi-
    make -j3 
## > /home/dave/android/history.txt

    cp ./arch/arm/boot/zImage ../kernelinjector.oxp_stock/zImage.new/
    
    find ./drivers -type f -name '*.ko' -exec cp -f {} ../kernelinjector.oxp_stock/structure.new/modules \;
    find ./arch -type f -name '*.ko' -exec cp -f {} ../kernelinjector.oxp_stock/structure.new/modules \;

    ls -l ./arch/arm/boot/zImage 

    cd ../kernelinjector.oxp_stock/

    sh ./compile
    mv ./bootimg.out/boot.img ./bootimg.out/boot$(date +%Y%m%d%H%M).img

    echo -e "Check zImage timestamp is correct\n"
##
