echo "Configure for Pi2/3"

RPI_TOOLROOT=`pwd`/../tools/arm-bcm2708/arm-rpi-4.9.3-linux-gnueabihf
RPI_OPT_VC=`pwd`/../firmware/hardfp/opt/vc
CLANG_TOOLROOT=`pwd`/../clang-sdk/sysroot

RPI_INCLUDES="-I$RPI_OPT_VC/include -I$RPI_OPT_VC/include/interface/vcos/pthreads -I$RPI_OPT_VC/include/interface/vmcs_host/linux"
RPI_LIBDIRS="-L$RPI_TOOLROOT/lib -L$RPI_OPT_VC/lib"
RPI_DEFINES="-D__VCCOREVER__=0x4000000 -mfpu=neon-vfpv4"
#RPI_KEEPS="-save-temps=obj"
RPI_KEEPS=""

./configure --enable-cross-compile\
 --cc=arm-linux-gnueabihf-clang\
 --as=arm-linux-gnueabihf-gcc\
 --ld=arm-linux-gnueabihf-clang\
 --arch=armv6t2\
 --cpu=cortex-a7\
 --target-os=linux\
 --disable-stripping\
 --disable-thumb\
 --enable-mmal\
 --enable-rpi\
 --extra-cflags="-ggdb $RPI_KEEPS $RPI_DEFINES $RPI_INCLUDES"\
 --extra-cxxflags="$RPI_DEFINES $RPI_INCLUDES"\
 --extra-ldflags="$RPI_LIBDIRS -Wl,-rpath=/opt/vc/lib,-rpath-link=$RPI_OPT_VC/lib,-rpath=/lib,-rpath=/usr/lib,-rpath-link=$RPI_TOOLROOT/lib,-rpath-link=$RPI_TOOLROOT/lib,-rpath-link=$CLANG_TOOLROOT/lib/arm-linux-gnueabihf"\
 --extra-libs="-pthread -Wl,--start-group -lbcm_host -lmmal -lmmal_util -lmmal_core -lvcos -lvcsm -lvchostif -lvchiq_arm"\
 --cross-prefix=$RPI_TOOLROOT/bin/arm-linux-gnueabihf-

# --enable-decoder=hevc_rpi\
# --enable-extra-warnings\
# --arch=armv71\
# --enable-shared\

# gcc option for getting asm listing
# -Wa,-ahls
