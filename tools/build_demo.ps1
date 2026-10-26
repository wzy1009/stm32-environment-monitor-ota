param([Parameter(Mandatory=$true)][string]$KeilRoot, [string]$Python = 'python', [switch]$PrivateConfig)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$header = Join-Path $repoRoot 'firmware\Core\Inc\ota_app.h'
$original = [IO.File]::ReadAllBytes($header)
$text = [IO.File]::ReadAllText($header)
$match = [regex]::Match($text, '#define FIRMWARE_VERSION "(\d+)\.(\d+)\.(\d+)"')
if (!$match.Success) { throw 'Demo build expects a numeric major.minor.patch firmware version.' }
$baseVersion = '{0}.{1}.{2}' -f $match.Groups[1].Value, $match.Groups[2].Value, $match.Groups[3].Value
$nextVersion = '{0}.{1}.{2}' -f $match.Groups[1].Value, $match.Groups[2].Value, ([int]$match.Groups[3].Value + 1)
$buildDir = Join-Path $repoRoot 'build'
try {
    # Build the base last so normal outputs match the restored source header.
    foreach ($version in @($nextVersion, $baseVersion)) {
        $versionText = $text.Replace($match.Value, "#define FIRMWARE_VERSION `"$version`"")
        [IO.File]::WriteAllText($header, $versionText, [Text.UTF8Encoding]::new($false))
        & (Join-Path $PSScriptRoot 'build.ps1') -KeilRoot $KeilRoot -PrivateConfig:$PrivateConfig
        $versionDir = Join-Path $buildDir "versions\$version"
        New-Item -ItemType Directory -Force $versionDir | Out-Null
        foreach ($name in @('ENV_APP.bin','ENV_APP.hex','ENV_APP.log','ENV_BOOT.bin','ENV_BOOT.hex','ENV_BOOT.log')) {
            Copy-Item -LiteralPath (Join-Path $buildDir $name) -Destination (Join-Path $versionDir $name) -Force
        }
    }
} finally {
    [IO.File]::WriteAllBytes($header, $original)
}
& $Python (Join-Path $PSScriptRoot 'firmware_package.py') merge --boot (Join-Path $buildDir 'ENV_BOOT.hex') --app (Join-Path $buildDir 'ENV_APP.hex') --out (Join-Path $buildDir 'ENV_INITIAL.hex')
if ($LASTEXITCODE -ne 0) { throw 'Initial HEX validation failed' }
$upgrade = Join-Path $buildDir "versions\$nextVersion\ENV_APP.bin"
& $Python (Join-Path $PSScriptRoot 'firmware_package.py') notice --bin $upgrade --version $nextVersion --url "http://firmware.local:8080/versions/$nextVersion/ENV_APP.bin" --out (Join-Path $buildDir 'ota-notice.example.json')
if ($LASTEXITCODE -ne 0) { throw 'Upgrade notification validation failed' }
Write-Output "Initial version: $baseVersion; OTA version: $nextVersion"
Write-Output 'Replace firmware.local with the actual reachable server address before sending the notification.'
