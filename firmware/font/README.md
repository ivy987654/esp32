# 字体

固件从独立的 flash 分区（`font`，偏移 `0x400000`）直接读取字体，不占内存。仓库**不带字体文件**，请自己生成：

1. 下载开源字体 [霞鹜文楷 LXGW WenKai](https://github.com/lxgw/LxgwWenKai/releases)（作者用的是 `LXGWWenKai-Medium.ttf`，SIL OFL 协议）。
2. 裁成常用字子集（GB2312 + Big5 一级字 + 假名 + 常用符号，约 4.8 MB）：

   ```bash
   pip install fonttools
   python tools/mkfont2.py LXGWWenKai-Medium.ttf font/font.ttf
   python tools/mkfontbin.py font/font.ttf font/font.bin
   ```

3. 烧进字体分区（只需一次，换字体时再烧）：

   ```bash
   ~/.platformio/packages/tool-esptool/esptool --chip esp32s3 \
     --port /dev/cu.usbserial-XXXX --baud 460800 write-flash -z 0x400000 font/font.bin
   ```

没烧字体时屏幕不会显示文字，串口日志会提示 `font partition missing`。

> 想换别的字体也可以，必须是 TrueType 轮廓（`glyf`）的 `.ttf`。思源黑体这类 CFF 轮廓的 `.otf` 先用 `tools/otf2ttf.py` 转换。不要用 macOS 系统字体（苹方、冬青黑体等），它们不允许再分发。
