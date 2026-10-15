# 第三方组件

本项目的转换引擎、界面和命令行源码使用 MIT 许可证，见 `LICENSE`。
下列通用依赖保留各自的版权与许可，不属于本项目原创内容。

## Qt

Windows 程序使用动态链接的 Qt Core、Gui、Widgets 及 Windows 平台插件。
当前构建采用未修改的 Qt 6.8.3，按 LGPL-3.0 使用；DLL 与程序分开放置，允许替换兼容的 Qt 库。
构建步骤见 README。Qt 的 LGPL、GPL 及所包含组件的许可证文本位于 `licenses/qt/`。

- 对应源码：[Qt Base 6.8.3](https://github.com/qt/qtbase/tree/v6.8.3)
- 源码归档：[qtbase-everywhere-src-6.8.3.tar.xz](https://download.qt.io/archive/qt/6.8/6.8.3/submodules/qtbase-everywhere-src-6.8.3.tar.xz)
- 第三方版权说明：`licenses/qt/THIRD-PARTY-NOTICES.txt`

版权说明覆盖 Qt Base 的源码发行内容，其中部分组件并未包含在本工具的程序包中。

## Microsoft Visual C++ 运行库

Windows 程序使用 Microsoft Visual C++ 运行库。程序包附带的运行库文件受 Microsoft 许可约束，
相应条款位于 `licenses/Microsoft-Visual-C-Runtime-2015-2022-License.txt`。

## Live2D

Live2D、Cubism 及相关格式名称属于各自权利人。本项目不隶属于 Live2D。
程序包不包含 Live2D SDK、Cubism Editor、第三方转换器或用户模型。
