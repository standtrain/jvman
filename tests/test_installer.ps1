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

function Assert-UserUninstallCompleted([string]$InstallDir) {
    $uninstaller = Join-Path $InstallDir 'uninstall.exe'
    $executable = Join-Path $InstallDir 'jvman.exe'
    $marker = Join-Path $InstallDir 'install.marker'
    $deadline = [DateTime]::UtcNow.AddSeconds(10)
    while (((Test-Path -LiteralPath $uninstaller) -or
            (Test-Path -LiteralPath $executable) -or
            (Test-Path -LiteralPath $marker) -or
            (Test-Path -LiteralPath 'Registry::HKEY_CURRENT_USER\Software\jvman\Installer')) -and
           [DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 100
    }
    $uninstallerRemaining = Test-Path -LiteralPath $uninstaller
    $executableRemaining = Test-Path -LiteralPath $executable
    $markerRemaining = Test-Path -LiteralPath $marker
    $metadataRemaining = Test-Path -LiteralPath 'Registry::HKEY_CURRENT_USER\Software\jvman\Installer'
    if ($uninstallerRemaining -or $executableRemaining -or
        $markerRemaining -or $metadataRemaining) {
        throw "Uninstall cleanup incomplete (uninstaller=$uninstallerRemaining, executable=$executableRemaining, marker=$markerRemaining, metadata=$metadataRemaining)"
    }
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

    Assert-Equal 2 (Invoke-GuiProcess $setupPath @('/S', '/REMOVE_DATA')) 'Data removal was accepted without uninstall mode'
    Assert-Equal 2 (Invoke-GuiProcess $setupPath @('/S', '/UNINSTALL', '/REMOVE_JDKS')) 'Managed JDK removal was accepted without data removal'

    if (-not $beforeInstallerState -and -not $beforeArpState) {
        $regularUninstaller = Join-Path $regularInstallDir 'uninstall.exe'
        $managedJdkDir = Join-Path $regularDataDir 'jdks\managed-test'
        $managedSentinel = Join-Path $managedJdkDir 'managed.marker'
        $cacheSentinel = Join-Path $regularDataDir 'cache\download.tmp'
        $versionsSentinel = Join-Path $regularDataDir 'versions\managed-test.conf'
        $externalJdkDir = Join-Path $root 'external-jdk'
        $externalSentinel = Join-Path $externalJdkDir 'external.marker'
        $currentLink = Join-Path $regularDataDir 'current'
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

            New-Item -ItemType Directory -Path $managedJdkDir -Force | Out-Null
            New-Item -ItemType Directory -Path (Split-Path -Parent $cacheSentinel) -Force | Out-Null
            New-Item -ItemType Directory -Path (Split-Path -Parent $versionsSentinel) -Force | Out-Null
            New-Item -ItemType Directory -Path $externalJdkDir -Force | Out-Null
            Set-Content -LiteralPath $managedSentinel -Value 'managed' -Encoding ASCII
            Set-Content -LiteralPath $cacheSentinel -Value 'cache' -Encoding ASCII
            Set-Content -LiteralPath $versionsSentinel -Value 'managed=1' -Encoding ASCII
            Set-Content -LiteralPath $externalSentinel -Value 'external' -Encoding ASCII
            New-Item -ItemType Junction -Path $currentLink -Target $externalJdkDir | Out-Null

            # /S is intentionally the only argument: the installed copy must
            # infer uninstall mode and preserve data by default.
            Assert-Equal 0 (Invoke-GuiProcess $regularUninstaller @('/S')) 'Installed uninstaller did not infer uninstall mode'
            Assert-UserUninstallCompleted $regularInstallDir
            Assert-Equal $beforePath ([Environment]::GetEnvironmentVariable('Path', 'User')) 'Program-only uninstall did not restore user PATH'
            if (-not (Test-Path -LiteralPath $managedSentinel -PathType Leaf) -or
                -not (Test-Path -LiteralPath $cacheSentinel -PathType Leaf) -or
                -not (Test-Path -LiteralPath $currentLink)) {
                throw 'Program-only uninstall removed jvman data'
            }

            Assert-Equal 0 (Invoke-GuiProcess $setupPath $regularArguments) 'Reinstall before data removal failed'
            Assert-Equal 0 (
                Invoke-GuiProcess $regularUninstaller @('/S', '/UNINSTALL', '/REMOVE_DATA')
            ) 'Data-preserving-JDK uninstall failed'
            Assert-UserUninstallCompleted $regularInstallDir
            Assert-Equal $beforePath ([Environment]::GetEnvironmentVariable('Path', 'User')) 'Data removal did not restore user PATH'
            if (-not (Test-Path -LiteralPath $managedSentinel -PathType Leaf)) {
                throw 'Data removal deleted a managed JDK without /REMOVE_JDKS'
            }
            $unexpectedData = @(
                Get-ChildItem -LiteralPath $regularDataDir -Force |
                    Where-Object { $_.Name -ine 'jdks' }
            )
            if ($unexpectedData.Count -ne 0) {
                throw "Data removal left non-JDK entries: $($unexpectedData.Name -join ', ')"
            }
            if (-not (Test-Path -LiteralPath $externalSentinel -PathType Leaf)) {
                throw 'Data removal followed the current junction into an external JDK'
            }

            Assert-Equal 0 (Invoke-GuiProcess $setupPath $regularArguments) 'Reinstall before full removal failed'
            Set-Content -LiteralPath (Join-Path $managedJdkDir 'second.marker') -Value 'managed' -Encoding ASCII
            Assert-Equal 0 (
                Invoke-GuiProcess $regularUninstaller @('/S', '/UNINSTALL', '/REMOVE_DATA', '/REMOVE_JDKS')
            ) 'Full data and managed JDK uninstall failed'
            Assert-UserUninstallCompleted $regularInstallDir
            Assert-Equal $beforePath ([Environment]::GetEnvironmentVariable('Path', 'User')) 'Full uninstall did not restore user PATH'
            if (Test-Path -LiteralPath $regularDataDir) {
                throw 'Full uninstall left the jvman data directory'
            }
            if (-not (Test-Path -LiteralPath $externalSentinel -PathType Leaf)) {
                throw 'Full uninstall deleted an external registered JDK'
            }
            Assert-Equal $false (Test-Path -LiteralPath 'Registry::HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Uninstall\jvman') 'Uninstaller left Add/Remove Programs state'
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
