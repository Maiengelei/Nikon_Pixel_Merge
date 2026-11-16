# Nikon Pixel Merge

## 功能

iPhone 应用从“文件”读取 Nikon Z 8 的 FX 全尺寸、14 位无损压缩 NEF 像素位移序列，在前台合成并导出一张 NEFX，支持镜头色差校正。4、8 帧输出 8256 × 5504，16、32 帧输出 16512 × 11008。应用按内存余量、热状态和低电量模式调整并发与默认选组，32 帧由用户手动选择并确认。

## 构建

使用 Xcode 16 或更新版本打开 `Nikon Pixel Merge iOS.xcodeproj`，选择 `Nikon Pixel Merge iOS` Scheme。部署目标为 iOS 17 或更新版本。选择 iPhone 模拟器后按 `⌘R` 运行，真机调试需在应用 Target 的 `Signing & Capabilities` 中选择开发团队。

## 第三方组件

应用仅使用系统框架和 C++ 标准库。可选校验程序 `tests/libraw_check.cpp` 使用 LibRaw 0.22 或更高版本核对 NEF 和 NEFX 的解码值。
