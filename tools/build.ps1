param([string]$KeilRoot = $env:KEIL_ROOT, [switch]$PrivateConfig)
$ErrorActionPreference = 'Stop'
if (!$KeilRoot) { throw 'Pass -KeilRoot or set KEIL_ROOT to your Keil installation directory.' }
$repoRoot = Split-Path -Parent $PSScriptRoot
$uv4 = Join-Path $KeilRoot 'UV4\UV4.exe'
$fromelf = Join-Path $KeilRoot 'ARM\ARMCC\bin\fromelf.exe'
if (!(Test-Path -LiteralPath $uv4) -or !(Test-Path -LiteralPath $fromelf)) {
    throw 'Keil ARMCC5 tools not found; pass -KeilRoot.'
}
$project = Join-Path $repoRoot 'firmware\MDK-ARM\ENV_BASE.uvprojx'
$buildDir = Join-Path $repoRoot 'build'
New-Item -ItemType Directory -Force $buildDir | Out-Null
$oldTemp = $env:TEMP
$oldTmp = $env:TMP
$originalProject = $null
try {
    $env:TEMP = $buildDir
    $env:TMP = $buildDir
    if ($PrivateConfig) {
        if (!(Test-Path -LiteralPath (Join-Path $repoRoot 'firmware\Core\Inc\cloud_private.h'))) {
            throw 'Private build requires firmware/Core/Inc/cloud_private.h.'
        }
        $originalProject = [IO.File]::ReadAllBytes($project)
        $projectText = [IO.File]::ReadAllText($project)
        $pattern = '<Define>([^<]*ENV_OTA_APP[^<]*)</Define>'
        if ([regex]::Matches($projectText, $pattern).Count -ne 1) { throw 'Expected exactly one application define list.' }
        # Keep Keil's XML layout; generic serializers produce unsupported empty tags.
        $privateText = [regex]::Replace($projectText, $pattern, {
            param($match)
            $defines = $match.Groups[1].Value
            if ($defines -match 'CLOUD_USE_PRIVATE_CONFIG=\d') {
                $defines = $defines -replace 'CLOUD_USE_PRIVATE_CONFIG=\d', 'CLOUD_USE_PRIVATE_CONFIG=1'
            } else { $defines += ',CLOUD_USE_PRIVATE_CONFIG=1' }
            '<Define>' + $defines + '</Define>'
        })
        [IO.File]::WriteAllText($project, $privateText, [Text.UTF8Encoding]::new($false))
    }
    foreach ($part in @('BOOT', 'APP')) {
        $target = "ENV_$part"
        $log = Join-Path $buildDir "$target.log"
        $arguments = '-r "{0}" -t"{1}" -o "{2}"' -f $project, $target, $log
        $process = Start-Process -FilePath $uv4 -ArgumentList $arguments -WorkingDirectory (Split-Path $project) -WindowStyle Hidden -PassThru -Wait
        $text = Get-Content -LiteralPath $log -Raw
        if ($process.ExitCode -ne 0 -or $text -notmatch '0 Error\(s\), 0 Warning\(s\)') {
            throw "Build failed (exit $($process.ExitCode)): $log"
        }
        $objects = Join-Path $repoRoot "firmware\MDK-ARM\Objects\$part"
        $binary = Join-Path $buildDir "$target.bin"
        & $fromelf --bin --output $binary (Join-Path $objects "$target.axf")
        if ($LASTEXITCODE -ne 0) { throw "$target BIN export failed" }
        Copy-Item -LiteralPath (Join-Path $objects "$target.hex") -Destination (Join-Path $buildDir "$target.hex") -Force
        Write-Output "$target`: 0 errors, 0 warnings"
    }
    Write-Output "Outputs: $buildDir"
    Write-Output 'First flash: ENV_BOOT.hex + ENV_APP.hex. OTA: ENV_APP.bin only.'
} finally {
    if ($null -ne $originalProject) { [IO.File]::WriteAllBytes($project, $originalProject) }
    $env:TEMP = $oldTemp
    $env:TMP = $oldTmp
}
