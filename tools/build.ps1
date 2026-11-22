# One-step build of the VPK: finds the game, regenerates the recompiled code when needed, configures and builds.
# Usage: tools/build.ps1 [-Game <the game folder>] [-Config Release|Debug] [-Clean] [-Reconfigure] [-Jobs N]
#                        [-Regenerate] [-Compiler gcc|clang] [-NoLto] [-PgoGenerate] [-PgoUse <profile>[,<profile>...]]
#                        [-RenderCheck] [-Census] [-Generator <generate.py options>]
# The game folder (with NFSC.exe) comes from -Game, then NFSC_GAME_DIR, then the folder used last time; it is
# remembered for the next run. Code is generated again when the executable or the recompiler changed, or with
# -Regenerate, which also runs discovery again instead of taking it from .gencache. Only what changed is rebuilt.
# -Compiler clang (the default) builds with clang and lld (NFSC_LLVM: the LLVM root, else the pinned LLVM of
# tools/deps.py) and ThinLTO (-NoLto: without) into
# build-<config>-clang; -PgoGenerate makes the instrumented build (build-<config>-pgogen), -PgoUse optimises with a
# merged .profdata (docs/settings.md). With gcc, -PgoGenerate builds into build-<config>-gcov and -PgoUse takes one or more
# nfsc.gcov files from instrumented runs (build-<config>-pgo). -RenderCheck builds the render check build (native
# renderer calls compared with the translated code's, not for timing) into build-<config>[-clang]-check.
# -Census makes the display census build (NFSC_CENSUS_ACCESS) in build-<config>...-census, with a 220 MB guest heap
# so its larger code and tables fit. -Generator passes options to the generator, separated by spaces or commas
# (comparison builds: -Generator '--partial-reads --alias-cpu'; generate.py --help lists them); a change regenerates.
param(
    [string]$Game,
    [string]$Config = 'Release',
    [switch]$Clean,
    [switch]$Reconfigure,
    [switch]$Regenerate,
    [int]$Jobs = [Environment]::ProcessorCount,
    [ValidateSet('gcc', 'clang')][string]$Compiler = 'clang',
    [switch]$NoLto,
    [switch]$PgoGenerate,
    [string[]]$PgoUse,
    [switch]$RenderCheck,
    [switch]$Census,
    [string]$Generator
)
if ($RenderCheck -and ($PgoGenerate -or $PgoUse)) { throw '-RenderCheck excludes the PGO builds' }
if ($NoLto -and $Compiler -ne 'clang') { throw '-NoLto needs -Compiler clang' }
if ($PgoGenerate -and $PgoUse) { throw '-PgoGenerate and -PgoUse exclude each other' }
if ($PgoUse -and $Compiler -eq 'clang' -and $PgoUse.Count -ne 1) { throw '-PgoUse with clang takes one merged .profdata' }
$PgoUse = @($PgoUse | Where-Object { $_ } | ForEach-Object { (Resolve-Path $_).Path -replace '\\', '/' })
$Generator = @($Generator -split '[ ,]+' | Where-Object { $_ }) -join ' '

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'env.ps1')
if ($Compiler -eq 'clang' -and (-not $env:NFSC_LLVM -or -not (Test-Path (Join-Path $env:NFSC_LLVM 'bin/clang*')))) {
    if ($env:NFSC_LLVM) { Write-Host "NFSC_LLVM=$env:NFSC_LLVM has no bin/clang: using the pinned LLVM" }
    $env:NFSC_LLVM = Get-Dependency 'llvm'
}
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if ($Compiler -eq 'clang') { $suffix = if ($PgoGenerate) { '-pgogen' } else { '-clang' } }
else { $suffix = if ($PgoGenerate) { '-gcov' } elseif ($PgoUse) { '-pgo' } else { '' } }
if ($RenderCheck) { $suffix += '-check' }
if ($Census) { $suffix += '-census' }
$build = Join-Path $repo "build-$($Config.ToLower())$suffix"
$generated = Join-Path $repo 'src/nfsc/generated'
$exeName = 'NFSC.exe'
$remembered = Join-Path $repo 'build-game-dir.txt'

function Resolve-GameDir {
    $candidates = @($Game, $env:NFSC_GAME_DIR)
    if (Test-Path $remembered) { $candidates += (Get-Content $remembered -Raw).Trim() }
    foreach ($dir in $candidates) {
        if (-not $dir) { continue }
        if (Test-Path (Join-Path $dir $exeName)) { return (Resolve-Path $dir).Path }
        if ($dir -eq $Game) { throw "$exeName not found in $Game (give the game folder)" }
    }
    return $null
}

# Fingerprint of the generator inputs: the executable, every recompiler source and the generator options.
function Get-GeneratorStamp([string]$gameDir) {
    $files = @(Get-Item (Join-Path $gameDir $exeName)) +
             @(Get-ChildItem (Join-Path $PSScriptRoot 'recompiler') -Recurse -File -Include '*.py', '*.txt' |
               Where-Object { $_.FullName -notmatch '__pycache__' } | Sort-Object FullName)
    $lines = @($files | ForEach-Object { (Get-FileHash $_.FullName -Algorithm SHA256).Hash })
    if ($Generator) { $lines += "options $Generator" }
    $bytes = [Text.Encoding]::UTF8.GetBytes(($lines -join "`n"))
    $sha = [Security.Cryptography.SHA256]::Create()
    return ([BitConverter]::ToString($sha.ComputeHash($bytes)) -replace '-', '')
}

$stampFile = Join-Path $generated '.stamp'
$haveCode = Test-Path (Join-Path $generated 'nfsc.cpp')
$gameDir = Resolve-GameDir
if ($gameDir) {
    Set-Content -Path $remembered -Value $gameDir -Encoding UTF8
    $stamp = Get-GeneratorStamp $gameDir
    $current = if (Test-Path $stampFile) { (Get-Content $stampFile -Raw).Trim() } else { '' }
    if ($Regenerate -or -not $haveCode -or $stamp -ne $current) {
        Write-Host "Generating the recompiled code from $(Join-Path $gameDir $exeName)..."
        & (Join-Path $PSScriptRoot 'generate.ps1') -Binaries $gameDir -Fresh:$Regenerate -Options ($Generator -split ' ')
        Set-Content -Path $stampFile -Value $stamp -Encoding ASCII
    }
} elseif (-not $haveCode) {
    throw "The game folder was not found: run tools/build.ps1 -Game '<the game folder with $exeName>'."
} else {
    Write-Host 'Game folder not found: building the existing generated code.'
}

# Native tools report failure through their exit code; Windows PowerShell 5.1 would stop on any stderr line.
$ErrorActionPreference = 'Continue'
if ($Clean -and (Test-Path $build)) { Remove-Item $build -Recurse -Force -ErrorAction Stop }

# Reconfigures when the compiler options (and the profiles' contents) differ from build-options.txt in the build folder.
$compilerOptions = @("-DNFSC_COMPILER=$Compiler", "-DNFSC_PGO_GENERATE=$(if ($PgoGenerate) { 'ON' } else { 'OFF' })",
                     "-DNFSC_PGO_USE=$($PgoUse -join ';')",
                     "-DNFSC_RENDER_CHECK=$(if ($RenderCheck) { 'ON' } else { 'OFF' })",
                     "-DNFSC_CENSUS_ACCESS=$(if ($Census) { 'ON' } else { 'OFF' })",
                     "-DNFSC_GUEST_HEAP_MIB=$(if ($Census) { '220' } else { '232' })")
if ($Compiler -eq 'clang') {
    $compilerOptions += "-DNFSC_LTO=$(if ($NoLto) { 'OFF' } else { 'ON' })"
    if (-not $env:NFSC_LLVM) { Write-Host 'NFSC_LLVM not set: clang from PATH' }
}
$stampLines = $compilerOptions + @($PgoUse | ForEach-Object { (Get-FileHash $_ -Algorithm SHA256).Hash })
$optionsFile = Join-Path $build 'build-options.txt'
$previous = if (Test-Path $optionsFile) { (Get-Content $optionsFile -Raw).Trim() } else { '' }
$optionsChanged = $previous -ne ($stampLines -join ' ')
if ($Reconfigure -or -not (Test-Path (Join-Path $build 'build.ninja')) -or $optionsChanged) {
    $toolchain = if ($Compiler -eq 'clang') { "$repo/cmake/vita-clang.cmake" } else { "$env:VITASDK/share/vita.toolchain.cmake" }
    $options = @("-DCMAKE_TOOLCHAIN_FILE=$toolchain", "-DCMAKE_BUILD_TYPE=$Config") + $compilerOptions
    $options += if ($env:SCE_PSP2_SDK_DIR) { "-DSCE_PSP2_SDK_DIR=$env:SCE_PSP2_SDK_DIR" } else { '-USCE_PSP2_SDK_DIR' }
    cmake -S $repo -B $build -G Ninja @options
    if ($LASTEXITCODE -ne 0) { throw 'configure failed' }
    Set-Content -Path $optionsFile -Value ($stampLines -join ' ') -Encoding UTF8
}

cmake --build $build --parallel $Jobs
if ($LASTEXITCODE -ne 0) { throw 'build failed' }
Write-Host "Output: $(Join-Path $build 'NFSCVITA.vpk')"
