# OpenUSBCam
在rk3588中使用ffmpeg库文件实现读取usb摄像头并存储视频
```
cd ~ FFMPEGUSB
mkdir build
cd build
cmake ..
make -j4
./MyFFmpegProject /dev/video0 0.mp4
```
