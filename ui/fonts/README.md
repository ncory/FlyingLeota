# Touchscreen fonts

LVGL bitmap fonts made from [Inter](https://github.com/rsms/inter), with a few icons from
Font Awesome 5 Free (the copy in LVGL's `scripts/built_in_font/`). Each holds printable
ASCII plus ° ± ² ³ · × – — ‘ ’ “ ” • … and −.

| File | Inter weight | Size | Icons |
| --- | --- | --- | --- |
| `leota_inter_14.c` | Regular | 14 px | |
| `leota_inter_16.c` | Regular | 16 px | |
| `leota_inter_16_semi.c` | SemiBold | 16 px | yes |
| `leota_inter_20_bold.c` | Bold | 20 px | yes |
| `leota_inter_28_bold.c` | Bold | 28 px | yes |

Icons: check, close, warning, up, down, left, right, home (LVGL's `LV_SYMBOL_OK`,
`LV_SYMBOL_CLOSE`, `LV_SYMBOL_WARNING`, `LV_SYMBOL_UP`, `LV_SYMBOL_DOWN`, `LV_SYMBOL_LEFT`,
`LV_SYMBOL_RIGHT`, `LV_SYMBOL_HOME`).

Made with lv_font_conv 1.5.2, for example:

```bash
npx lv_font_conv --bpp 4 --size 16 --no-compress --format lvgl \
  --font Inter-SemiBold.otf -r 0x20-0x7E,0xB0,0xB1,0xB2,0xB3,0xB7,0xD7,0x2013,0x2014,0x2018,0x2019,0x201C,0x201D,0x2022,0x2026,0x2212 \
  --font FontAwesome5-Solid+Brands+Regular.woff -r 0xF00C,0xF00D,0xF071,0xF077,0xF078,0xF053,0xF054,0xF015 \
  -o leota_inter_16_semi.c
```

Licences: Inter is under the SIL Open Font License 1.1 (`OFL-Inter.txt`). Font Awesome Free's
fonts are under the SIL Open Font License 1.1, and its icons under CC BY 4.0
(https://fontawesome.com/license/free).
