param([switch]$BuildOnly)
$ErrorActionPreference = 'Stop'
$taskSimDir = $PSScriptRoot
$taskToolBin = Join-Path $taskSimDir '.tools/w64devkit/bin'
if (-not (Test-Path (Join-Path $taskToolBin 'gcc.exe'))) {
    $taskToolsDir = Join-Path $taskSimDir '.tools'
    New-Item -ItemType Directory -Force $taskToolsDir | Out-Null
    $taskArchive = Join-Path $taskToolsDir 'w64devkit-x64-2.6.0.7z.exe'
    if (-not (Test-Path $taskArchive)) {
        Write-Host 'Downloading portable preview compiler (first run only)...'
        Invoke-WebRequest 'https://github.com/skeeto/w64devkit/releases/download/v2.6.0/w64devkit-x64-2.6.0.7z.exe' -OutFile $taskArchive
    }
    $taskExpectedHash = 'bb212f0012a9324b8cf4104a76c07199d9c17607b4c5755632108d0432359beb'
    if ((Get-FileHash -LiteralPath $taskArchive -Algorithm SHA256).Hash -ne $taskExpectedHash) {
        throw 'Compiler download checksum mismatch. Remove the archive in simulator/.tools and retry.'
    }
    $taskExtract = Start-Process -FilePath $taskArchive -ArgumentList @('-y', "-o`"$taskToolsDir`"") -WindowStyle Hidden -Wait -PassThru
    if ($taskExtract.ExitCode -ne 0 -or -not (Test-Path (Join-Path $taskToolBin 'gcc.exe'))) {
        throw 'Portable compiler extraction failed.'
    }
}
$env:PATH = "$taskToolBin;$env:PATH"
$taskBuildDir = Join-Path $taskSimDir 'build'
& (Join-Path $taskToolBin 'cmake.exe') -S $taskSimDir -B $taskBuildDir -G Ninja '-DCMAKE_BUILD_TYPE=Release' '-DCMAKE_C_COMPILER=gcc' '-DCMAKE_CXX_COMPILER=g++'
if ($LASTEXITCODE -ne 0) { throw 'Preview configuration failed.' }
& (Join-Path $taskToolBin 'cmake.exe') --build $taskBuildDir --parallel 4
if ($LASTEXITCODE -ne 0) { throw 'Preview compilation failed.' }
if (-not $BuildOnly) {
    # The preview window is user-requested, so launch it visibly.
    Start-Process -FilePath (Join-Path $taskBuildDir 'rf_preview.exe') -WorkingDirectory $taskSimDir
}
