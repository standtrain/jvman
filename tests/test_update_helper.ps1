param(
    [Parameter(Mandatory = $true)]
    [string]$Helper
)

$ErrorActionPreference = 'Stop'
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
$isElevated = $principal.IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
$tempRoot = [IO.Path]::GetTempPath()
$root = Join-Path $tempRoot ("jvman-update-helper-test-" + [Guid]::NewGuid().ToString('N'))
$originalTemp = $env:TEMP
$originalTmp = $env:TMP

try {
    New-Item -ItemType Directory -Path $root | Out-Null
    $env:TEMP = $root
    $env:TMP = $root
    $target = Join-Path $root 'test-update-target.exe'
    $replacement = Join-Path $root 'test-update-replacement.exe'
    Copy-Item -LiteralPath $Helper -Destination $target
    Copy-Item -LiteralPath $Helper -Destination $replacement

    $stream = [IO.File]::Open($replacement, [IO.FileMode]::Append,
                              [IO.FileAccess]::Write, [IO.FileShare]::None)
    try {
        $stream.WriteByte(0x5a)
        $stream.Flush($true)
    } finally {
        $stream.Dispose()
    }

    $original = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash
    $expected = (Get-FileHash -LiteralPath $replacement -Algorithm SHA256).Hash
    & $target --reject-stage $replacement
    if ($LASTEXITCODE -ne 0) {
        throw 'stage checksum rejection test failed'
    }
    & $target --reject-current $replacement
    if ($LASTEXITCODE -ne 0) {
        throw 'current checksum rejection test failed'
    }
    if ((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -ne $original) {
        throw 'a rejected helper update changed the target'
    }
    if ($isElevated) {
        & $target $replacement 2>$null
        if ($LASTEXITCODE -ne 4) {
            throw "elevated helper update returned $LASTEXITCODE instead of refusing publication"
        }
        if ((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -ne $original) {
            throw 'an elevated helper update changed the target'
        }
    } else {
        & $target --remove-stage $replacement
        if ($LASTEXITCODE -ne 0) {
            throw 'missing stage cleanup test failed'
        }
        $missingStageDeadline = [DateTime]::UtcNow.AddSeconds(15)
        do {
            $missingStageFiles = @(Get-ChildItem -LiteralPath $root `
                -Filter '*.jvman-*.tmp' -ErrorAction SilentlyContinue)
            if ($missingStageFiles.Count -eq 0) { break }
            Start-Sleep -Milliseconds 50
        } while ([DateTime]::UtcNow -lt $missingStageDeadline)
        if ($missingStageFiles.Count -ne 0) {
            throw 'a missing stage update left a temporary file'
        }
        if ((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -ne $original) {
            throw 'a missing stage update changed the target'
        }

        & $target $replacement
        if ($LASTEXITCODE -ne 0) {
            throw "update helper harness exited with $LASTEXITCODE"
        }

        $deadline = [DateTime]::UtcNow.AddSeconds(15)
        $actual = ''
        do {
            try {
                $actual = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash
            } catch {
                $actual = ''
            }
            if ($actual -eq $expected) { break }
            Start-Sleep -Milliseconds 50
        } while ([DateTime]::UtcNow -lt $deadline)

        if ($actual -ne $expected) {
            throw 'Windows helper did not publish the expected executable'
        }
        & $target --probe
        if ($LASTEXITCODE -ne 0) {
            throw 'updated helper executable could not be started'
        }
    }
    & $target --jvman-internal-apply-update-v1 2>$null
    if ($LASTEXITCODE -ne 2 -or !(Test-Path -LiteralPath $target)) {
        throw 'internal update mode deleted or accepted a non-helper executable'
    }
    if (Get-ChildItem -LiteralPath $root -Filter '*.jvman-*.tmp') {
        throw 'Windows helper left an update or backup file beside the target'
    }
    $cleanupDeadline = [DateTime]::UtcNow.AddSeconds(15)
    do {
        $newHelpers = @(Get-ChildItem -LiteralPath $root `
            -Filter 'jvman-update-helper-*.exe' -ErrorAction SilentlyContinue)
        if ($newHelpers.Count -eq 0) { break }
        Start-Sleep -Milliseconds 50
    } while ([DateTime]::UtcNow -lt $cleanupDeadline)
    if ($newHelpers.Count -ne 0) {
        throw "Windows update helper did not delete itself: $($newHelpers.FullName -join ', ')"
    }
} finally {
    $env:TEMP = $originalTemp
    $env:TMP = $originalTmp
    if (Test-Path -LiteralPath $root) {
        Remove-Item -LiteralPath $root -Recurse -Force
    }
}
