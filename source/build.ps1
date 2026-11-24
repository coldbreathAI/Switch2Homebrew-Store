param(
    [string]$SdkPath = $env:DEVKITPRO,
    [string]$FfmpegPath = 'ffmpeg.exe',
    [string]$MusicPath,
    [string]$IconPath,
    [string[]]$ArtworkManifest
)

$ErrorActionPreference = 'Stop'
if (-not $SdkPath) { throw 'Set DEVKITPRO or pass -SdkPath with the Ounce Devkit installation path.' }
$sdk = (Resolve-Path -LiteralPath $SdkPath).Path
$ffmpeg = (Get-Command $FfmpegPath -CommandType Application).Source
$build = (New-Item -ItemType Directory -Force (Join-Path $PSScriptRoot 'build')).FullName
$scratch = (New-Item -ItemType Directory -Force (Join-Path $build 'temp')).FullName
$compilerFolder = Join-Path $sdk 'devkitA64\bin'
$toolsFolder = Join-Path $sdk 'tools\bin'
$gcc = Join-Path $compilerFolder 'aarch64-none-elf-gcc.exe'
$objcopy = Join-Path $compilerFolder 'aarch64-none-elf-objcopy.exe'
$nacptool = Join-Path $toolsFolder 'nacptool.exe'
$elf2nro = Join-Path $toolsFolder 'elf2nro.exe'

$env:DEVKITPRO = '.'
$env:DEVKITA64 = (Join-Path $sdk 'devkitA64').Replace('\', '/')
$env:PATH = "$compilerFolder;$toolsFolder;$env:PATH"
$env:TEMP = $scratch
$env:TMP = $scratch

function Get-BuildDependency([string]$Url, [string]$Path, [string]$ExpectedHash) {
    if (Test-Path -LiteralPath $Path -PathType Leaf) {
        $download = $Path
    } else {
        $download = "$Path.download"
        Invoke-WebRequest -UseBasicParsing -Uri $Url -OutFile $download
    }
    if ((Get-FileHash -LiteralPath $download -Algorithm SHA256).Hash -ne $ExpectedHash) {
        throw "Unexpected dependency checksum. Remove $download and retry."
    }
    if ($download -ne $Path) {
        Move-Item -LiteralPath $download -Destination $Path
    }
}

$webpSource = Join-Path $build 'libwebp-1.6.0'
$webpBuild = Join-Path $build 'webp-aarch64'
$webpDecoder = Join-Path $webpBuild 'libwebpdecoder.a'
if (-not (Test-Path -LiteralPath $webpDecoder -PathType Leaf)) {
    if (-not (Test-Path -LiteralPath $webpSource -PathType Container)) {
        $archive = Join-Path $build 'libwebp-1.6.0.tar.gz'
        Get-BuildDependency 'https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-1.6.0.tar.gz' `
            $archive 'e4ab7009bf0629fd11982d4c2aa83964cf244cffba7347ecd39019a9e38c4564'
        & tar.exe -xf $archive -C $build
        if ($LASTEXITCODE -ne 0) { throw 'Could not extract the WebP decoder source.' }
    }
    $webpOptions = @('-DCMAKE_SYSTEM_NAME=Generic', '-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY',
        "-DCMAKE_C_COMPILER=$gcc", "-DCMAKE_AR=$compilerFolder\aarch64-none-elf-ar.exe",
        "-DCMAKE_RANLIB=$compilerFolder\aarch64-none-elf-ranlib.exe",
        '-DCMAKE_C_FLAGS=-mcpu=cortex-a78c+crypto -mtp=soft -fPIE -ffunction-sections -fdata-sections',
        '-DCMAKE_BUILD_TYPE=Release', '-DWEBP_BUILD_ANIM_UTILS=OFF', '-DWEBP_BUILD_CWEBP=OFF',
        '-DWEBP_BUILD_DWEBP=OFF', '-DWEBP_BUILD_GIF2WEBP=OFF', '-DWEBP_BUILD_IMG2WEBP=OFF',
        '-DWEBP_BUILD_VWEBP=OFF', '-DWEBP_BUILD_WEBPINFO=OFF', '-DWEBP_BUILD_LIBWEBPMUX=OFF',
        '-DWEBP_BUILD_WEBPMUX=OFF', '-DWEBP_BUILD_EXTRAS=OFF', '-DWEBP_USE_THREAD=OFF', '-DWEBP_ENABLE_SIMD=OFF')
    & cmake.exe -S $webpSource -B $webpBuild -G Ninja @webpOptions | Out-Host
    if ($LASTEXITCODE -ne 0) { throw 'Could not configure the WebP decoder.' }
    & cmake.exe --build $webpBuild --target webpdecoder | Out-Host
    if ($LASTEXITCODE -ne 0) { throw 'Could not build the WebP decoder.' }
}

$architecture = @('-mcpu=cortex-a78c+crypto', '-mtp=soft', '-fPIE')
$compile = $architecture + @('-std=gnu23', '-O2', '-Wall', '-Wextra', '-Werror',
    '-ffunction-sections', '-fdata-sections', '-D_DEFAULT_SOURCE', '-D__SWITCH__', '-D__SWITCH2__',
    '-Ilibounce/include', '-Iportlibs/switch/include', "-I$build", "-I$PSScriptRoot", "-I$webpSource\src")

$fontDirectory = (New-Item -ItemType Directory -Force (Join-Path $build 'fonts')).FullName
$fontOrigin = 'https://raw.githubusercontent.com/notofonts/noto-fonts/ffebf8c1ee449e544955a7e813c54f9b73848eac/hinted/ttf/NotoSans'
$regularFont = Join-Path $fontDirectory 'NotoSans-Regular.ttf'
$boldFont = Join-Path $fontDirectory 'NotoSans-Bold.ttf'
Get-BuildDependency "$fontOrigin/NotoSans-Regular.ttf" $regularFont 'b85c38ecea8a7cfb39c24e395a4007474fa5a4fc864f6ee33309eb4948d232d5'
Get-BuildDependency "$fontOrigin/NotoSans-Bold.ttf" $boldFont 'c976e4b1b99edc88775377fcc21692ca4bfa46b6d6ca6522bfda505b28ff9d6a'
$null = & (Join-Path $PSScriptRoot 'make-font.ps1') -OutputDirectory $build -RegularFont $regularFont -BoldFont $boldFont
if ($IconPath) {
    $icon = (Resolve-Path -LiteralPath $IconPath).Path
} else {
    $icon = Join-Path $build 'store-icon.jpg'
    $bag = 'drawbox=x=52:y=94:w=152:h=112:color=0xe2e8f0:t=8,' +
        'drawbox=x=100:y=54:w=56:h=48:color=0xe2e8f0:t=8,' +
        'drawbox=x=64:y=106:w=128:h=88:color=0x38bdf8:t=fill'
    & $ffmpeg -y -nostdin -v error -f lavfi -i 'color=c=0x172554:s=256x256' -vf $bag `
        -frames:v 1 -threads 1 -update 1 $icon
    if ($LASTEXITCODE -ne 0) { throw 'Could not generate the default icon.' }
}
if (-not $ArtworkManifest) {
    $ArtworkManifest = @(Join-Path $build 'artwork.json')
    Set-Content -LiteralPath $ArtworkManifest[0] -Value '{}' -Encoding Ascii
}
& python (Join-Path $PSScriptRoot 'make-store-data.py') `
    --reviewed (Join-Path $PSScriptRoot 'catalog-reviewed.json') `
    --english (Join-Path $PSScriptRoot 'catalog-en.json') --logo $icon --ffmpeg $ffmpeg --output $build `
    --sources (Join-Path $PSScriptRoot 'catalog-sources.json') `
    --artwork @ArtworkManifest
if ($LASTEXITCODE -ne 0) { throw 'Could not build the reviewed catalog.' }

$pcm = Join-Path $build 'store-music.pcm'
if ($MusicPath) {
    $musicSource = (Resolve-Path -LiteralPath $MusicPath).Path
    & $ffmpeg -y -nostdin -v error -xerror -i $musicSource -map '0:a:0' -vn -sn -dn `
        -ac 2 -ar 48000 -c:a pcm_s16le -f s16le $pcm
    if ($LASTEXITCODE -ne 0) { throw 'Could not decode the supplied background music.' }
    $pcmSize = (Get-Item -LiteralPath $pcm).Length
    if ($pcmSize -lt 4 -or ($pcmSize % 4) -ne 0) { throw 'Invalid stereo PCM data.' }
} else {
    # objcopy needs a nonempty file. A single byte cannot be read as a stereo PCM frame.
    [System.IO.File]::WriteAllBytes($pcm, [byte[]]@(0))
}

Push-Location -LiteralPath $build
try {
    $assets = [ordered]@{'store-covers' = 'rgb'; 'store-logo' = 'rgb'; 'store-music' = 'pcm'}
    $assetObjects = foreach ($name in $assets.Keys) {
        $path = Join-Path $build "asset-$name.o"
        $inputFile = "$name.$($assets[$name])"
        & $objcopy -I binary -O elf64-littleaarch64 -B aarch64 `
            --rename-section ".data=.rodata.$name,alloc,load,readonly,data,contents" $inputFile $path
        if ($LASTEXITCODE -ne 0) { throw "Could not pack $name resource." }
        $path
    }
} finally {
    Pop-Location
}

$elf = Join-Path $build 'Switch2Homebrew-Store.elf'
$nacp = Join-Path $build 'Switch2Homebrew-Store.nacp'
$nro = Join-Path $build 'Switch2Homebrew-Store.nro'
Push-Location -LiteralPath $sdk
try {
    $objects = foreach ($file in @('main', 'download', 'installer', 'store-ui', 'store-wave', 'store-audio',
                               'catalog-sync', 'store-covers', 'qrcodegen', 'catalog-data')) {
        $objectPath = Join-Path $build ($file + '.o')
        $parent = if ($file -eq 'catalog-data') { $build } else { $PSScriptRoot }
        & $gcc @compile -c (Join-Path $parent ($file + '.c')) -o $objectPath
        if ($LASTEXITCODE -ne 0) { throw "No se pudo compilar $file.c" }
        $objectPath
    }

    & $gcc @architecture '-specs=libounce/switch.specs' '-Llibounce/lib' '-Lportlibs/switch/lib' @objects @assetObjects `
        '-Wl,--start-group' $webpDecoder '-lminizip' '-lz' '-lnx' '-lm' '-Wl,--end-group' -o $elf
    if ($LASTEXITCODE -ne 0) { throw 'No se pudo enlazar el ejecutable ARM64.' }

    & $nacptool --create 'Switch2Homebrew Store' 'Switch2Homebrew' '0.5.2' $nacp
    if ($LASTEXITCODE -ne 0) { throw 'No se pudieron generar los metadatos de la tienda.' }
    & $elf2nro $elf $nro "--nacp=$nacp" "--icon=$icon"
    if ($LASTEXITCODE -ne 0) { throw 'No se pudo empaquetar el archivo NRO.' }
} finally {
    Pop-Location
}

Get-Item -LiteralPath $nro | Select-Object FullName, Length
