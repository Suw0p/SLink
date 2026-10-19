# Third-party notices

SLink 使用以下第三方组件：

- libssh2
- OpenSSL
- libsodium
- SQLite
- zlib
- libvterm
- Scintilla
- Lexilla

对应版本和许可文本保存在 [`3rd/include/LICENSES`](3rd/include/LICENSES)。
libvterm 0.3.3 从 [`3rd/libvterm`](3rd/libvterm) 中的源码构建，并带有
SLink 的终端缩放边界修复；其许可证同时保存在该源码目录。

`3rd/lib` 中的静态库和 DLL 仅用于编译或运行 SLink，不改变各组件
原有许可证和版权声明。本文件仅作汇总，不替代各组件的原始许可文本。
