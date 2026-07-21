[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Setup,
    [Parameter(Mandatory = $true)]
    [string]$Binary
)

$ErrorActionPreference = 'Stop'

function Resolve-ExistingFile([string]$Path, [string]$Label) {
    $resolved = (Resolve-Path -LiteralPath $Path -ErrorAction Stop).Path
    if (-not (Test-Path -LiteralPath $resolved -PathType Leaf)) {
        throw "$Label is not a file: $resolved"
    }
    return $resolved
}

function Invoke-GuiProcess([string]$FilePath, [string[]]$Arguments) {
    $process = Start-Process -FilePath $FilePath -ArgumentList $Arguments -Wait -PassThru -WindowStyle Hidden
    if ($null -eq $process) {
        throw "Could not start $FilePath"
    }
    return [int]$process.ExitCode
}

function Assert-Equal([object]$Expected, [object]$Actual, [string]$Message) {
    if ($Expected -cne $Actual) {
        throw "$Message (expected '$Expected', got '$Actual')"
    }
}

function Assert-PathContains(
    [AllowNull()][string]$PathValue,
    [string]$Expected,
    [string]$Message
) {
    $expectedPath = [IO.Path]::GetFullPath($Expected).TrimEnd([char[]]@('\', '/'))
    foreach ($entry in @($PathValue -split ';')) {
        $candidate = $entry.Trim().Trim('"')
        if (-not $candidate) { continue }
        try {
            $candidate = [Environment]::ExpandEnvironmentVariables($candidate)
            $candidate = [IO.Path]::GetFullPath($candidate).TrimEnd([char[]]@('\', '/'))
        }
        catch {
            continue
        }
        if ([string]::Equals($candidate, $expectedPath,
                            [StringComparison]::OrdinalIgnoreCase)) {
            return
        }
    }
    throw "$Message (missing '$Expected')"
}

$setupPath = Resolve-ExistingFile $Setup 'Setup'
$binaryPath = Resolve-ExistingFile $Binary 'Payload binary'
$root = Join-Path ([IO.Path]::GetTempPath()) ("jvman-installer-test-" + [guid]::NewGuid().ToString('N'))
$portableDir = Join-Path $root '中文 path with spaces'
$blockedDir = Join-Path $root 'blocked-target'
$corruptDir = Join-Path $root 'corrupt-target'
$corruptSetup = Join-Path $root 'corrupt setup.exe'
$regularInstallDir = Join-Path $root 'regular-install'
$regularDataDir = Join-Path $root 'regular-data'
$beforePath = [Environment]::GetEnvironmentVariable('Path', 'User')
$beforeJavaHome = [Environment]::GetEnvironmentVariable('JAVA_HOME', 'User')
$beforeProcessJvmanHome = [Environment]::GetEnvironmentVariable('JVMAN_HOME', 'Process')
$beforeInstallerState = Test-Path -LiteralPath 'Registry::HKEY_CURRENT_USER\Software\jvman\Installer'
$beforeArpState = Test-Path -LiteralPath 'Registry::HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Uninstall\jvman'

New-Item -ItemType Directory -Path $portableDir -Force | Out-Null
try {
    $createdNew = $false
    $instanceGuard = [Threading.Mutex]::new(
        $false,
        'Local\jvman.Setup.1F07284D-788B-4F89-A327-DA0F15511708',
        [ref]$createdNew)
    try {
        Assert-Equal $true $createdNew 'Installer instance test mutex already existed'
        $blockedArguments = @('/S', '/PORTABLE', "/DIR=`"$blockedDir`"")
        Assert-Equal 3 (Invoke-GuiProcess $setupPath $blockedArguments) 'Concurrent installer was not rejected'
        if (Test-Path -LiteralPath (Join-Path $blockedDir 'jvman.exe')) {
            throw 'Rejected concurrent installer wrote a payload'
        }
    }
    finally {
        $instanceGuard.Dispose()
    }

    $arguments = @('/S', '/PORTABLE', "/DIR=`"$portableDir`"")
    Assert-Equal 0 (Invoke-GuiProcess $setupPath $arguments) 'Portable install failed'

    $installed = Join-Path $portableDir 'jvman.exe'
    if (-not (Test-Path -LiteralPath $installed -PathType Leaf)) {
        throw "Portable install did not create $installed"
    }
    $expectedHash = (Get-FileHash -LiteralPath $binaryPath -Algorithm SHA256).Hash
    $actualHash = (Get-FileHash -LiteralPath $installed -Algorithm SHA256).Hash
    Assert-Equal $expectedHash $actualHash 'Extracted payload differs from jvman.exe'

    $version = & $installed version 2>&1
    if ($LASTEXITCODE -ne 0 -or ($version -join "`n") -notmatch '0\.2\.0') {
        throw "Installed jvman version command failed: $($version -join ' ')"
    }

    Assert-Equal 0 (Invoke-GuiProcess $setupPath $arguments) 'Repeated portable install failed'
    $afterHash = (Get-FileHash -LiteralPath $installed -Algorithm SHA256).Hash
    Assert-Equal $expectedHash $afterHash 'Repeated portable install changed the payload'

    Copy-Item -LiteralPath $setupPath -Destination $corruptSetup -Force
    $bytes = [IO.File]::ReadAllBytes($corruptSetup)
    if ($bytes.Length -lt 65) {
        throw 'Setup executable is too small to contain a payload and footer'
    }
    $flipIndex = $bytes.Length - 65
    $bytes[$flipIndex] = [byte]($bytes[$flipIndex] -bxor 0x01)
    [IO.File]::WriteAllBytes($corruptSetup, $bytes)
    $corruptArgs = @('/S', '/PORTABLE', "/DIR=`"$corruptDir`"")
    $corruptExit = Invoke-GuiProcess $corruptSetup $corruptArgs
    if ($corruptExit -eq 0) {
        throw 'Corrupted setup unexpectedly succeeded'
    }
    if (Test-Path -LiteralPath (Join-Path $corruptDir 'jvman.exe')) {
        throw 'Corrupted setup published a payload'
    }

    if (-not $beforeInstallerState -and -not $beforeArpState) {
        $regularUninstaller = Join-Path $regularInstallDir 'uninstall.exe'
        [Environment]::SetEnvironmentVariable('JVMAN_HOME', $regularDataDir, 'Process')
        try {
            $regularArguments = @('/S', '/ADD_TO_PATH', '/USER_PATH', "/DIR=`"$regularInstallDir`"")
            Assert-Equal 0 (Invoke-GuiProcess $setupPath $regularArguments) 'Regular install failed'
            if (-not (Test-Path -LiteralPath $regularUninstaller -PathType Leaf)) {
                throw "Regular install did not create $regularUninstaller"
            }
            $installedUserPath = [Environment]::GetEnvironmentVariable('Path', 'User')
            $stableJavaBin = Join-Path $regularDataDir 'current\bin'
            Assert-PathContains $installedUserPath $regularInstallDir 'Regular install did not add its program directory to user PATH'
            Assert-PathContains $installedUserPath $stableJavaBin 'Regular install did not add current\bin to user PATH'
            if (Test-Path -LiteralPath $stableJavaBin) {
                throw 'Regular install unexpectedly required an existing current JDK'
            }

            # /S is intentionally the only argument: the installed copy must
            # infer uninstall mode from authenticated installation state.
            Assert-Equal 0 (Invoke-GuiProcess $regularUninstaller @('/S')) 'Installed uninstaller did not infer uninstall mode'
            $cleanupDeadline = [DateTime]::UtcNow.AddSeconds(10)
            while (((Test-Path -LiteralPath $regularUninstaller) -or
                    (Test-Path -LiteralPath (Join-Path $regularInstallDir 'jvman.exe')) -or
                    (Test-Path -LiteralPath (Join-Path $regularInstallDir 'install.marker')) -or
                    (Test-Path -LiteralPath 'Registry::HKEY_CURRENT_USER\Software\jvman\Installer')) -and
                   [DateTime]::UtcNow -lt $cleanupDeadline) {
                Start-Sleep -Milliseconds 100
            }
            $uninstallerRemaining = Test-Path -LiteralPath $regularUninstaller
            $executableRemaining = Test-Path -LiteralPath (Join-Path $regularInstallDir 'jvman.exe')
            $markerRemaining = Test-Path -LiteralPath (Join-Path $regularInstallDir 'install.marker')
            $metadataRemaining = Test-Path -LiteralPath 'Registry::HKEY_CURRENT_USER\Software\jvman\Installer'
            if ($uninstallerRemaining -or $executableRemaining -or
                $markerRemaining -or $metadataRemaining) {
                throw "Direct uninstall cleanup incomplete (uninstaller=$uninstallerRemaining, executable=$executableRemaining, marker=$markerRemaining, metadata=$metadataRemaining)"
            }
            Assert-Equal $false (Test-Path -LiteralPath 'Registry::HKEY_CURRENT_USER\Software\jvman\Installer') 'Direct uninstaller left installer registry state'
            Assert-Equal $false (Test-Path -LiteralPath 'Registry::HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Uninstall\jvman') 'Direct uninstaller left Add/Remove Programs state'
            Assert-Equal $beforePath ([Environment]::GetEnvironmentVariable('Path', 'User')) 'Direct uninstall did not restore user PATH'
        }
        finally {
            [Environment]::SetEnvironmentVariable('JVMAN_HOME', $beforeProcessJvmanHome, 'Process')
            if ((Test-Path -LiteralPath $regularUninstaller -PathType Leaf) -and
                (Test-Path -LiteralPath 'Registry::HKEY_CURRENT_USER\Software\jvman\Installer')) {
                try {
                    [void](Invoke-GuiProcess $regularUninstaller @('/S', '/UNINSTALL'))
                }
                catch {
                    Write-Warning 'Could not clean up the temporary regular installation.'
                }
            }
        }
    }

    Assert-Equal $beforePath ([Environment]::GetEnvironmentVariable('Path', 'User')) 'Portable install changed user PATH'
    Assert-Equal $beforeJavaHome ([Environment]::GetEnvironmentVariable('JAVA_HOME', 'User')) 'Portable install changed user JAVA_HOME'
    Assert-Equal $beforeInstallerState (Test-Path -LiteralPath 'Registry::HKEY_CURRENT_USER\Software\jvman\Installer') 'Portable install changed installer registry state'
    Assert-Equal $beforeArpState (Test-Path -LiteralPath 'Registry::HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Uninstall\jvman') 'Portable install changed Add/Remove Programs state'
    Write-Output 'Installer portable and integrity tests passed.'
}
finally {
    try {
        [Environment]::SetEnvironmentVariable('Path', $beforePath, 'User')
        [Environment]::SetEnvironmentVariable('JAVA_HOME', $beforeJavaHome, 'User')
    }
    finally {
        # Remove only the uniquely named temporary tree created by this test.
        if ($root -and (Test-Path -LiteralPath $root)) {
            $tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
            $resolvedRoot = [IO.Path]::GetFullPath($root)
            if ($resolvedRoot.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase) -and
                [IO.Path]::GetFileName($resolvedRoot).StartsWith('jvman-installer-test-', [StringComparison]::Ordinal)) {
                Remove-Item -LiteralPath $resolvedRoot -Recurse -Force -ErrorAction SilentlyContinue
            }
        }
    }
}
