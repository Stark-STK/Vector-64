# Do checking drops in quiescence gain Elo in crazyhouse, and at what depth?
#
#   # test A -- does the feature gain at all (2 plies vs off):
#   powershell -ExecutionPolicy Bypass -File tools\sprt_dropchecks.ps1 -Test A
#
#   # test B -- is deeper better (4 plies vs 2):
#   powershell -ExecutionPolicy Bypass -File tools\sprt_dropchecks.ps1 -Test B
#
# Run the two in separate terminals to use the machine fully. Each side is a
# single-threaded engine, so -Conc is literally how many cores that test uses:
# with -Conc 2 on both, the pair occupies 4 cores plus a little arbiter
# overhead. Drop to -Conc 1 each on a 2-core box.
#
# Background: quiescence generated captures only, so it never saw a checking
# drop -- the dominant tactical motif in a drop variant -- and walked past
# forced mates. ENGINE_DROP_CHECK_QDEPTH now bounds how many quiescence plies
# generate them. The default of 2 was a guess; this measures it.
#
# The depth is a COMPILE-TIME knob on purpose, so the two sides are two
# binaries built from identical source rather than one binary in two modes.
# This script builds both into separate directories and matches them.
#
# Adjudication is by ChessEngine-validator, not a chess library: the engine is
# the only thing here that knows the rules, which is also why crazyhouse can
# be tested at all.
#
# SPRT: H0 elo<=0, H1 elo>=5, alpha=beta=0.05, stops at LLR +/-2.94.

param(
  [ValidateSet("A", "B")] [string]$Test = "A",
  [int]$Nodes = 25000,
  [int]$Games = 6000,
  [int]$Conc  = 2,
  [string]$Variant = "crazyhouse",
  # Tests A and B share the qdepth=2 build. Running both at once would have
  # two cmake invocations writing the same directory, so pre-build the three
  # trees once and pass -SkipBuild to both runs.
  [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

if ($Test -eq "A") {
  $newDepth = 2; $baseDepth = 0
  $label = "drop-checks in qsearch: 2 plies vs OFF"
} else {
  $newDepth = 4; $baseDepth = 2
  $label = "drop-checks in qsearch: 4 plies vs 2"
}

$newDir  = Join-Path $root ("build-dc{0}" -f $newDepth)
$baseDir = Join-Path $root ("build-dc{0}" -f $baseDepth)
$logdir  = Join-Path $root "sprt\logs"
New-Item -ItemType Directory -Force -Path $logdir | Out-Null
$log = Join-Path $logdir ("dropchecks_{0}_{1}v{2}_{3}n.log" -f $Variant, $newDepth, $baseDepth, $Nodes)

# Inherit the generator the main build tree already uses. Without this a fresh
# directory falls back to the platform default, which on a machine that also
# has Visual Studio installed silently switches toolchain mid-comparison.
function Get-Generator {
  $cache = Join-Path $root "build\CMakeCache.txt"
  if (Test-Path $cache) {
    $line = Select-String -Path $cache -Pattern '^CMAKE_GENERATOR:INTERNAL=(.+)$' |
            Select-Object -First 1
    if ($line) { return $line.Matches[0].Groups[1].Value }
  }
  return $null
}

function Build-Side([string]$dir, [int]$depth) {
  Write-Host ("  building qdepth={0} -> {1}" -f $depth, (Split-Path $dir -Leaf))
  $gen = Get-Generator
  $genArgs = if ($gen) { @("-G", $gen) } else { @() }
  cmake -S $root -B $dir @genArgs -DCMAKE_BUILD_TYPE=Release -DENGINE_VIZ=OFF `
        -DENGINE_DROP_CHECK_QDEPTH=$depth | Out-Null
  if ($LASTEXITCODE -ne 0) { throw "cmake configure failed for qdepth=$depth" }
  cmake --build $dir --target ChessEngine-variants ChessEngine-validator -j | Out-Null
  if ($LASTEXITCODE -ne 0) { throw "build failed for qdepth=$depth" }
}

Write-Host ""
Write-Host "===================================================================="
Write-Host "  STK-Vector-64  --  $label"
Write-Host "===================================================================="
if ($SkipBuild) {
  Write-Host "  -SkipBuild: using the existing build trees"
} else {
  Build-Side $newDir  $newDepth
  Build-Side $baseDir $baseDepth
}

$newExe  = Join-Path $newDir  "bin\ChessEngine-variants.exe"
$baseExe = Join-Path $baseDir "bin\ChessEngine-variants.exe"
$arbiter = Join-Path $newDir  "bin\ChessEngine-validator.exe"
foreach ($f in @($newExe, $baseExe, $arbiter)) {
  if (-not (Test-Path $f)) { Write-Error "missing binary: $f"; exit 1 }
}

Write-Host "--------------------------------------------------------------------"
Write-Host ("  new     : ENGINE_DROP_CHECK_QDEPTH={0}" -f $newDepth)
Write-Host ("  base    : ENGINE_DROP_CHECK_QDEPTH={0}" -f $baseDepth)
Write-Host ("  variant : {0}   (arbiter: ChessEngine-validator)" -f $Variant)
Write-Host ("  test    : {0} nodes/move  |  up to {1} games  |  {2} workers" -f $Nodes, $Games, $Conc)
Write-Host ("  gate    : H0 elo<=0  H1 elo>=5   (accept at LLR +/-2.94)")
Write-Host ("  log     : {0}" -f $log)
Write-Host "===================================================================="
Write-Host "  Classical eval only -- NNUE is refused in variant mode by design."
Write-Host "--------------------------------------------------------------------"

$sw = [System.Diagnostics.Stopwatch]::StartNew()

python (Join-Path $root "tools\nnue\match.py") `
  --engine $newExe --base-engine $baseExe --arbiter $arbiter `
  --variant $Variant `
  --sprt 0 5 --games $Games --nodes $Nodes `
  --concurrency $Conc --seed 606060 2>&1 | Tee-Object -FilePath $log

$sw.Stop()
Write-Host "--------------------------------------------------------------------"
Write-Host ("  finished in {0:n1} min. Full log: {1}" -f $sw.Elapsed.TotalMinutes, $log)
Write-Host "  Paste the final: line back; the default qdepth gets set from it."
Write-Host "===================================================================="
