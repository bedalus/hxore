    echo -e "Making HOX+ zImage\n"
    export PATH=$PATH:/opt/toolchain3/bin/
    export ARCH=arm
    export SUBARCH=arm
    export CROSS_COMPILE=arm-cortex_a8-linux-gnueabi-

# delete everything
#rm -fR kernelinjector.oxp/structure.new/modules/*
rm -f kernelinjector.oxp/zImage.new/zImage

# cd kernelinjector.oxp
# sh extract
# cd ..

# make
make -j7

# copy modules
find ./ -type f -name '*.ko' -exec cp -f {} kernelinjector.oxp/ramdisk/modules \;

# copy zImage
cp -f arch/arm/boot/zImage kernelinjector.oxp/zImage.new/zImage

cd kernelinjector.oxp
./compile
cp bootimg.out/boot.img ~/Documents_OSX/boot.img
cd ..

#gzip kernelinjector.oxp/structure.new/modules/* kernelinjector.oxp/structure.new/modules/mods.tar.gz
