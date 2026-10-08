#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
生成 UnnamedBotSetup.vdproj（Visual Studio Installer Projects 工程）。

做法：拿一份「VS 能正常加载」的现成 vdproj 当骨架（SAMPLE），只替换其中的
Hierarchy / File / Folder / Shortcut 四段与产品属性。

为什么这么做：完全手写的 vdproj 会被 VS 拒绝加载（报「已取消该操作」），
即使各段看起来与样例一致；骨架工程自带的安装向导（UserInterface）、
CustomAction、MsiBootstrapper 等样板段是确定可用的，直接沿用最稳。

载荷：
  Unnamedbot.exe + 应用自带 DLL + VC 运行库（app-local，用户无需装 redist）
  + FrameworkShaders + models/Haru + models/wuwuwu + WebView2 引导程序
安装位置：%LOCALAPPDATA%\\Programs\\Unnamedbot（按用户安装，免 UAC；
          应用要在自己目录旁写 data/logs，Program Files 下会没权限）
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
CRT_SRC = r'E:\VSS\VC\Redist\MSVC\14.51.36231\x64\Microsoft.VC145.CRT'
# 骨架：同机上另一个工程的 vdproj（VS 能加载）。只借用它的样板段。
SAMPLE = r'D:\VSProject\maplecourt\AEZSetup\AEZSetup.vdproj'

PRODUCT_NAME = 'UnnamedBot'
MANUFACTURER = 'UnnamedBot'
PRODUCT_VERSION = '1.0.0'
PRODUCT_CODE = '{9F2B7C41-3E5A-4D18-9C77-5B1E0A2D6F31}'
UPGRADE_CODE = '{4A7D1E93-6C02-4B85-A1F4-8E3D2C7B5A09}'

FILE_TYPE_GUID = '{1FB2D0AE-D3B9-43D4-B9DD-F88EC61E35DE}'
FOLDER_APP_GUID = '{3C67513D-01DD-4637-8A68-80971EB9504F}'
FOLDER_SPECIAL_GUID = '{1525181F-901A-416C-8A58-119130FE478E}'
FOLDER_SUB_GUID = '{9EF0B969-E518-4E46-987F-47570745A589}'
SHORTCUT_GUID = '{970C0BB2-C7D0-45D7-ABFA-7EC378858BC0}'

APP_DLLS = ['cpr.dll', 'libcurl.dll', 'z.dll', 'sqlite3.dll', 'Live2DCubismCore.dll']
CRT_DLLS = ['msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll']   # import 表实测只需这三个
WEBVIEW2 = 'MicrosoftEdgeWebView2Setup.exe'

_used_keys = {}


def key_for(text):
    """目标路径 → 稳定的 32 位十六进制键（同一目标重复生成结果一致）"""
    k = '_' + hashlib.md5(text.encode('utf-8')).hexdigest().upper()
    if k in _used_keys and _used_keys[k] != text:
        raise SystemExit('键冲突：%s 与 %s' % (text, _used_keys[k]))
    _used_keys[k] = text
    return k


def read_text(path):
    return io.open(path, encoding='utf-8-sig', newline='').read().replace('\r\n', '\n')


def write_crlf(path, text):
    # 必须带 UTF-8 BOM：VSD 的解析器按「带 BOM 就 UTF-8，否则按 CodePage(936/GBK)」
    # 判断编码，不带 BOM 时它会把 UTF-8 的中文串按 GBK 解，随后解析失败并报
    # 「已取消该操作」。VS 自己保存的 vdproj 全都是带 BOM 的。
    io.open(path, 'w', encoding='utf-8-sig', newline='').write(text.replace('\n', '\r\n'))


def span_of(text, name, indent=8):
    m = re.search(r'\n' + ' ' * indent + '"' + name + r'"\s*\n', text)
    if not m:
        return None
    start = text.index('{', m.end())
    depth = 0
    for i in range(start, len(text)):
        if text[i] == '{':
            depth += 1
        elif text[i] == '}':
            depth -= 1
            if depth == 0:
                return m.start() + 1, i + 1
    return None


def replace_section(text, name, new_block, indent=8):
    s = span_of(text, name, indent)
    if not s:
        raise SystemExit('骨架里找不到 %s 段' % name)
    return text[:s[0]] + new_block + text[s[1]:]


def prepare_redist():
    """VC 运行库拷进工程目录，安装工程自包含"""
    os.makedirs(os.path.join(REDIST, 'crt'), exist_ok=True)
    for name in CRT_DLLS:
        src = os.path.join(CRT_SRC, name)
        if not os.path.isfile(src):
            raise SystemExit('缺少 VC 运行库文件：' + src)
        with open(src, 'rb') as fsrc, open(os.path.join(REDIST, 'crt', name), 'wb') as fdst:
            fdst.write(fsrc.read())
    return list(CRT_DLLS)


def collect_payload():
    """[(源文件绝对路径, 目标子目录元组)]，子目录 () = 安装根目录"""
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
            raise SystemExit('缺少构建产物：' + p + '（先构建 Unnamedbot）')
        items.append((p, ()))
    for name in CRT_DLLS:
        items.append((os.path.join(REDIST, 'crt', name), ()))
    items.append((os.path.join(REDIST, WEBVIEW2), ('redists',)))

    add_dir(os.path.join(REL, 'FrameworkShaders'), ('FrameworkShaders',))
    for model in ('Haru', 'wuwuwu'):
        add_dir(os.path.join(APP, 'models', model), ('models', model))
    return items


def render(items):
    app_key = key_for('$appdir')
    menu_key = key_for('$programmenu')
    desk_key = key_for('$desktop')

    subfolders = sorted({parts for _src, parts in items if parts})
    # 只收集「文件所在目录」是不够的：models/Haru/... 这类深层路径的中间层
    # （models、models/Haru）本身也是目标文件夹，漏掉就会让文件引用了不存在的
    # 文件夹键 —— VS 加载时会直接取消，报「已取消该操作」。
    all_dirs = set()
    for parts in subfolders:
        for depth in range(len(parts)):
            all_dirs.add(parts[:depth + 1])
    all_dirs = sorted(all_dirs)
    folder_keys = {}
    for parts in all_dirs:
        folder_keys[parts] = key_for('dir:' + '/'.join(parts))

    hierarchy = []
    files = []
    exe_key = None
    for src, parts in items:
        name = os.path.basename(src)
        target = '/'.join(parts + (name,))
        k = key_for(target)
        if name.lower() == 'unnamedbot.exe':
            exe_key = k
        folder_key = app_key if not parts else folder_keys[parts]
        files.append(
            '            "%s:%s"\n'
            '            {\n'
            '            "SourcePath" = "8:%s"\n'
            '            "TargetName" = "8:%s"\n'
            '            "Tag" = "8:"\n'
            '            "Folder" = "8:%s"\n'
            '            "Condition" = "8:"\n'
            '            "Transitive" = "11:FALSE"\n'
            '            "Vital" = "11:TRUE"\n'
            '            "ReadOnly" = "11:FALSE"\n'
            '            "Hidden" = "11:FALSE"\n'
            '            "System" = "11:FALSE"\n'
            '            "Permanent" = "11:FALSE"\n'
            '            "SharedLegacy" = "11:FALSE"\n'
            '            "PackageAs" = "3:1"\n'
            '            "Register" = "3:1"\n'
            '            "Exclude" = "11:FALSE"\n'
            '            "IsDependency" = "11:FALSE"\n'
            '            "IsolateTo" = "8:"\n'
            '            }\n'
            % (FILE_TYPE_GUID, k, src.replace('\\', '\\\\'), name, folder_key))
        hierarchy.append(
            '        "Entry"\n        {\n'
            '        "MsmKey" = "8:%s"\n'
            '        "OwnerKey" = "8:_UNDEFINED"\n'
            '        "MsmSig" = "8:_UNDEFINED"\n'
            '        }\n' % k)

    def render_folder(parts):
        key = app_key if not parts else folder_keys[parts]
        children = [p for p in all_dirs if len(p) == len(parts) + 1 and p[:len(parts)] == parts]
        inner = ''
        for child in sorted(children):
            inner += ('                    "%s:%s"\n'
                      '                    {\n'
                      '                    "Name" = "8:%s"\n'
                      '                    "AlwaysCreate" = "11:FALSE"\n'
                      '                    "Condition" = "8:"\n'
                      '                    "Transitive" = "11:FALSE"\n'
                      '                    "Property" = "8:%s"\n'
                      '                        "Folders"\n'
                      '                        {\n'
                      '%s'
                      '                        }\n'
                      '                    }\n'
                      % (FOLDER_SUB_GUID, folder_keys[child], child[-1],
                         key_for('propid:' + '/'.join(child)), render_folder(child)))
        return inner

    folders = (
        '            "%s:%s"\n'
        '            {\n'
        '            "Name" = "8:#1916"\n'
        '            "AlwaysCreate" = "11:FALSE"\n'
        '            "Condition" = "8:"\n'
        '            "Transitive" = "11:FALSE"\n'
        '            "Property" = "8:DesktopFolder"\n'
        '                "Folders"\n'
        '                {\n'
        '                }\n'
        '            }\n'
        '            "%s:%s"\n'
        '            {\n'
        '            "Name" = "8:#1919"\n'
        '            "AlwaysCreate" = "11:FALSE"\n'
        '            "Condition" = "8:"\n'
        '            "Transitive" = "11:FALSE"\n'
        '            "Property" = "8:ProgramMenuFolder"\n'
        '                "Folders"\n'
        '                {\n'
        '                }\n'
        '            }\n'
        '            "%s:%s"\n'
        '            {\n'
        '            "DefaultLocation" = "8:[LocalAppDataFolder]Programs\\\\%s"\n'
        '            "Name" = "8:#1925"\n'
        '            "AlwaysCreate" = "11:FALSE"\n'
        '            "Condition" = "8:"\n'
        '            "Transitive" = "11:FALSE"\n'
        '            "Property" = "8:TARGETDIR"\n'
        '                "Folders"\n'
        '                {\n'
        '%s'
        '                }\n'
        '            }\n'
        % (FOLDER_SPECIAL_GUID, desk_key, FOLDER_SPECIAL_GUID, menu_key,
           FOLDER_APP_GUID, app_key, PRODUCT_NAME, render_folder(())))

    shortcuts = ''
    for label, folder in (('StartMenu', menu_key), ('Desktop', desk_key)):
        shortcuts += (
            '            "%s:%s"\n'
            '            {\n'
            '            "Name" = "8:%s"\n'
            '            "Arguments" = "8:"\n'
            '            "Description" = "8:%s"\n'
            '            "ShowCmd" = "3:1"\n'
            '            "IconIndex" = "3:0"\n'
            '            "Transitive" = "11:FALSE"\n'
            '            "Target" = "8:%s"\n'
            '            "Folder" = "8:%s"\n'
            '            "WorkingFolder" = "8:%s"\n'
            '            "Icon" = "8:"\n'
            '            "Feature" = "8:"\n'
            '            }\n'
            % (SHORTCUT_GUID, key_for('lnk:' + label), PRODUCT_NAME,
               'UnnamedBot 桌宠中控', exe_key, folder, app_key))
    return ''.join(hierarchy), ''.join(files), folders, shortcuts, exe_key


def main():
    for path in (REL, REDIST, SAMPLE):
        if not os.path.exists(path):
            raise SystemExit('路径不存在：' + path)
    prepare_redist()
    items = collect_payload()
    hierarchy, files, folders, shortcuts, _exe = render(items)

    skeleton = read_text(SAMPLE)
    skeleton = replace_section(skeleton, 'Hierarchy',
                               '\n    "Hierarchy"\n    {\n' + hierarchy + '    }\n', 4)
    skeleton = replace_section(skeleton, 'File', '\n        "File"\n        {\n' + files + '        }\n')
    skeleton = replace_section(skeleton, 'Folder', '\n        "Folder"\n        {\n' + folders + '        }\n')
    skeleton = replace_section(skeleton, 'Shortcut',
                               '\n        "Shortcut"\n        {\n' + shortcuts + '        }\n')
    skeleton = replace_section(skeleton, 'Registry', '\n        "Registry"\n        {\n        }\n')
    # 骨架里还有指向它自己工程的 ProjectOutput（SourcePath = ..\Debug\maplecourt.exe，
    # 而且带一个不属于本方案的 OutputProjectGuid）。这类残留会让 VSD 在加载阶段
    # 解析失败并直接取消（「已取消该操作」）。MergeModule / CustomAction 同理，一并清空。
    for section in ('ProjectOutput', 'MergeModule', 'CustomAction'):
        skeleton = replace_section(skeleton, section,
                                   '\n        "%s"\n        {\n        }\n' % section)

    m = re.search(r'\n        "Product"\n        \{.*?\n        \}\n', skeleton, re.S)
    if not m:
        raise SystemExit('骨架里找不到 Product 段')
    p = m.group(0)

    def setval(text, key, value):
        return re.sub(r'("' + re.escape(key) + r'"\s*=\s*)"8:[^"]*"',
                      lambda mm: mm.group(1) + '"8:' + value + '"', text, count=1)

    for key, val in (('ProductName', PRODUCT_NAME), ('ProductCode', PRODUCT_CODE),
                     ('UpgradeCode', UPGRADE_CODE), ('ProductVersion', PRODUCT_VERSION),
                     ('Manufacturer', MANUFACTURER), ('Title', PRODUCT_NAME),
                     ('ARPCONTACT', MANUFACTURER), ('ARPCOMMENTS', 'AI 桌宠中控'),
                     ('ARPPRODUCTICON', '')):
        p = setval(p, key, val)
    p = p.replace('"InstallAllUsers" = "11:TRUE"', '"InstallAllUsers" = "11:FALSE"')
    p = p.replace('"RemovePreviousVersions" = "11:FALSE"', '"RemovePreviousVersions" = "11:TRUE"')
    skeleton = skeleton.replace(m.group(0), p, 1)

    skeleton = skeleton.replace('"OutputFilename" = "8:AEZSetup.msi"',
                                '"OutputFilename" = "8:UnnamedBotSetup.msi"')

    target = os.path.join(SETUP, 'UnnamedBotSetup.vdproj')
    write_crlf(target, skeleton)
    total = sum(os.path.getsize(src) for src, _ in items)
    print('已生成 %s' % target)
    print('  文件 %d 个（%.1f MB）' % (len(items), total / 1048576.0))
    print('  安装位置：[LocalAppDataFolder]Programs\\%s（按用户安装，免 UAC）' % PRODUCT_NAME)


if __name__ == '__main__':
    main()
