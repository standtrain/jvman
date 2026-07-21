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

function Invoke-GuiProcess(
    [string]$FilePath,
    [string[]]$Arguments,
    [int]$TimeoutSeconds = 30
) {
    $process = Start-Process -FilePath $FilePath -ArgumentList $Arguments -PassThru -WindowStyle Hidden
    if ($null -eq $process) {
        throw "Could not start $FilePath"
    }
    try {
        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
            $process.Kill()
            throw "$FilePath did not exit within $TimeoutSeconds seconds"
        }
        return [int]$process.ExitCode
    }
    finally {
        $process.Dispose()
    }
}

function Assert-Equal([object]$Expected, [object]$Actual, [string]$Message) {
    if ($Expected -cne $Actual) {
        throw "$Message (expected '$Expected', got '$Actual')"
    }
}

function Set-PreferenceLanguage([string]$Language) {
    $key = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey(
        'Software\jvman\Preferences',
        [Microsoft.Win32.RegistryKeyPermissionCheck]::ReadWriteSubTree)
    try {
        $key.SetValue('Language', $Language,
                      [Microsoft.Win32.RegistryValueKind]::String)
    }
    finally {
        $key.Dispose()
    }
}

function Get-PreferenceLanguage() {
    $key = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey(
        'Software\jvman\Preferences', $false)
    if (-not $key) { return $null }
    try {
        return $key.GetValue('Language', $null,
                             [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
    }
    finally {
        $key.Dispose()
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

function Assert-PathStartsWith(
    [AllowNull()][string]$PathValue,
    [string]$Expected,
    [string]$Message
) {
    $expectedPath = [IO.Path]::GetFullPath($Expected).TrimEnd([char[]]@('\\', '/'))
    $first = @($PathValue -split ';' | Where-Object { $_ })[0]
    if ($null -eq $first) { throw "$Message (PATH is empty)" }
    $candidate = [Environment]::ExpandEnvironmentVariables(
        $first.Trim().Trim('"'))
    $candidate = [IO.Path]::GetFullPath($candidate).TrimEnd([char[]]@('\\', '/'))
    if (-not [string]::Equals($candidate, $expectedPath,
                             [StringComparison]::OrdinalIgnoreCase)) {
        throw "$Message (expected '$Expected', got '$first')"
    }
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
$englishInstallDir = Join-Path $root 'english-install'
$englishDataDir = Join-Path $root 'english-data'
$legacyInstallDir = Join-Path $root 'legacy-install'
$legacyDataDir = Join-Path $root 'legacy-data'
$legacyJavaBin = Join-Path $legacyDataDir 'current\bin'
$beforePath = [Environment]::GetEnvironmentVariable('Path', 'User')
$beforeJavaHome = [Environment]::GetEnvironmentVariable('JAVA_HOME', 'User')
$beforeProcessJvmanHome = [Environment]::GetEnvironmentVariable('JVMAN_HOME', 'Process')
$beforeProcessLanguage = [Environment]::GetEnvironmentVariable('JVMAN_LANG', 'Process')
$beforeInstallerState = Test-Path -LiteralPath 'Registry::HKEY_CURRENT_USER\Software\jvman\Installer'
$beforeArpState = Test-Path -LiteralPath 'Registry::HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Uninstall\jvman'
$beforeMachinePath = [Environment]::GetEnvironmentVariable('Path', 'Machine')
$beforeMachineInstallerState = Test-Path -LiteralPath 'Registry::HKEY_LOCAL_MACHINE\Software\jvman\Installer'
$beforeMachineArpState = Test-Path -LiteralPath 'Registry::HKEY_LOCAL_MACHINE\Software\Microsoft\Windows\CurrentVersion\Uninstall\jvman'
$machineInstallDir = Join-Path (
    [Environment]::GetFolderPath([Environment+SpecialFolder]::ProgramFiles)
) 'jvman'
$machineDataDir = Join-Path (
    [Environment]::GetFolderPath([Environment+SpecialFolder]::CommonApplicationData)
) 'jvman'
$beforeMachineInstallDir = Test-Path -LiteralPath $machineInstallDir
$beforeMachineDataDir = Test-Path -LiteralPath $machineDataDir
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
$isElevated = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
$preferencesPath = 'Software\jvman\Preferences'
$preferencesKey = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($preferencesPath, $false)
$preferencesKeyExisted = $null -ne $preferencesKey
$preferenceLanguageExisted = $false
$beforePreferenceLanguage = $null
$beforePreferenceLanguageKind = $null
if ($preferencesKey) {
    try {
        $preferenceLanguageExisted = $preferencesKey.GetValueNames() -contains 'Language'
        if ($preferenceLanguageExisted) {
            $beforePreferenceLanguage = $preferencesKey.GetValue(
                'Language', $null,
                [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
            $beforePreferenceLanguageKind = $preferencesKey.GetValueKind('Language')
        }
    }
    finally {
        $preferencesKey.Dispose()
    }
}

New-Item -ItemType Directory -Path $portableDir -Force | Out-Null
try {
    Remove-Item Env:JVMAN_LANG -ErrorAction SilentlyContinue
    $createdNew = $false
    $instanceGuard = [Threading.Mutex]::new(
        $true,
        'Local\jvman.Setup.1F07284D-788B-4F89-A327-DA0F15511708',
        [ref]$createdNew)
    try {
        Assert-Equal $true $createdNew 'Installer instance test mutex already existed'
        $blockedArguments = @('/S', '/PORTABLE', "/DIR=`"$blockedDir`"")
        Assert-Equal 3 (Invoke-GuiProcess $setupPath $blockedArguments) 'Concurrent installer was not rejected'
        if (Test-Path -LiteralPath (Join-Path $blockedDir 'jvman.exe')) {
            throw 'Rejected concurrent installer wrote a payload'
        }

        if (-not $isElevated) {
            $resumeArguments = @(
                '/S',
                '/_JVMAN_MACHINE_ELEVATED_V1',
                '/_JVMAN_LANG=0',
                '/SYSTEM_PATH'
            )
            $resumeTimer = [Diagnostics.Stopwatch]::StartNew()
            Assert-Equal 1 (Invoke-GuiProcess $setupPath $resumeArguments 5) 'Forged elevated install resume was not rejected'
            $resumeTimer.Stop()
            if ($resumeTimer.Elapsed.TotalSeconds -ge 5) {
                throw 'Forged elevated install resume reached the mutex handoff path'
            }

            $uninstallResumeArguments = @(
                '/S',
                '/_JVMAN_MACHINE_ELEVATED_V1',
                '/_JVMAN_LANG=1',
                '/UNINSTALL',
                '/MACHINE'
            )
            Assert-Equal 1 (Invoke-GuiProcess $setupPath $uninstallResumeArguments 5) 'Forged elevated uninstall resume was not rejected'
            Assert-Equal 2 (
                Invoke-GuiProcess $setupPath @('/S', '/_JVMAN_MACHINE_ELEVATED_V1', '/SYSTEM_PATH') 5
            ) 'Malformed elevated resume was not rejected during parsing'
        }
    }
    finally {
        $instanceGuard.ReleaseMutex()
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
    if ($LASTEXITCODE -ne 0 -or ($version -join "`n") -notmatch '0\.2\.1') {
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
    Assert-Equal 2 (
        Invoke-GuiProcess $setupPath @('/S', '/UNINSTALL', '/MACHINE', '/REMOVE_DATA')
    ) 'Machine uninstall accepted a per-user data removal scope'

    if (-not $beforeInstallerState -and -not $beforeArpState) {
        $regularUninstaller = Join-Path $regularInstallDir 'uninstall.exe'
        $englishUninstaller = Join-Path $englishInstallDir 'uninstall.exe'
        $managedJdkDir = Join-Path $regularDataDir 'jdks\managed-test'
        $managedSentinel = Join-Path $managedJdkDir 'managed.marker'
        $externalJdkDir = Join-Path $root 'external-jdk'
        $externalSentinel = Join-Path $externalJdkDir 'external.marker'
        $currentLink = Join-Path $regularDataDir 'current'
        Set-PreferenceLanguage 'en'
        [Environment]::SetEnvironmentVariable('JVMAN_HOME', $regularDataDir, 'Process')
        try {
            $regularArguments = @(
                '/S', '/LANG=zh-CN', '/ADD_TO_PATH', '/USER_PATH',
                '/CONFIGURE_JAVA', '/REPLACE_JAVA_HOME',
                "/DIR=`"$regularInstallDir`""
            )
            Assert-Equal 0 (Invoke-GuiProcess $setupPath $regularArguments) 'Regular install failed'
            if (-not (Test-Path -LiteralPath $regularUninstaller -PathType Leaf)) {
                throw "Regular install did not create $regularUninstaller"
            }
            Assert-Equal 'zh-CN' (Get-PreferenceLanguage) 'Chinese setup did not replace the prior English CLI preference'
            $installedCli = Join-Path $regularInstallDir 'jvman.exe'
            $localizedHelp = & $installedCli 2>&1
            $localizedExit = $LASTEXITCODE
            $localizedText = $localizedHelp -join "`n"
            $zhUsage = -join ([char[]]@(0x7528, 0x6cd5, 0xff1a))
            if ($localizedExit -ne 0 -or
                $localizedText -notmatch [regex]::Escape($zhUsage)) {
                throw "Chinese setup did not select Chinese CLI help:`n$localizedText"
            }
            $installedUserPath = [Environment]::GetEnvironmentVariable('Path', 'User')
            $stableJavaBin = Join-Path $regularDataDir 'current\bin'
            Assert-PathContains $installedUserPath $regularInstallDir 'Regular install did not add its program directory to user PATH'
            Assert-PathContains $installedUserPath $stableJavaBin 'Regular install did not add current\bin to user PATH'
            Assert-PathStartsWith $installedUserPath $stableJavaBin 'Regular install did not prioritize current\bin in user PATH'
            Assert-Equal $currentLink (
                [Environment]::GetEnvironmentVariable('JAVA_HOME', 'User')
            ) 'Regular install did not configure stable JAVA_HOME before the first use'
            if (Test-Path -LiteralPath $stableJavaBin) {
                throw 'Regular install unexpectedly required an existing current JDK'
            }

            $reorderedUserPath = (@(
                $installedUserPath -split ';' |
                    Where-Object { $_ -and $_ -ine $stableJavaBin }
            ) + @($stableJavaBin)) -join ';'
            [Environment]::SetEnvironmentVariable(
                'Path', $reorderedUserPath, 'User')
            Assert-Equal 0 (
                Invoke-GuiProcess $setupPath $regularArguments
            ) 'Upgrade did not migrate an owned current\\bin PATH entry'
            Assert-PathStartsWith (
                [Environment]::GetEnvironmentVariable('Path', 'User')
            ) $stableJavaBin 'Upgrade left its owned current\\bin behind an older Java entry'

            $userEnvironmentKey = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey(
                'Environment', $true)
            if (-not $userEnvironmentKey) {
                throw 'Could not open the user environment registry key'
            }
            try {
                $prioritizedUserPath = [string]$userEnvironmentKey.GetValue(
                    'Path', $null,
                    [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
                if (-not ($userEnvironmentKey.GetValueNames() -contains 'JAVA_HOME')) {
                    throw 'Managed JAVA_HOME is missing before the rollback test'
                }
                $managedJavaHome = [string]$userEnvironmentKey.GetValue(
                    'JAVA_HOME', $null,
                    [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
                $managedJavaHomeKind = $userEnvironmentKey.GetValueKind('JAVA_HOME')
                $rollbackPath = (@(
                    $prioritizedUserPath -split ';' |
                        Where-Object { $_ -ine $regularInstallDir }
                ) + @($regularInstallDir)) -join ';'
                $userEnvironmentKey.SetValue(
                    'Path', $rollbackPath,
                    [Microsoft.Win32.RegistryValueKind]::ExpandString)
            }
            finally {
                $userEnvironmentKey.Dispose()
            }
            $conflictingJavaHome = Join-Path $root 'foreign-java-home'
            $failedUpgradeArguments = @(
                '/S', '/LANG=zh-CN', '/ADD_TO_PATH', '/USER_PATH',
                '/CONFIGURE_JAVA',
                "/DIR=`"$regularInstallDir`""
            )
            try {
                $userEnvironmentKey = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey(
                    'Environment', $true)
                if (-not $userEnvironmentKey) {
                    throw 'Could not write the conflicting JAVA_HOME value'
                }
                try {
                    $userEnvironmentKey.SetValue(
                        'JAVA_HOME', $conflictingJavaHome, $managedJavaHomeKind)
                }
                finally {
                    $userEnvironmentKey.Dispose()
                }
                Assert-Equal 1 (
                    Invoke-GuiProcess $setupPath $failedUpgradeArguments
                ) 'Upgrade with a conflicting JAVA_HOME returned an unexpected exit code'
                $userEnvironmentKey = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey(
                    'Environment', $false)
                if (-not $userEnvironmentKey) {
                    throw 'Failed upgrade removed the user environment registry key'
                }
                try {
                    $restoredPath = [string]$userEnvironmentKey.GetValue(
                        'Path', $null,
                        [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
                    if (-not [string]::Equals(
                            $rollbackPath, $restoredPath,
                            [StringComparison]::Ordinal)) {
                        throw 'Failed upgrade did not exactly restore user PATH'
                    }
                    Assert-Equal ([Microsoft.Win32.RegistryValueKind]::ExpandString) (
                        $userEnvironmentKey.GetValueKind('Path')
                    ) 'Failed upgrade did not restore the user PATH registry type'
                    $restoredJavaHome = [string]$userEnvironmentKey.GetValue(
                        'JAVA_HOME', $null,
                        [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
                    if (-not [string]::Equals(
                            $conflictingJavaHome, $restoredJavaHome,
                            [StringComparison]::Ordinal)) {
                        throw 'Failed upgrade changed the conflicting JAVA_HOME value'
                    }
                }
                finally {
                    $userEnvironmentKey.Dispose()
                }
            }
            finally {
                $userEnvironmentKey = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey(
                    'Environment',
                    [Microsoft.Win32.RegistryKeyPermissionCheck]::ReadWriteSubTree)
                try {
                    $userEnvironmentKey.SetValue(
                        'JAVA_HOME', $managedJavaHome, $managedJavaHomeKind)
                }
                finally {
                    $userEnvironmentKey.Dispose()
                }
            }

            New-Item -ItemType Directory -Path $managedJdkDir -Force | Out-Null
            New-Item -ItemType Directory -Path (Join-Path $regularDataDir 'versions') -Force | Out-Null
            New-Item -ItemType Directory -Path (Join-Path $regularDataDir 'cache') -Force | Out-Null
            New-Item -ItemType Directory -Path $externalJdkDir -Force | Out-Null
            Set-Content -LiteralPath $managedSentinel -Value 'managed' -Encoding ASCII
            Set-Content -LiteralPath (Join-Path $regularDataDir 'versions\managed-test.conf') -Value 'managed=1' -Encoding ASCII
            Set-Content -LiteralPath (Join-Path $regularDataDir 'cache\download.tmp') -Value 'cache' -Encoding ASCII
            Set-Content -LiteralPath $externalSentinel -Value 'external' -Encoding ASCII
            New-Item -ItemType Junction -Path $currentLink -Target $externalJdkDir | Out-Null

            # /S is intentionally the only argument: the installed copy must
            # infer uninstall mode and preserve data by default.
            Assert-Equal 0 (Invoke-GuiProcess $regularUninstaller @('/S')) 'Installed uninstaller did not infer uninstall mode'
            Assert-UserUninstallCompleted $regularInstallDir
            Assert-Equal $beforePath ([Environment]::GetEnvironmentVariable('Path', 'User')) 'Program-only uninstall did not restore user PATH'
            Assert-Equal $beforeJavaHome ([Environment]::GetEnvironmentVariable('JAVA_HOME', 'User')) 'Program-only uninstall did not restore user JAVA_HOME'
            if (-not (Test-Path -LiteralPath $managedSentinel -PathType Leaf) -or
                -not (Test-Path -LiteralPath (Join-Path $regularDataDir 'cache\download.tmp') -PathType Leaf) -or
                -not (Test-Path -LiteralPath $currentLink)) {
                throw 'Program-only uninstall removed jvman data'
            }

            Assert-Equal 0 (Invoke-GuiProcess $setupPath $regularArguments) 'Reinstall before data removal failed'
            Assert-Equal 0 (
                Invoke-GuiProcess $regularUninstaller @('/S', '/UNINSTALL', '/REMOVE_DATA')
            ) 'Data-preserving-JDK uninstall failed'
            Assert-UserUninstallCompleted $regularInstallDir
            Assert-Equal $beforePath ([Environment]::GetEnvironmentVariable('Path', 'User')) 'Data removal did not restore user PATH'
            Assert-Equal $beforeJavaHome ([Environment]::GetEnvironmentVariable('JAVA_HOME', 'User')) 'Data removal did not restore user JAVA_HOME'
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
            Assert-Equal $beforeJavaHome ([Environment]::GetEnvironmentVariable('JAVA_HOME', 'User')) 'Full uninstall did not restore user JAVA_HOME'
            if (Test-Path -LiteralPath $regularDataDir) {
                throw 'Full uninstall left the jvman data directory'
            }
            if (-not (Test-Path -LiteralPath $externalSentinel -PathType Leaf)) {
                throw 'Full uninstall deleted an external registered JDK'
            }
            Assert-Equal $false (Test-Path -LiteralPath 'Registry::HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Uninstall\jvman') 'Uninstaller left Add/Remove Programs state'

            Set-PreferenceLanguage 'zh-CN'
            [Environment]::SetEnvironmentVariable('JVMAN_HOME', $englishDataDir, 'Process')
            $englishArguments = @('/S', '/LANG=en', '/NO_PATH', "/DIR=`"$englishInstallDir`"")
            Assert-Equal 0 (Invoke-GuiProcess $setupPath $englishArguments) 'English language install failed'
            Assert-Equal 'en' (Get-PreferenceLanguage) 'English setup did not replace the prior Chinese CLI preference'
            $englishCli = Join-Path $englishInstallDir 'jvman.exe'
            $englishHelp = & $englishCli 2>&1
            $englishExit = $LASTEXITCODE
            $englishText = $englishHelp -join "`n"
            if ($englishExit -ne 0 -or $englishText -notmatch '(?m)^Usage:$' -or
                $englishText -match [regex]::Escape($zhUsage)) {
                throw "English setup did not select English CLI help:`n$englishText"
            }
            Assert-Equal 0 (
                Invoke-GuiProcess $englishUninstaller @('/S')
            ) 'English language install could not be uninstalled'
            Assert-UserUninstallCompleted $englishInstallDir

            if ($isElevated) {
                $legacyUninstaller = Join-Path $legacyInstallDir 'uninstall.exe'
                $legacyMachinePath = [Environment]::GetEnvironmentVariable(
                    'Path', 'Machine')
                try {
                    [Environment]::SetEnvironmentVariable(
                        'JVMAN_HOME', $legacyDataDir, 'Process')
                    $legacyArguments = @('/S', '/LANG=en', '/NO_PATH', "/DIR=`"$legacyInstallDir`"")
                    Assert-Equal 0 (
                        Invoke-GuiProcess $setupPath $legacyArguments
                    ) 'Legacy migration fixture install failed'

                    $metadataRoot = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey(
                        'Software\jvman\Installer', $true)
                    if (-not $metadataRoot) { throw 'Legacy fixture metadata is missing' }
                    try {
                        $activeState = [int]$metadataRoot.GetValue('ActiveState', -1)
                        if ($activeState -lt 0 -or $activeState -gt 1) {
                            throw 'Legacy fixture ActiveState is invalid'
                        }
                        $stateKey = $metadataRoot.OpenSubKey("State$activeState", $false)
                        if (-not $stateKey) { throw 'Legacy fixture active state is missing' }
                        try {
                            foreach ($valueName in $stateKey.GetValueNames()) {
                                $metadataRoot.SetValue(
                                    $valueName,
                                    $stateKey.GetValue(
                                        $valueName, $null,
                                        [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames),
                                    $stateKey.GetValueKind($valueName))
                            }
                        }
                        finally {
                            $stateKey.Dispose()
                        }
                        $metadataRoot.DeleteValue('ActiveState', $false)
                        $metadataRoot.DeleteSubKeyTree('State0', $false)
                        $metadataRoot.DeleteSubKeyTree('State1', $false)
                        $metadataRoot.SetValue(
                            'AppPathOwned', 1,
                            [Microsoft.Win32.RegistryValueKind]::DWord)
                        $metadataRoot.SetValue(
                            'AppPathScope', 1,
                            [Microsoft.Win32.RegistryValueKind]::DWord)
                        $metadataRoot.SetValue(
                            'JavaPathOwned', 1,
                            [Microsoft.Win32.RegistryValueKind]::DWord)
                        $metadataRoot.SetValue(
                            'JavaPathScope', 1,
                            [Microsoft.Win32.RegistryValueKind]::DWord)
                    }
                    finally {
                        $metadataRoot.Dispose()
                    }

                    $legacyPathWithEntry = if ([string]::IsNullOrEmpty($legacyMachinePath)) {
                        $legacyInstallDir
                    }
                    elseif ($legacyMachinePath.EndsWith(';')) {
                        $legacyMachinePath + $legacyInstallDir
                    }
                    else {
                        $legacyMachinePath + ';' + $legacyInstallDir
                    }
                    $legacyPathWithEntries = $legacyPathWithEntry + ';' +
                        $legacyJavaBin
                    [Environment]::SetEnvironmentVariable(
                        'Path', $legacyPathWithEntries, 'Machine')
                    $failedLegacyUpgrade = Invoke-GuiProcess $corruptSetup @(
                        '/S', '/LANG=en', '/NO_PATH',
                        "/DIR=`"$legacyInstallDir`""
                    ) 60
                    if ($failedLegacyUpgrade -eq 0) {
                        throw 'Corrupted upgrade unexpectedly succeeded'
                    }
                    Assert-Equal $legacyPathWithEntries (
                        [Environment]::GetEnvironmentVariable('Path', 'Machine')
                    ) 'Failed upgrade did not restore all legacy system PATH entries'
                    $failedMetadata = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey(
                        'Software\jvman\Installer', $false)
                    if (-not $failedMetadata) {
                        throw 'Failed upgrade removed legacy installer metadata'
                    }
                    try {
                        Assert-Equal 1 ([int]$failedMetadata.GetValue('AppPathOwned', 0)) `
                            'Failed upgrade cleared legacy PATH ownership'
                        Assert-Equal 1 ([int]$failedMetadata.GetValue('AppPathScope', 0)) `
                            'Failed upgrade changed legacy PATH scope'
                        Assert-Equal 1 ([int]$failedMetadata.GetValue('JavaPathOwned', 0)) `
                            'Failed upgrade cleared legacy Java PATH ownership'
                        Assert-Equal 1 ([int]$failedMetadata.GetValue('JavaPathScope', 0)) `
                            'Failed upgrade changed legacy Java PATH scope'
                    }
                    finally {
                        $failedMetadata.Dispose()
                    }
                    Assert-Equal 0 (
                        Invoke-GuiProcess $setupPath @('/S', '/LANG=en', '/UNINSTALL') 60
                    ) 'Legacy HKCU machine-PATH uninstall failed'
                    Assert-UserUninstallCompleted $legacyInstallDir
                    Assert-Equal $legacyMachinePath (
                        [Environment]::GetEnvironmentVariable('Path', 'Machine')
                    ) 'Legacy migration did not restore system PATH'
                }
                finally {
                    [Environment]::SetEnvironmentVariable(
                        'Path', $legacyMachinePath, 'Machine')
                    if ((Test-Path -LiteralPath $legacyUninstaller -PathType Leaf) -and
                        (Test-Path -LiteralPath 'Registry::HKEY_CURRENT_USER\Software\jvman\Installer')) {
                        try {
                            [void](Invoke-GuiProcess $legacyUninstaller @('/S', '/UNINSTALL') 60)
                        }
                        catch {
                            Write-Warning 'Could not clean up the legacy migration fixture.'
                        }
                    }
                }
            }
        }
        finally {
            [Environment]::SetEnvironmentVariable('JVMAN_HOME', $beforeProcessJvmanHome, 'Process')
            foreach ($candidateUninstaller in @($regularUninstaller, $englishUninstaller)) {
                if ((Test-Path -LiteralPath $candidateUninstaller -PathType Leaf) -and
                    (Test-Path -LiteralPath 'Registry::HKEY_CURRENT_USER\Software\jvman\Installer')) {
                    try {
                        [void](Invoke-GuiProcess $candidateUninstaller @('/S', '/UNINSTALL'))
                    }
                    catch {
                        Write-Warning 'Could not clean up a temporary regular installation.'
                    }
                }
            }
        }
    }

    if ($isElevated -and -not $beforeMachineInstallerState -and
        -not $beforeMachineArpState -and -not $beforeMachineInstallDir -and
        -not $beforeMachineDataDir) {
        $machineUninstaller = Join-Path $machineInstallDir 'uninstall.exe'
        try {
            Set-PreferenceLanguage 'zh-CN'
            $machineArguments = @('/S', '/LANG=en', '/SYSTEM_PATH')
            Assert-Equal 0 (
                Invoke-GuiProcess $setupPath $machineArguments 60
            ) 'Elevated machine install failed'
            if (-not (Test-Path -LiteralPath (Join-Path $machineInstallDir 'jvman.exe') -PathType Leaf) -or
                -not (Test-Path -LiteralPath $machineUninstaller -PathType Leaf)) {
                throw 'Machine install did not publish its protected program files'
            }
            Assert-Equal $true (
                Test-Path -LiteralPath 'Registry::HKEY_LOCAL_MACHINE\Software\jvman\Installer'
            ) 'Machine install did not create HKLM metadata'
            Assert-Equal $true (
                Test-Path -LiteralPath 'Registry::HKEY_LOCAL_MACHINE\Software\Microsoft\Windows\CurrentVersion\Uninstall\jvman'
            ) 'Machine install did not create an HKLM uninstall record'
            Assert-PathContains (
                [Environment]::GetEnvironmentVariable('Path', 'Machine')
            ) $machineInstallDir 'Machine install did not add Program Files to system PATH'
            Assert-Equal 'en' (Get-PreferenceLanguage) 'Machine install did not update the invoking user language preference'
            $machineHelp = & (Join-Path $machineInstallDir 'jvman.exe') 2>&1
            if ($LASTEXITCODE -ne 0 -or ($machineHelp -join "`n") -notmatch '(?m)^Usage:$') {
                throw "Machine-installed CLI did not use English:`n$($machineHelp -join "`n")"
            }

            Assert-Equal 0 (
                Invoke-GuiProcess $setupPath $machineArguments 60
            ) 'Repeated machine install failed'
            Assert-Equal 2 (
                Invoke-GuiProcess $machineUninstaller @('/S', '/REMOVE_DATA') 5
            ) 'Machine uninstaller accepted a per-user data removal scope'
            if (-not (Test-Path -LiteralPath $machineUninstaller -PathType Leaf) -or
                -not (Test-Path -LiteralPath 'Registry::HKEY_LOCAL_MACHINE\Software\jvman\Installer')) {
                throw 'Rejected machine data removal changed the machine installation'
            }
            Assert-Equal 0 (
                Invoke-GuiProcess $machineUninstaller @('/S') 60
            ) 'Installed machine uninstaller failed'
            $machineCleanupDeadline = [DateTime]::UtcNow.AddSeconds(10)
            while (((Test-Path -LiteralPath $machineInstallDir) -or
                    (Test-Path -LiteralPath 'Registry::HKEY_LOCAL_MACHINE\Software\jvman\Installer') -or
                    (Test-Path -LiteralPath 'Registry::HKEY_LOCAL_MACHINE\Software\Microsoft\Windows\CurrentVersion\Uninstall\jvman')) -and
                   [DateTime]::UtcNow -lt $machineCleanupDeadline) {
                Start-Sleep -Milliseconds 100
            }
            Assert-Equal $beforeMachinePath (
                [Environment]::GetEnvironmentVariable('Path', 'Machine')
            ) 'Machine uninstall did not restore system PATH'
            Assert-Equal $false (
                Test-Path -LiteralPath 'Registry::HKEY_LOCAL_MACHINE\Software\jvman\Installer'
            ) 'Machine uninstall left HKLM metadata'
            Assert-Equal $false (
                Test-Path -LiteralPath 'Registry::HKEY_LOCAL_MACHINE\Software\Microsoft\Windows\CurrentVersion\Uninstall\jvman'
            ) 'Machine uninstall left its HKLM uninstall record'
            Assert-Equal $false (
                Test-Path -LiteralPath $machineInstallDir
            ) 'Machine uninstall left the Program Files directory'
        }
        finally {
            if ((Test-Path -LiteralPath $machineUninstaller -PathType Leaf) -and
                (Test-Path -LiteralPath 'Registry::HKEY_LOCAL_MACHINE\Software\jvman\Installer')) {
                try {
                    [void](Invoke-GuiProcess $setupPath @('/S', '/UNINSTALL', '/MACHINE') 60)
                }
                catch {
                    Write-Warning 'Could not clean up the temporary machine installation.'
                }
            }
        }
    }

    Assert-Equal $beforePath ([Environment]::GetEnvironmentVariable('Path', 'User')) 'Portable install changed user PATH'
    Assert-Equal $beforeJavaHome ([Environment]::GetEnvironmentVariable('JAVA_HOME', 'User')) 'Portable install changed user JAVA_HOME'
    Assert-Equal $beforeInstallerState (Test-Path -LiteralPath 'Registry::HKEY_CURRENT_USER\Software\jvman\Installer') 'Portable install changed installer registry state'
    Assert-Equal $beforeArpState (Test-Path -LiteralPath 'Registry::HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Uninstall\jvman') 'Portable install changed Add/Remove Programs state'
    Assert-Equal $beforeMachinePath ([Environment]::GetEnvironmentVariable('Path', 'Machine')) 'Installer tests changed machine PATH'
    Assert-Equal $beforeMachineInstallerState (Test-Path -LiteralPath 'Registry::HKEY_LOCAL_MACHINE\Software\jvman\Installer') 'Installer tests changed machine installer state'
    Assert-Equal $beforeMachineArpState (Test-Path -LiteralPath 'Registry::HKEY_LOCAL_MACHINE\Software\Microsoft\Windows\CurrentVersion\Uninstall\jvman') 'Installer tests changed machine Add/Remove Programs state'
    Assert-Equal $beforeMachineInstallDir (Test-Path -LiteralPath $machineInstallDir) 'Installer tests changed the Program Files machine directory state'
    Assert-Equal $beforeMachineDataDir (Test-Path -LiteralPath $machineDataDir) 'Installer tests changed the ProgramData machine directory state'
    Write-Output 'Installer integration and integrity tests passed.'
}
finally {
    try {
        [Environment]::SetEnvironmentVariable('Path', $beforePath, 'User')
        [Environment]::SetEnvironmentVariable('JAVA_HOME', $beforeJavaHome, 'User')
        if ($isElevated -and -not $beforeMachineInstallerState -and
            -not $beforeMachineArpState -and -not $beforeMachineInstallDir) {
            [Environment]::SetEnvironmentVariable(
                'Path', $beforeMachinePath, 'Machine')
        }
        [Environment]::SetEnvironmentVariable('JVMAN_HOME', $beforeProcessJvmanHome, 'Process')
        [Environment]::SetEnvironmentVariable('JVMAN_LANG', $beforeProcessLanguage, 'Process')
        $restorePreferences = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey(
            $preferencesPath,
            [Microsoft.Win32.RegistryKeyPermissionCheck]::ReadWriteSubTree)
        try {
            if ($preferenceLanguageExisted) {
                $restorePreferences.SetValue(
                    'Language', $beforePreferenceLanguage,
                    $beforePreferenceLanguageKind)
            }
            else {
                $restorePreferences.DeleteValue('Language', $false)
            }
            $removePreferences = -not $preferencesKeyExisted -and
                $restorePreferences.GetValueNames().Count -eq 0 -and
                $restorePreferences.GetSubKeyNames().Count -eq 0
        }
        finally {
            $restorePreferences.Dispose()
        }
        if ($removePreferences) {
            [Microsoft.Win32.Registry]::CurrentUser.DeleteSubKey(
                $preferencesPath, $false)
        }
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
