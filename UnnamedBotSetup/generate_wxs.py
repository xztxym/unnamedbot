#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
生成 UnnamedBotSetup.wxs（WiX v3）并给出可编译的 MSI 工程。

为什么用 WiX：VS 的 Installer Projects（vdproj）在这台机器上命令行构建不可用
（生成的 vdproj 会被 VSD 以「已取消该操作」拒绝加载，见 UnnamedBotSetup.vdproj
与 generate_vdproj.py 的说明），而 WiX 的命令行工具链可以从 NuGet 取到，
构建与验证都能自动化。

要点：
  * 按用户安装 → %LOCALAPPDATA%\\Programs\\UnnamedBot（免 UAC；应用要在自己
    目录旁写 data/logs/prompts，Program Files 下没有写权限）
  * VC 运行库 app-local 随包（三个 DLL），用户不必另装 redist
  * 两个 Live2D 模型（Haru / wuwuwu）与 FrameworkShaders 一并打包
  * 附微软签名的 WebView2 Evergreen 引导程序（redists\\），本机主流 Win10/11
    通常已自带运行时
"""

import hashlib
import io
import os

REPO = r'D:\VSProject\Unnamedbot'
APP = os.path.join(REPO, 'Unnamedbot')
REL = os.path.join(APP, 'x64', 'Release')
SETUP = os.path.join(REPO, 'UnnamedBotSetup')
REDIST = os.path.join(SETUP, 'redist')

PRODUCT_NAME = 'UnnamedBot'
MANUFACTURER = 'UnnamedBot'
PRODUCT_VERSION = '1.0.0'
PRODUCT_CODE = '{9F2B7C41-3E5A-4D18-9C77-5B1E0A2D6F31}'
UPGRADE_CODE = '{4A7D1E93-6C02-4B85-A1F4-8E3D2C7B5A09}'

APP_DLLS = ['cpr.dll', 'libcurl.dll', 'z.dll', 'sqlite3.dll', 'Live2DCubismCore.dll']
CRT_DLLS = ['msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll']
WEBVIEW2 = 'MicrosoftEdgeWebView2Setup.exe'


def guid_for(text):
    """目标路径 → 稳定的 GUID（组件/文件的 GUID 必须跨版本稳定）"""
    h = hashlib.md5(('unnamedbot:' + text).encode('utf-8')).hexdigest().upper()
    return '{%s-%s-%s-%s-%s}' % (h[0:8], h[8:12], h[12:16], h[16:20], h[20:32])


def ident_for(text):
    """目标路径 → 合法的 WiX Id（只用字母数字与下划线）"""
    h = hashlib.md5(('id:' + text).encode('utf-8')).hexdigest()[:16].upper()
    return 'c' + h


def collect_payload():
    items = []

    def add_dir(src_dir, sub):
        for root, _dirs, files in os.walk(src_dir):
            for fn in sorted(files):
                full = os.path.join(root, fn)
                rel = os.path.relpath(full, src_dir)
                items.append((full, tuple(sub) + tuple(rel.replace('\\', '/').split('/')[:-1])))

    for name in ['Unnamedbot.exe'] + APP_DLLS + CRT_DLLS:
        p = os.path.join(REL if name in APP_DLLS + ['Unnamedbot.exe'] else os.path.join(REDIST, 'crt'), name)
        if not os.path.isfile(p):
            raise SystemExit('缺少构建产物：' + p)
        items.append((p, ()))
    items.append((os.path.join(REDIST, WEBVIEW2), ('redists',)))
    add_dir(os.path.join(REL, 'FrameworkShaders'), ('FrameworkShaders',))
    for model in ('Haru', 'wuwuwu'):
        add_dir(os.path.join(APP, 'models', model), ('models', model))
    return items


def dir_id(parts):
    return 'd_' + hashlib.md5(('dir:' + '/'.join(parts)).encode()).hexdigest()[:12]


def main():
    items = collect_payload()

    # 目录树（自顶向下）
    dirs = set()
    for _src, parts in items:
        for depth in range(len(parts)):
            dirs.add(parts[:depth + 1])
    tree = {}
    for parts in dirs:
        tree.setdefault(parts[:-1], []).append(parts[-1])
    for key in tree:
        tree[key] = sorted(tree[key])

    def render_dirs(parent, indent):
        out = ''
        for name in tree.get(parent, []):
            parts = parent + (name,)
            out += '%s<Directory Id="%s" Name="%s">\n' % (' ' * indent, dir_id(parts), name)
            out += render_dirs(parts, indent + 2)
            out += '%s</Directory>\n' % (' ' * indent)
        return out

    # 文件 → 组件（组件必须挂在所属目录的 DirectoryRef 下，且不能再写 Directory 属性）
    comps_by_dir = {}
    all_comps = []
    for src, parts in items:
        name = os.path.basename(src)
        target = '/'.join(parts + (name,))
        comp_id = ident_for(target)
        file_id = 'f' + hashlib.md5(('file:' + target).encode()).hexdigest()[:20].upper()
        parent = dir_id(parts) if parts else 'INSTALLFOLDER'
        shortcut = ''
        if name.lower() == 'unnamedbot.exe':
            shortcut = (
                '\n          <Shortcut Id="StartMenuShortcut" Directory="AppMenuDir" Name="%s"\n'
                '                    WorkingDirectory="INSTALLFOLDER" Icon="AppIcon" Advertise="no" />\n'
                '          <Shortcut Id="DesktopShortcut" Directory="DesktopFolder" Name="%s"\n'
                '                    WorkingDirectory="INSTALLFOLDER" Icon="AppIcon" Advertise="no" />'
                % (PRODUCT_NAME, PRODUCT_NAME))
        extra = ''
        if name.lower() == 'unnamedbot.exe':
            # 开始菜单目录也要在卸载时删掉（ICE64 要求按用户目录登记 RemoveFile）
            extra += ('        <RemoveFolder Id="rmf_menu" Directory="AppMenuDir" On="uninstall" />\n')
        comps_by_dir.setdefault(parent, []).append(
            '      <Component Id="%s" Guid="%s">\n'
            # 按用户安装（perUser）时组件 KeyPath 必须是 HKCU 注册表项，不能是文件，
            # 否则 MSI 校验报 ICE38。给每个组件写一个标记项作为 KeyPath。
            '        <RegistryValue Root="HKCU" Key="Software\\%s\\Components" Name="%s"\n'
            '                       Type="integer" Value="1" KeyPath="yes" />\n'
            # 每个安装目录都要登记 RemoveFile（否则 ICE64：用户目录卸载后残留）
            '        <RemoveFolder Id="rmf_%s" Directory="%s" On="uninstall" />\n'
            '%s'
            '        <File Id="%s" Source="%s">%s\n'
            '        </File>\n'
            '      </Component>\n'
            % (comp_id, guid_for(target), MANUFACTURER, comp_id, comp_id, parent, extra,
               file_id, src.replace('&', '&amp;'), shortcut))
        all_comps.append(comp_id)

    # 只登记有文件的目录不够：像 models / models\Haru 这种中间目录、以及
    # %LOCALAPPDATA%\Programs 本身没有文件，ICE64 仍要求它们在 RemoveFile 表里。
    # 统一挂到一个专门的清理组件上。
    cleanup_ids = ['ProgramsDir'] + [dir_id(parts) for parts in sorted(dirs)]
    cleanup_ids = [d for d in cleanup_ids if d not in comps_by_dir]
    cleanup = ''
    if cleanup_ids:
        cleanup = ('      <Component Id="cmp_cleanup" Guid="%s">\n'
                   '        <RegistryValue Root="HKCU" Key="Software\\%s\\Components" Name="cmp_cleanup"\n'
                   '                       Type="integer" Value="1" KeyPath="yes" />\n'
                   % (guid_for('cleanup'), MANUFACTURER)
                   + ''.join('        <RemoveFolder Id="rmf_%s" Directory="%s" On="uninstall" />\n'
                             % (d, d) for d in cleanup_ids)
                   + '      </Component>\n')
        comps_by_dir.setdefault('INSTALLFOLDER', []).append(cleanup)
        all_comps.append('cmp_cleanup')

    directory_refs = ''
    for parent in ['INSTALLFOLDER'] + sorted(k for k in comps_by_dir if k != 'INSTALLFOLDER'):
        if parent not in comps_by_dir:
            continue
        directory_refs += ('    <DirectoryRef Id="%s">\n' % parent
                           + ''.join(comps_by_dir[parent])
                           + '    </DirectoryRef>\n')

    wxs = '''<?xml version="1.0" encoding="utf-8"?>
<Wix xmlns="http://schemas.microsoft.com/wix/2006/wi">
  <Product Id="%(product_code)s" Name="%(name)s" Language="2052" Version="%(version)s"
           Manufacturer="%(manufacturer)s" UpgradeCode="%(upgrade_code)s" Codepage="936">
    <Package InstallerVersion="500" Compressed="yes" InstallScope="perUser"
             InstallPrivileges="limited" Description="%(name)s 桌宠中控"
             Comments="AI 桌宠中控（含 Live2D 模型与运行库，无需额外依赖）" />
    <MajorUpgrade AllowSameVersionUpgrades="yes"
                  DowngradeErrorMessage="已经安装了更新版本的 %(name)s。" />
    <MediaTemplate EmbedCab="yes" />

    <Icon Id="AppIcon" SourceFile="%(exe)s" />
    <Property Id="ARPPRODUCTICON" Value="AppIcon" />
    <Property Id="ARPCOMMENTS" Value="AI 桌宠中控" />

    <Directory Id="TARGETDIR" Name="SourceDir">
      <Directory Id="LocalAppDataFolder">
        <Directory Id="ProgramsDir" Name="Programs">
          <Directory Id="INSTALLFOLDER" Name="%(name)s">
%(subdirs)s
          </Directory>
        </Directory>
      </Directory>
      <Directory Id="ProgramMenuFolder">
        <Directory Id="AppMenuDir" Name="%(name)s" />
      </Directory>
      <Directory Id="DesktopFolder" />
    </Directory>

    <DirectoryRef Id="TARGETDIR" />
%(directory_refs)s
    <Feature Id="MainFeature" Title="%(name)s" Level="1">
%(comp_refs)s    </Feature>

    <UIRef Id="WixUI_Minimal" />
  </Product>
</Wix>
''' % {
        'product_code': PRODUCT_CODE,
        'upgrade_code': UPGRADE_CODE,
        'name': PRODUCT_NAME,
        'version': PRODUCT_VERSION,
        'manufacturer': MANUFACTURER,
        'exe': os.path.join(REL, 'Unnamedbot.exe'),
        'subdirs': render_dirs((), 12),
        'directory_refs': directory_refs,
        'comp_refs': ''.join('      <ComponentRef Id="%s" />\n' % c for c in all_comps),
    }

    target = os.path.join(SETUP, 'UnnamedBotSetup.wxs')
    io.open(target, 'w', encoding='utf-8', newline='').write(wxs)
    total = sum(os.path.getsize(src) for src, _ in items)
    print('已生成 %s' % target)
    print('  文件 %d 个（%.1f MB）· 组件 %d 个 · 子目录 %d 个'
          % (len(items), total / 1048576.0, len(all_comps), len(dirs)))
    print('  安装位置：%%LOCALAPPDATA%%\\Programs\\%s（按用户安装，免 UAC）' % PRODUCT_NAME)


if __name__ == '__main__':
    main()
