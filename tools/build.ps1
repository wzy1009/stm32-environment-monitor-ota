param([string]$KeilRoot = 'E:\keil5')
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$uv4 = Join-Path $KeilRoot 'UV4\UV4.exe'
$fromelf = Join-Path $KeilRoot 'ARM\ARMCC\bin\fromelf.exe'
if (!(Test-Path -LiteralPath $uv4) -or !(Test-Path -LiteralPath $fromelf)) { throw 'Keil ARMCC5 tools not found; pass -KeilRoot.' }
$buildDir = Join-Path $repoRoot 'build'
New-Item -ItemType Directory -Force $buildDir | Out-Null
$env:TMP = $buildDir
$env:TEMP = $buildDir
$jobs = @(
    @{ Project='firmware\MDK-ARM\ENV_BASE.uvprojx'; Target='ENV_OTA' },
    @{ Project='firmware\MDK-ARM\ENV_BASE.uvprojx'; Target='ENV_BASE' },
    @{ Project='bootloader\MDK-ARM\OTA_BOOT.uvprojx'; Target='OTA_BOOT' }
)
foreach ($job in $jobs) {
    $project = Join-Path $repoRoot $job.Project
    $log = Join-Path $buildDir ($job.Target + '.log')
    $arguments = '-r "{0}" -t "{1}" -o "{2}"' -f $project,$job.Target,$log
    $process = Start-Process -FilePath $uv4 -ArgumentList $arguments -WorkingDirectory (Split-Path $project) -WindowStyle Hidden -PassThru
    $process.WaitForExit()
    $text = Get-Content -LiteralPath $log -Raw
    if ($process.ExitCode -ne 0 -or $text -notmatch '0 Error\(s\), 0 Warning\(s\)') { throw "Build failed: $log" }
    Write-Output "$($job.Target): 0 errors, 0 warnings"
}
$appAxf = Join-Path $repoRoot 'firmware\MDK-ARM\Objects\ENV_OTA.axf'
& $fromelf --bin --output (Join-Path $buildDir 'ENV_OTA.bin') $appAxf
if ($LASTEXITCODE -ne 0) { throw 'APP BIN export failed' }
& $fromelf --bin --output (Join-Path $buildDir 'OTA_BOOT.bin') (Join-Path $repoRoot 'bootloader\MDK-ARM\OTA_BOOT\OTA_BOOT.axf')
if ($LASTEXITCODE -ne 0) { throw 'Boot BIN export failed' }
Write-Output "OTA BIN: $(Join-Path $buildDir 'ENV_OTA.bin')"
