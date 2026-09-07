#!/usr/bin/env python3
# 生成 Unicode -> GB2312 映射表（供 OSD 文本渲染用）
#
# 背景：安凯 OSD 画字接口和字库按 GB2312 码位索引，云端下发的
# 配置是 UTF-8，设备上无 iconv（uClibc 不带），故内嵌一张查找表。
#
# 用法：python3 scripts/gen_gb2312_table.py
# 输出：libs/osd/src/gb2312_unicode_table.inc（勿手改，改需求重新生成）
#
# 依赖宿主机 Python 自带 gb2312 codec，无第三方库

import os

OUT = os.path.join(os.path.dirname(__file__),
                   '..', 'libs', 'osd', 'src', 'gb2312_unicode_table.inc')

entries = []
for hi in range(0xA1, 0xF8):       # 区号 1-87
    for lo in range(0xA1, 0xFF):   # 位号 1-94
        try:
            ch = bytes([hi, lo]).decode('gb2312')
        except UnicodeDecodeError:
            continue
        if len(ch) != 1:
            continue
        entries.append((ord(ch), (hi << 8) | lo))

entries.sort()  # 按 Unicode 升序，设备端二分查找

with open(OUT, 'w') as f:
    f.write('/* 本文件由 scripts/gen_gb2312_table.py 自动生成，勿手改。\n')
    f.write(' * Unicode 码点 -> GB2312 码 的映射表（按 Unicode 升序，供二分查找）。\n')
    f.write(' * 每条：高 16 位 = Unicode 码点，低 16 位 = GB2312 两字节码\n')
    f.write(' * （高字节=区，低字节=位）。共 %d 条 */\n' % len(entries))
    f.write('static const uint32_t kUnicodeToGb2312[] = {\n')
    line = '   '
    for cp, gb in entries:
        item = '0x%04X%04X,' % (cp, gb)
        if len(line) + len(item) > 100:
            f.write(line + '\n')
            line = '   '
        line += ' ' + item
    if line.strip():
        f.write(line + '\n')
    f.write('};\n')

print('generated %d entries -> %s' % (len(entries), os.path.abspath(OUT)))
