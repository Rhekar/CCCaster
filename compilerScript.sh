#!/bin/bash

# MAKE SURE YOU DO NOT HAVE ANY i686-w64-mingw32 INSTALLED

# misc bs
sudo apt update
sudo apt install -y build-essential bison flex texinfo wget curl git gawk libgmp-dev libmpfr-dev libmpc-dev libisl-dev zlib1g-dev libzstd-dev python3

# set up file bs
mkdir -p ~/mingw-gcc14/src
mkdir -p ~/mingw-gcc14/build-binutils
mkdir -p ~/mingw-gcc14/build-gcc
# this one confuses me. i dont get it
sudo mkdir -p /opt/mingw-gcc14/include 

# build gcc and binutils
cd ~/mingw-gcc14/src
wget https://ftp.gnu.org/gnu/gcc/gcc-14.2.0/gcc-14.2.0.tar.xz
wget https://ftp.gnu.org/gnu/binutils/binutils-2.43.1.tar.xz\
tar -xf gcc-14.2.0.tar.xz
tar -xf binutils-2.43.1.tar.xz
git clone --depth 1 --branch v12.0.0 https://github.com/mingw-w64/mingw-w64.git

# build binutils
cd ~/mingw-gcc14/build-binutils
../src/binutils-2.43.1/configure --target=i686-w64-mingw32 --prefix=/opt/mingw-gcc14 --with-sysroot=/opt/mingw-gcc14/i686-w64-mingw32 --disable-nls --disable-werror
make -j$(nproc)
sudo make install

# not sure if this is needed, but im scared
export PATH=/opt/mingw-gcc14/bin:$PATH
sudo export PATH=/opt/mingw-gcc14/bin:$PATH
# if you get ranlib errors,,, idrek
# sudo has different path. fuck me
# you might have to sudo visudo and add it to the defaultpaths arg thing

cd ~/mingw-gcc14/src/mingw-w64/mingw-w64-headers
./configure --host=i686-w64-mingw32 --prefix=/opt/mingw-gcc14/i686-w64-mingw32

make
sudo make install

# build mingw
cd ~/mingw-gcc14/src/gcc-14.2.0
./contrib/download_prerequisites
cd ~/mingw-gcc14/build-gcc
../src/gcc-14.2.0/configure --target=i686-w64-mingw32 --prefix=/opt/mingw-gcc14 --with-sysroot=/opt/mingw-gcc14 --with-native-system-header-dir=/include --enable-languages=c,c++ --disable-multilib --disable-nls --disable-shared --disable-libssp --disable-libvtv --disable-libgomp --disable-libquadmath --disable-libatomic --disable-libstdcxx-pch --enable-threads=win32

make -j$(nproc) all-gcc all-target-libgcc
sudo make install-gcc install-target-libgcc

cd ~/mingw-gcc14/src/mingw-w64/mingw-w64-crt
./configure --host=i686-w64-mingw32 --prefix=/opt/mingw-gcc14/i686-w64-mingw32

make -j$(nproc)
sudo make install

# build gcc
cd ~/mingw-gcc14/build-gcc
make -j$(nproc)
sudo make install

# build this one library i need
cd ~/mingw-gcc14/src/mingw-w64/mingw-w64-libraries/winpthreads
./configure --host=i686-w64-mingw32 --prefix=/opt/mingw-gcc14/i686-w64-mingw32
make -j$(nproc)
sudo make install

