# Nikon Pixel Merge

## 功能

Mac 应用与命令行工具 `nikon-merge` 将 Nikon Z 8 的像素位移序列合成为一张 14 位线性 RGB NEFX。支持 FX 全尺寸、14 位无损压缩 NEF，按相机元数据恢复帧序，检查缺帧、重复帧和曝光冲突，并可按镜头配置校正色差。

| 输入帧数 | 输出尺寸 |
| --- | --- |
| 4、8 | 8256 × 5504 |
| 16、32 | 16512 × 11008 |

## 构建

目标平台为搭载 Apple 芯片、运行 macOS 13 或更新版本的 Mac。在 Xcode 打开 `Nikon_Pixel_Merge.xcodeproj`，选择 `Nikon_Pixel_Merge` Scheme 和 `My Mac`，按 `⌘R` 运行。

命令行构建使用 Apple Command Line Tools，以下命令在仓库根目录执行。

```sh
make -j        # 构建应用和 CLI，产物位于 build/
make test      # 运行分组与合成测试
make package   # 生成本地签名的应用 ZIP
```

## 第三方组件

应用与 CLI 仅使用系统框架和 C++ 标准库。可选的解码校验程序 `tests/libraw_check.cpp` 使用 LibRaw 0.22 或更高版本。`tools/compare_display.py` 调用其 `dcraw_emu` 工具生成显影对照图。
