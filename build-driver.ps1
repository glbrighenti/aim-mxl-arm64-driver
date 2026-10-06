param([string]$NuGetPath)

$ErrorActionPreference = 'Stop'
$version = '10.0.26100.6584'
$sdkVersion = '10.0.26100.1'
$tools = Join-Path $env:TEMP 'aim-driver-tools'
$packages = Join-Path $tools 'packages'
$wdk = Join-Path $packages "Microsoft.Windows.WDK.ARM64.$version\c"
$sdk = Join-Path $packages "Microsoft.Windows.SDK.CPP.$sdkVersion\c"
$sdkArm64 = Join-Path $packages "Microsoft.Windows.SDK.CPP.arm64.$sdkVersion"
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'

if (!(Test-Path $vswhere)) { throw 'Visual Studio Installer (vswhere.exe) is required' }
$visualStudio = & $vswhere -latest -version '[17.0,18.0)' -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.ARM64 -property installationPath
if (!$visualStudio) { throw 'Visual Studio 2022 ARM64 C++ tools are required' }
$toolset = Get-ChildItem (Join-Path $visualStudio 'VC\Tools\MSVC') -Directory | Sort-Object Name -Descending | Select-Object -First 1
$compiler = Join-Path $toolset.FullName 'bin\Hostarm64\arm64\cl.exe'
$linker = Join-Path $toolset.FullName 'bin\Hostarm64\arm64\link.exe'
if (!(Test-Path $compiler)) { throw 'Native ARM64 MSVC compiler is required' }

if (!(Test-Path $wdk) -or !(Test-Path $sdk) -or !(Test-Path $sdkArm64)) {
    if (!$NuGetPath) {
        $NuGetPath = Join-Path $tools 'nuget.exe'
        if (!(Test-Path $NuGetPath)) {
            New-Item -ItemType Directory -Path $tools -Force | Out-Null
            Invoke-WebRequest -UseBasicParsing 'https://dist.nuget.org/win-x86-commandline/latest/nuget.exe' -OutFile $NuGetPath
        }
    }
    if (!(Test-Path $NuGetPath)) { throw "NuGet executable not found: $NuGetPath" }
    & $NuGetPath install Microsoft.Windows.WDK.ARM64 -Version $version -OutputDirectory $packages -NonInteractive -Source 'https://api.nuget.org/v3/index.json'
    if ($LASTEXITCODE -ne 0) { throw 'WDK NuGet restore failed' }
}
if (!(Test-Path $wdk) -or !(Test-Path $sdk) -or !(Test-Path $sdkArm64)) {
    throw 'Expected WDK and matching 26100 SDK NuGet packages were not restored'
}

$output = Join-Path $PSScriptRoot 'driver\out'
New-Item -ItemType Directory -Path $output -Force | Out-Null
$object = Join-Path $output 'aim_mxl.obj'
$image = Join-Path $output 'aim_mxl.sys'
$originalInclude = $env:INCLUDE
$originalLib = $env:LIB
try {
    $env:INCLUDE = @(
        (Join-Path $wdk 'Include\10.0.26100.0\km'),
        (Join-Path $wdk 'Include\wdf\kmdf\1.15'),
        (Join-Path $sdk 'Include\10.0.26100.0\shared'),
        (Join-Path $sdk 'Include\10.0.26100.0\ucrt'),
        (Join-Path $sdk 'Include\10.0.26100.0\um'),
        (Join-Path $toolset.FullName 'include')
    ) -join ';'
    $env:LIB = @(
        (Join-Path $wdk 'Lib\10.0.26100.0\km\ARM64'),
        (Join-Path $wdk 'Lib\wdf\kmdf\ARM64\1.15'),
        (Join-Path $toolset.FullName 'lib\arm64')
    ) -join ';'

    & $compiler /nologo /c /kernel /W4 /WX /wd4324 /D_ARM64_ /DNTDDI_VERSION=0x0A00000C /Fo"$object" (Join-Path $PSScriptRoot 'driver\aim_mxl.c')
    if ($LASTEXITCODE -ne 0) { throw 'ARM64 driver compilation failed' }
    & $linker /nologo /machine:arm64 /driver /subsystem:native /entry:FxDriverEntry /nodefaultlib /OUT:"$image" $object wdfdriverentry.lib wdfldr.lib ntoskrnl.lib bufferoverflowfastfailk.lib libcmt.lib
    if ($LASTEXITCODE -ne 0) { throw 'ARM64 driver linking failed' }
} finally {
    $env:INCLUDE = $originalInclude
    $env:LIB = $originalLib
}

Copy-Item (Join-Path $PSScriptRoot 'driver\aim_mxl.inf') (Join-Path $output 'aim_mxl.inf')
$catalogTool = Join-Path $wdk 'bin\10.0.26100.0\x86\Inf2Cat.exe'
if (!(Test-Path $catalogTool)) { throw 'WDK ARM64 catalog generator is required' }
& $catalogTool "/driver:$output" /os:10_GE_ARM64
if ($LASTEXITCODE -ne 0) { throw 'ARM64 driver catalog generation failed' }
"Unsigned ARM64 package: $output"