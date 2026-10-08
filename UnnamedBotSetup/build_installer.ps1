# 重新生成并构建 UnnamedBotSetup.msi（WiX v3）
#
# 为什么不用 VS 的 Installer Projects：那份工程（UnnamedBotSetup.vdproj）在本机
# 命令行下会被 VSD 以「已取消该操作」拒绝加载，见 generate_vdproj.py 里的说明。
# WiX 工具链可以直接从 NuGet 取到，构建与验证全自动。
#
# 用法： powershell -ExecutionPolicy Bypass -File UnnamedBotSetup\build_installer.ps1

$ErrorActionPreference = 'Stop'
$Setup = Split-Path -Parent $MyInvocation.MyCommand.Path
$Repo = Split-Path -Parent $Setup
$toolsRoot = Join-Path $env:LOCALAPPDATA 'UnnamedBotSetup'
$wixDir = Join-Path $toolsRoot 'wix3\tools'
$py = 'C:\Users\HP\.dsh\dsh-runtimes\dsh-primary-runtime\dependencies\python\python.exe'
if (-not (Test-Path $py)) { $py = 'python' }

# 1) WiX v3 工具链（首次运行时从 NuGet 取；GitHub 在本机不可达）
if (-not (Test-Path (Join-Path $wixDir 'candle.exe'))) {
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    $idx = Invoke-RestMethod -Uri 'https://api.nuget.org/v3-flatcontainer/wix/index.json' -TimeoutSec 60
    $pick = ($idx.versions | Where-Object { $_ -like '3.*' } |
             Sort-Object { [version]($_ -replace '-.*$', '') } | Select-Object -Last 1)
    $nupkg = Join-Path $env:TEMP "wix.$pick.nupkg"
    Invoke-WebRequest -Uri "https://api.nuget.org/v3-flatcontainer/wix/$pick/wix.$pick.nupkg" `
        -OutFile $nupkg -UseBasicParsing -TimeoutSec 300
    New-Item -ItemType Directory -Force -Path (Join-Path $toolsRoot 'wix3') | Out-Null
    Copy-Item $nupkg (Join-Path $env:TEMP 'wix3.zip') -Force
    Expand-Archive -Path (Join-Path $env:TEMP 'wix3.zip') -DestinationPath (Join-Path $toolsRoot 'wix3') -Force
    Write-Host "已获取 WiX $pick"
}

# 2) 先把应用构建出来（安装包要打包 x64\Release 的产物）
$msbuild = 'E:\VSS\MSBuild\Current\Bin\MSBuild.exe'
if (Test-Path $msbuild) {
    & $msbuild (Join-Path $Repo 'Unnamedbot\Unnamedbot.vcxproj') /p:Configuration=Release /p:Platform=x64 /m /nologo /v:minimal |
        Select-String -Pattern 'error|Unnamedbot.vcxproj ->' | Select-Object -First 5
}

# 3) 生成 wxs 并编译
& $py (Join-Path $Setup 'generate_wxs.py')
$obj = Join-Path $Setup 'obj'
New-Item -ItemType Directory -Force -Path $obj | Out-Null
$msi = Join-Path $Setup 'UnnamedBotSetup.msi'
Remove-Item $msi -ErrorAction SilentlyContinue
& (Join-Path $wixDir 'candle.exe') -nologo -ext WixUIExtension -out (Join-Path $obj 'setup.wixobj') (Join-Path $Setup 'UnnamedBotSetup.wxs')
& (Join-Path $wixDir 'light.exe') -nologo -ext WixUIExtension -cultures:zh-CN -out $msi (Join-Path $obj 'setup.wixobj')
if (-not (Test-Path $msi)) { throw 'MSI 未生成' }
Get-Item $msi | ForEach-Object { "构建完成：{0} ({1:N1} MB)" -f $_.FullName, ($_.Length / 1MB) }
