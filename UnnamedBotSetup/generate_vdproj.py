#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
生成 UnnamedBotSetup.vdproj（Visual Studio Installer Projects 工程）—— 用 VS2026 插件构建。

以 VS 自己写出的 vdproj（skeleton.vdproj）为骨架，只做外科式增改，绝不整段替换结构。
实测踩过的坑（都已在本文件里规避）：
 1. 文件必须 UTF-8 带 BOM；
 2. 整段替换 Hierarchy/File/Folder/Shortcut → VSD 拒绝加载（「已取消该操作」）；
 3. 不能删样板带过来的条目（快捷方式 Icon 等仍引用它们，删了就悬空 → 同样拒绝加载），
    只能**重指向**（改 SourcePath）；
 4. ProjectOutput 段不能清空（拒绝加载），把它的子项改写成普通文件项即可；
 5. 快捷方式的 Icon 不能指向 .exe（构建校验报 Unable to build shortcut），留空即可；
 6. 新子目录必须插进 Application Folder 自己的 Folders 块 —— 锚点要对准
    "Property" = "8:TARGETDIR"，否则会插进 images\\gen 这类深层空块，
    模型/着色器会被装到 images\\gen\\models\\... 而不是安装根目录。

载荷：Unnamedbot.exe + 应用 DLL + VC 运行库(app-local) + FrameworkShaders
      + models/Haru + models/wuwuwu + WebView2 引导程序 + 安装说明
安装位置：%LOCALAPPDATA%\\Programs\\UnnamedBot（按用户安装，免 UAC）
"""

import hashlib
import io
import os
import re

REPO = r'D:\VSProject\Unnamedbot'
APP = os.path.join(REPO, 'Unnamedbot')
REL = os.path.join(APP, 'x64', 'Release')
SETUP = os.path.join(REPO, 'UnnamedBotSetup')
REDIST = os.path.join(SETUP, 'redist')
SKELETON = os.path.join(SETUP, 'skeleton.vdproj')
CRT_SRC = r'E:\VSS\VC\Redist\MSVC\14.51.36231\x64\Microsoft.VC145.CRT'

PRODUCT_NAME = 'UnnamedBot'
MANUFACTURER = 'UnnamedBot'
PRODUCT_VERSION = '1.0.0'
PRODUCT_CODE = '{9F2B7C41-3E5A-4D18-9C77-5B1E0A2D6F31}'
UPGRADE_CODE = '{4A7D1E93-6C02-4B85-A1F4-8E3D2C7B5A09}'

FILE_GUID = '{1FB2D0AE-D3B9-43D4-B9DD-F88EC61E35DE}'
SUB_GUID = '{9EF0B969-E518-4E46-987F-47570745A589}'
SHORTCUT_GUID = '{970C0BB2-C7D0-45D7-ABFA-7EC378858BC0}'
PROJOUT_GUID = '{5259A561-127C-4D43-A0A1-72F10C7B3BF8}'

APP_DLLS = ['cpr.dll', 'libcurl.dll', 'z.dll', 'sqlite3.dll', 'Live2DCubismCore.dll']
CRT_DLLS = ['msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll']
WEBVIEW2 = 'MicrosoftEdgeWebView2Setup.exe'
PLACEHOLDER = 'placeholder.txt'


def key_for(text):
    return '_' + hashlib.md5(('unnamedbot:' + text).encode('utf-8')).hexdigest().upper()


def read_text(path):
    return io.open(path, encoding='utf-8-sig', newline='').read().replace('\r\n', '\n')


def write_bom(path, text):
    io.open(path, 'w', encoding='utf-8-sig', newline='').write(text.replace('\n', '\r\n'))


def block_range(t, at):
    depth = 0
    for i in range(at, len(t)):
        if t[i] == '{':
            depth += 1
        elif t[i] == '}':
            depth -= 1
            if depth == 0:
                return i + 1
    raise SystemExit('大括号不配对')


def prepare_files():
    os.makedirs(os.path.join(REDIST, 'crt'), exist_ok=True)
    for name in CRT_DLLS:
        src = os.path.join(CRT_SRC, name)
        if not os.path.isfile(src):
            raise SystemExit('缺少 VC 运行库：' + src)
        with open(src, 'rb') as a, open(os.path.join(REDIST, 'crt', name), 'wb') as b:
            b.write(a.read())
    ph = os.path.join(REDIST, PLACEHOLDER)
    io.open(ph, 'w', encoding='utf-8', newline='').write('UnnamedBot 安装包占位文件（随 MSI 打包，可忽略）。\r\n')
    return ph


def collect_payload(placeholder):
    items = []

    def add_dir(src_dir, sub):
        for root, _dirs, files in os.walk(src_dir):
            for fn in sorted(files):
                full = os.path.join(root, fn)
                rel = os.path.relpath(full, src_dir)
                items.append((full, tuple(sub) + tuple(rel.replace('\\', '/').split('/')[:-1])))

    for name in ['Unnamedbot.exe'] + APP_DLLS:
        p = os.path.join(REL, name)
        if not os.path.isfile(p):
            raise SystemExit('缺少构建产物：' + p)
        items.append((p, ()))
    for name in CRT_DLLS:
        items.append((os.path.join(REDIST, 'crt', name), ()))
    items.append((os.path.join(REDIST, WEBVIEW2), ('redists',)))
    add_dir(os.path.join(REL, 'FrameworkShaders'), ('FrameworkShaders',))
    for model in ('Haru', 'wuwuwu'):
        add_dir(os.path.join(APP, 'models', model), ('models', model))
    items.append((placeholder, ()))
    return items


def main():
    for path in (REL, REDIST, SKELETON):
        if not os.path.exists(path):
            raise SystemExit('路径不存在：' + path)

    placeholder = prepare_files()
    items = collect_payload(placeholder)
    payload = [(s, p) for s, p in items if os.path.basename(s) != PLACEHOLDER]
    text = read_text(SKELETON)

    app_folder = re.search(r'\{3C67513D[^}]*\}:(_[0-9A-F]{32})"', text).group(1)

    # 1) 样板残留文件条目 → 重指向占位文件（保留条目，避免悬空）
    n_repoint, pos = 0, 0
    while True:
        m = re.search(r'\n[ \t]*"' + re.escape(FILE_GUID) + r':(_[0-9A-F]{32})"\n[ \t]*\{', text[pos:])
        if not m:
            break
        start = pos + m.start()
        end = block_range(text, pos + m.end() - 1)
        blk = text[start:end]
        sp = re.search(r'"SourcePath" = "8:([^"]*)"', blk)
        if sp and 'unnamedbot' not in sp.group(1).lower():
            blk = re.sub(r'("SourcePath" = "8:)[^"]*"',
                         lambda mm: mm.group(1) + placeholder.replace('\\', '\\\\') + '"', blk, count=1)
            text = text[:start] + blk + text[end:]
            pos = start + len(blk)
            n_repoint += 1
            continue
        pos = end
    print('样板残留文件条目重指向占位文件：%d 条' % n_repoint)

    # 2) ProjectOutput 子项 → README 文件项（不能清空该段）
    readme = os.path.join(SETUP, 'README-安装说明.txt')
    io.open(readme, 'w', encoding='utf-8-sig', newline='').write(
        'UnnamedBot 桌宠中控\r\n====================\r\n\r\n'
        '安装位置：%LOCALAPPDATA%\\Programs\\UnnamedBot（按用户安装，无需管理员权限）\r\n\r\n'
        'Unnamedbot.exe 主程序；cpr/libcurl/z/sqlite3 为 HTTP 与数据库运行时；\r\n'
        'Live2DCubismCore.dll Live2D 运行库；msvcp140/vcruntime140* VC++ 运行库（随包）；\r\n'
        'FrameworkShaders 渲染着色器；models\\Haru、models\\wuwuwu 两个 Live2D 模型；\r\n'
        'redists\\MicrosoftEdgeWebView2Setup.exe WebView2 引导程序（系统未自带时运行）。\r\n\r\n'
        '卸载：设置 → 应用 → 已安装的应用 → UnnamedBot\r\n')
    po = re.search(r'\n        "ProjectOutput"\n        \{\n', text)
    inner = text.index('\n', text.index('{', po.end() - 2)) + 1
    ch = re.search(r'[ \t]*"' + re.escape(PROJOUT_GUID) + r'[^\n]*"\n[ \t]*\{', text[inner:])
    cs = inner + ch.start()
    ce = block_range(text, inner + ch.end() - 1)
    text = text[:cs] + ('            "%s:%s"\n            {\n'
                        '            "SourcePath" = "8:%s"\n'
                        '            "TargetName" = "8:README-安装说明.txt"\n'
                        '            "Tag" = "8:"\n            "Folder" = "8:%s"\n'
                        '            "Condition" = "8:"\n            "Transitive" = "11:FALSE"\n'
                        '            "Vital" = "11:TRUE"\n            "ReadOnly" = "11:FALSE"\n'
                        '            "Hidden" = "11:FALSE"\n            "System" = "11:FALSE"\n'
                        '            "Permanent" = "11:FALSE"\n            "SharedLegacy" = "11:FALSE"\n'
                        '            "PackageAs" = "3:1"\n            "Register" = "3:1"\n'
                        '            "Exclude" = "11:FALSE"\n            "IsDependency" = "11:FALSE"\n'
                        '            "IsolateTo" = "8:"\n            }\n'
                        % (FILE_GUID, key_for('file:README'), readme.replace('\\', '\\\\'), app_folder)) + text[ce:]

    # 3) 新子目录插进 Application Folder 自己的 Folders 块（TARGETDIR）
    all_dirs = sorted({p[:d + 1] for _s, p in payload for d in range(len(p))})
    folder_key = {p: key_for('dir:' + '/'.join(p)) for p in all_dirs}

    def render_sub(parts, indent):
        out = ''
        for child in [p for p in all_dirs if len(p) == len(parts) + 1 and p[:len(parts)] == parts]:
            out += ('%s"%s:%s"\n%s{\n' % (' ' * indent, SUB_GUID, folder_key[child], ' ' * indent)
                    + '%s"Name" = "8:%s"\n' % (' ' * (indent + 4), child[-1])
                    + '%s"AlwaysCreate" = "11:FALSE"\n' % (' ' * (indent + 4))
                    + '%s"Condition" = "8:"\n' % (' ' * (indent + 4))
                    + '%s"Transitive" = "11:FALSE"\n' % (' ' * (indent + 4))
                    + '%s"Property" = "8:%s"\n' % (' ' * (indent + 4), key_for('prop:' + '/'.join(child)))
                    + '%s"Folders"\n%s{\n%s%s}\n%s}\n'
                    % (' ' * (indent + 8), ' ' * (indent + 8), render_sub(child, indent + 12),
                       ' ' * (indent + 8), ' ' * indent))
        return out

    m = re.search(r'("Property" = "8:TARGETDIR"\n[ \t]*"Folders"\n[ \t]*\{\n)([ \t]*)', text)
    if not m:
        raise SystemExit('找不到 Application Folder 的 Folders 块（TARGETDIR）')
    text = text[:m.end(1)] + render_sub((), len(m.group(2)) + 4) + text[m.end(1):]

    # 4) 追加文件项 + Hierarchy
    files, hier, exe_key = [], [], None
    for src, parts in payload:
        name = os.path.basename(src)
        k = key_for('file:' + '/'.join(parts + (name,)))
        if name.lower() == 'unnamedbot.exe':
            exe_key = k
        fk = app_folder if not parts else folder_key[parts]
        files.append('            "%s:%s"\n            {\n'
                     '            "SourcePath" = "8:%s"\n            "TargetName" = "8:%s"\n'
                     '            "Tag" = "8:"\n            "Folder" = "8:%s"\n'
                     '            "Condition" = "8:"\n            "Transitive" = "11:FALSE"\n'
                     '            "Vital" = "11:TRUE"\n            "ReadOnly" = "11:FALSE"\n'
                     '            "Hidden" = "11:FALSE"\n            "System" = "11:FALSE"\n'
                     '            "Permanent" = "11:FALSE"\n            "SharedLegacy" = "11:FALSE"\n'
                     '            "PackageAs" = "3:1"\n            "Register" = "3:1"\n'
                     '            "Exclude" = "11:FALSE"\n            "IsDependency" = "11:FALSE"\n'
                     '            "IsolateTo" = "8:"\n            }\n'
                     % (FILE_GUID, k, src.replace('\\', '\\\\'), name, fk))
        hier.append('        "Entry"\n        {\n        "MsmKey" = "8:%s"\n'
                    '        "OwnerKey" = "8:_UNDEFINED"\n        "MsmSig" = "8:_UNDEFINED"\n        }\n' % k)

    m = re.search(r'\n        "File"\n        \{\n', text)
    text = text[:m.end()] + ''.join(files) + text[m.end():]
    m = re.search(r'\n    "Hierarchy"\n    \{\n', text)
    text = text[:m.end()] + ''.join(hier) + text[m.end():]

    # 5) 样板两条快捷方式：改名、指向 exe、Icon 留空
    n_sc = 0
    for m in list(re.finditer(r'\n[ \t]*"' + re.escape(SHORTCUT_GUID) + r':(_[0-9A-F]{32})"\n[ \t]*\{', text)):
        start = m.start()
        end = block_range(text, m.end() - 1)
        blk = text[start:end]
        blk = re.sub(r'("Name" = "8:)[^"]*"', lambda mm: mm.group(1) + PRODUCT_NAME + '"', blk, count=1)
        blk = re.sub(r'("Description" = "8:)[^"]*"', lambda mm: mm.group(1) + PRODUCT_NAME + ' 桌宠中控"', blk, count=1)
        blk = re.sub(r'("Target" = "8:)[^"]*"', lambda mm: mm.group(1) + exe_key + '"', blk, count=1)
        blk = re.sub(r'("WorkingFolder" = "8:)[^"]*"', lambda mm: mm.group(1) + app_folder + '"', blk, count=1)
        blk = re.sub(r'("Icon" = "8:)[^"]*"', lambda mm: mm.group(1) + '"', blk, count=1)
        text = text[:start] + blk + text[end:]
        n_sc += 1
    print('快捷方式已改名并指向 exe：%d 条' % n_sc)

    # 6) 清空 Registry（样板写 HKLM）
    m = re.search(r'\n        "Registry"\n        \{\n', text)
    s0 = text.index('{', m.end() - 2)
    text = text[:m.start() + 1] + '        "Registry"\n        {\n        }\n' + text[block_range(text, s0):]

    # 7) 安装位置与产品属性
    text = text.replace('"DefaultLocation" = "8:[ProgramFilesFolder][Manufacturer]\\\\[ProductName]"',
                        '"DefaultLocation" = "8:[LocalAppDataFolder]Programs\\\\%s"' % PRODUCT_NAME)

    def setval(t, key, val):
        return re.sub(r'("' + re.escape(key) + r'"\s*=\s*)"8:[^"]*"',
                      lambda mm: mm.group(1) + '"8:' + val + '"', t, count=1)

    # 只改 Product 段里的键：文件里更早的地方（前置条件 BootstrapperCfg 的 Items）
    # 也有 ProductCode，全局替换会改错地方 —— 实测就是这样把样板的产品码带了出来。
    pm = re.search(r'\n        "Product"\n        \{\n', text)
    pstart = pm.end()
    pend = block_range(text, text.index('{', pstart) - 1)
    prod = text[pstart:pend]
    for k, v in (('ProductName', PRODUCT_NAME), ('ProductCode', PRODUCT_CODE),
                 ('UpgradeCode', UPGRADE_CODE), ('ProductVersion', PRODUCT_VERSION),
                 ('Manufacturer', MANUFACTURER), ('Title', PRODUCT_NAME),
                 ('ARPCONTACT', MANUFACTURER), ('ARPCOMMENTS', 'AI 桌宠中控'),
                 ('ARPPRODUCTICON', '')):
        prod = setval(prod, k, v)
    prod = prod.replace('"InstallAllUsers" = "11:TRUE"', '"InstallAllUsers" = "11:FALSE"')
    prod = prod.replace('"RemovePreviousVersions" = "11:FALSE"', '"RemovePreviousVersions" = "11:TRUE"')
    text = text[:pstart] + prod + text[pend:]
    text = text.replace('"OutputFilename" = "8:AEZSetup.msi"', '"OutputFilename" = "8:UnnamedBotSetup.msi"')

    target = os.path.join(SETUP, 'UnnamedBotSetup.vdproj')
    write_bom(target, text)
    total = sum(os.path.getsize(s) for s, _ in payload)
    print('已生成 %s' % target)
    print('  文件 %d 个（%.1f MB）· 子目录 %d 个' % (len(payload), total / 1048576.0, len(all_dirs)))
    print('  安装位置：[LocalAppDataFolder]Programs\\%s（按用户安装，免 UAC）' % PRODUCT_NAME)


if __name__ == '__main__':
    main()
