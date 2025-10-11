cd /root/nvr-gui-vaapi
make clean
mkdir -p build && cd build
cmake ..
make -j$(nproc)