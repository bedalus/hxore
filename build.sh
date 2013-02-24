    echo -e "Making HOX+ zImage\n"
    export PATH=$PATH:/opt/toolchain/bin/
    export ARCH=arm
    export SUBARCH=arm
    export CROSS_COMPILE=arm-eabi-

# delete everything
rm -fR kernelinjector.oxp/structure.new/modules/*
rm -f kernelinjector.oxp/zImage.new/zImage

cd kernelinjector.oxp
sh extract
cd ..

# make
make -j3

# copy modules
find drivers -type f -name '*.ko' -exec cp -f {} kernelinjector.oxp/structure.new/modules \;
find arch -type f -name '*.ko' -exec cp -f {} kernelinjector.oxp/structure.new/modules \;
find fs -type f -name '*.ko' -exec cp -f {} kernelinjector.oxp/structure.new/modules \;

# copy zImage
cp -f arch/arm/boot/zImage kernelinjector.oxp/zImage.new/zImage

cd kernelinjector.oxp
sh compile
cd ..

#gzip kernelinjector.oxp/structure.new/modules/* kernelinjector.oxp/structure.new/modules/mods.tar.gz

# ready for further editing

