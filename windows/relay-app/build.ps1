param(
    [string]$Toolchain = 'C:\D\bedrock-protocol-cpp\_deps\msys64\ucrt64',
    [switch]$SkipNative,
    [string]$PackageName = 'CPE-Relay-Windows-1.0.2-x64'
)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$nativeBuild = Join-Path $repo 'build-windows-app'
if ($PackageName -notmatch '\A[A-Za-z0-9][A-Za-z0-9_.-]{0,100}\z') { throw 'Invalid package name' }
$staging = Join-Path $PSScriptRoot ('obj\standalone-' + [guid]::NewGuid().ToString('N'))
$assets = Join-Path $staging 'assets'
$publish = Join-Path $staging 'publish'
$env:PATH = (Join-Path $Toolchain 'bin') + ';' + $env:PATH
if (!$SkipNative) {
    & (Join-Path $Toolchain 'bin\cmake.exe') -S $repo -B $nativeBuild -G Ninja `
        '-DCMAKE_BUILD_TYPE=Release' '-DCMAKE_CXX_FLAGS_RELEASE=-O2 -DNDEBUG' `
        "-DCMAKE_CXX_COMPILER=$Toolchain/bin/g++.exe" "-DCMAKE_MAKE_PROGRAM=$Toolchain/bin/ninja.exe" `
        "-DCMAKE_PREFIX_PATH=$Toolchain" '-DBEDROCK_PROTOCOL_CPP_BUILD_WINDOWS_APP=ON' `
        '-DBEDROCK_PROTOCOL_CPP_BUILD_TOOLS=OFF' '-DBEDROCK_PROTOCOL_CPP_BUILD_BOT=OFF' `
        '-DBEDROCK_PROTOCOL_CPP_BUILD_EXAMPLES=OFF' '-DBEDROCK_PROTOCOL_CPP_INSTALL=OFF' '-DBUILD_TESTING=OFF'
    if ($LASTEXITCODE -ne 0) { throw 'Native configure failed' }
    & (Join-Path $Toolchain 'bin\cmake.exe') --build $nativeBuild --target cpe_relay_windows -j2
    if ($LASTEXITCODE -ne 0) { throw 'Native build failed' }
}
New-Item -ItemType Directory -Path $assets -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $nativeBuild 'cpe_relay_windows.dll') -Destination $assets
foreach ($dll in @('libcrypto-3-x64.dll', 'libgcc_s_seh-1.dll', 'libstdc++-6.dll', 'libwinpthread-1.dll', 'zlib1.dll')) {
    Copy-Item -LiteralPath (Join-Path $Toolchain "bin\$dll") -Destination $assets
}
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'README.txt') -Destination $assets
$dataRoot = Join-Path $repo 'data\minecraft-data'
$paths = Get-Content -LiteralPath (Join-Path $dataRoot 'dataPaths.json') -Raw | ConvertFrom-Json
foreach ($version in @('1.21.2', '1.21.100')) {
    $versionOut = Join-Path $assets "minecraft-data\$version"
    New-Item -ItemType Directory -Path $versionOut -Force | Out-Null
    foreach ($name in @('blocks', 'blockStates', 'blockCollisionShapes')) {
        $source = Join-Path $dataRoot ($paths.bedrock.$version.$name + "/$name.json")
        Copy-Item -LiteralPath $source -Destination $versionOut
    }
}
$licenses = Join-Path $assets 'licenses'
New-Item -ItemType Directory -Path $licenses -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $repo 'third_party\raknet\LICENSE') -Destination (Join-Path $licenses 'RakNet.txt')
foreach ($name in @('openssl', 'zlib', 'gcc-libs', 'libwinpthread')) {
    $licenseSource = Join-Path $Toolchain "share\licenses\$name"
    if (Test-Path -LiteralPath $licenseSource) { Copy-Item -LiteralPath $licenseSource -Destination $licenses -Recurse -Force }
}
dotnet publish (Join-Path $PSScriptRoot 'CpeRelay.Windows.csproj') -c Release -r win-x64 --self-contained true `
    '-p:PublishSingleFile=true' '-p:IncludeNativeLibrariesForSelfExtract=true' '-p:IncludeAllContentForSelfExtract=true' `
    '-p:EnableCompressionInSingleFile=true' '-p:DebugType=None' "-p:RelayBundleDir=$assets" -o $publish
if ($LASTEXITCODE -ne 0) { throw 'Desktop build failed' }
$published = @(Get-ChildItem -LiteralPath $publish -Recurse -File)
if ($published.Count -ne 1 -or $published[0].Name -ne 'CPE-Relay.exe') { throw 'Standalone publish must contain exactly one EXE' }
$dist = Join-Path $PSScriptRoot 'dist'
New-Item -ItemType Directory -Path $dist -Force | Out-Null
$exe = Join-Path $dist "$PackageName.exe"
Copy-Item -LiteralPath (Join-Path $publish 'CPE-Relay.exe') -Destination $exe
Get-FileHash -LiteralPath $exe -Algorithm SHA256
Write-Output "Standalone EXE: $exe"
