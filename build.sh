cmake -S . -B build-pc \
-DBACKEND=PC \
-DCMAKE_POLICY_VERSION_MINIMUM=3.5

cmake --build build-pc
./build-pc/pc_pico
