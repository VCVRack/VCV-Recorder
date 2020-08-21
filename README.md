# VCV Recorder

Requires [NASM](https://www.nasm.us/index.php) or [Yasm](https://yasm.tortall.net/) to build.

### Patented encoders

All currently-patented encoders are disabled by default and not distributed in binary form on the [VCV Library](https://library.vcvrack.com/VCV-Recorder).

To enable H.264 and AAC, build with
```bash
ENABLE_H264=1 make dep
ENABLE_H264=1 make
```
