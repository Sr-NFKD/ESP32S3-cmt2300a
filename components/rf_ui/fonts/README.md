# 菜单字体

`rf_font_menu_32.c` 是 32 像素、4 bpp 的菜单字库，只包含“接收发射干扰调试设置亮度休眠唤醒重置关于”，由 LVGL 官方 lv_font_conv 1.5.3 从 Noto Sans SC Medium 子集转换。

源文件 `menu-source.woff2` 来自 Google Fonts 的 Noto Sans SC text 子集接口；许可见同目录 `OFL.txt`。转换后的字库名为 `rf_font_menu_32`。

`rf_font_debug_22.c` 是 22 像素的收发、检测和调试状态字库，包含这些页面所需中文以及 ASCII 0x20～0x7E。Google Fonts text 子集也必须包含这些 ASCII 字符，否则仅向转换器传 --range 并不能补出源字体中没有的字形。

重新生成（先在工具目录安装 lv_font_conv 1.5.3）：

```text
node simulator/.tools/font-tools/node_modules/lv_font_conv/lv_font_conv.js --font components/rf_ui/fonts/menu-source.woff2 --symbols "接收发射干扰调试设置亮度休眠唤醒重置关于" --size 32 --bpp 4 --format lvgl --no-compress --lv-font-name rf_font_menu_32 -o components/rf_ui/fonts/rf_font_menu_32.c
```

新增中文菜单文字时，需要重新获取包含新字的字体子集并转换；当前子集不包含其他汉字。
