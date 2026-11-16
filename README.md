# Nikon Pixel Merge

## 功能

C++17 核心将 Nikon Z 8 的 FX 全尺寸、14 位无损压缩 NEF 像素位移序列合成为一张线性 RGB NEFX。它按相机元数据分组，检查缺帧、重复帧和曝光设置，并支持按镜头配置校正色差。

| 输入帧数 | 输出尺寸 |
| --- | --- |
| 4、8 | 8256 × 5504 |
| 16、32 | 16512 × 11008 |

## 构建

在 macOS 安装 Apple Command Line Tools 后，从仓库根目录构建静态库。扫描入口为 `npm::scan_groups`，合成入口为 `npm::export_group`。

```sh
mkdir -p build
(cd build && c++ -std=c++17 -O2 -pthread -I../src -c ../src/*.cpp && ar rcs libnikon_merge.a groups.o raw.o merge.o output.o job.o)
```

Mac 应用和命令行程序在 `Nikon_Pixel_Merge_Mac` 分支，iPhone 应用在 `Nikon_Pixel_Merge_IOS` 分支。

## 第三方组件

核心仅依赖 C++ 标准库和系统 API。可选校验程序 `tests/libraw_check.cpp` 使用 LibRaw 0.22 或更高版本核对 NEF 和 NEFX 的解码值。
