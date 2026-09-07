param(
    [string]$Toolchain = 'C:\D\bedrock-protocol-cpp\_deps\msys64\ucrt64',
    [switch]$SkipNative
)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$nativeBuild = Join-Path $repo 'build-windows-app'
$package = Join-Path $PSScriptRoot 'dist\CPE-Relay-Windows-x64'
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
dotnet publish (Join-Path $PSScriptRoot 'CpeRelay.Windows.csproj') -c Release -r win-x64 --self-contained true `
    '-p:PublishSingleFile=true' '-p:IncludeNativeLibrariesForSelfExtract=true' '-p:DebugType=None' -o $package
if ($LASTEXITCODE -ne 0) { throw 'Desktop build failed' }
Copy-Item -LiteralPath (Join-Path $nativeBuild 'cpe_relay_windows.dll') -Destination $package
foreach ($dll in @('libcrypto-3-x64.dll', 'libgcc_s_seh-1.dll', 'libstdc++-6.dll', 'libwinpthread-1.dll', 'zlib1.dll')) {
    Copy-Item -LiteralPath (Join-Path $Toolchain "bin\$dll") -Destination $package
}
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'README.txt') -Destination $package
$dataRoot = Join-Path $repo 'data\minecraft-data'
$paths = Get-Content -LiteralPath (Join-Path $dataRoot 'dataPaths.json') -Raw | ConvertFrom-Json
foreach ($version in @('1.21.2', '1.21.100')) {
    $versionOut = Join-Path $package "minecraft-data\$version"
    New-Item -ItemType Directory -Path $versionOut -Force | Out-Null
    foreach ($name in @('blocks', 'blockStates', 'blockCollisionShapes')) {
        $source = Join-Path $dataRoot ($paths.bedrock.$version.$name + "/$name.json")
        Copy-Item -LiteralPath $source -Destination $versionOut
    }
}
$licenses = Join-Path $package 'licenses'
New-Item -ItemType Directory -Path $licenses -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $repo 'third_party\raknet\LICENSE') -Destination (Join-Path $licenses 'RakNet.txt')
foreach ($name in @('openssl', 'zlib', 'gcc-libs', 'libwinpthread')) {
    $licenseSource = Join-Path $Toolchain "share\licenses\$name"
    if (Test-Path -LiteralPath $licenseSource) { Copy-Item -LiteralPath $licenseSource -Destination $licenses -Recurse -Force }
}
$zip = Join-Path $PSScriptRoot 'dist\CPE-Relay-Windows-x64.zip'
Compress-Archive -LiteralPath $package -DestinationPath $zip -Force
Get-FileHash -LiteralPath $zip -Algorithm SHA256
Write-Output "Package: $zip"
