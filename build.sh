#!/bin/sh

TOOLCHAIN="/home/hillbeast/Development/cross/arm-gnu-toolchain-15.2.rel1-x86_64-arm-none-eabi/bin/arm-none-eabi-"
CORES=16
VERSION="Test-`date '+%Y%m%d-%H%M'`"
CPDEST="/home/hillbeast/Desktop/"
DTB="samsung/s5p6442-apollo.dtb"

export KBUILD_BUILD_VERSION=$VERSION

if [ "$1" = "config" ]; then
	make menuconfig ARCH=arm CROSS_COMPILE=$TOOLCHAIN
	exit
fi

echo "Compiling the kernel ($VERSION)..."
rm -f arch/arm/boot/zImage
make -j$CORES CROSS_COMPILE=$TOOLCHAIN ARCH=arm

if test -f "arch/arm/boot/zImage"; then
	TARBALL=$VERSION-zImage.tar
	cp arch/arm/boot/zImage ./
	echo "  DTB CAT $DTB > zImage"
	cat arch/arm/boot/zImage arch/arm/boot/dts/$DTB > ./zImage
	echo "  TAR     $TARBALL"
	tar cf $TARBALL zImage
	echo "  COPY    $CPDEST$TARBALL"
	cp $TARBALL $CPDEST
else
	echo "Make failed to produce zImage"
fi

echo "Done"
