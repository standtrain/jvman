param(
    [Parameter(Mandatory = $true)]
    [string]$Binary
)

$ErrorActionPreference = 'Stop'
$binaryPath = (Resolve-Path -LiteralPath $Binary).Path
$unicodeName = ([string][char]0x6d4b) + ([char]0x8bd5) + ' ' +
               [char]::ConvertFromUtf32(0x1f600)
$testRoot = Join-Path $env:TEMP ("jvman $unicodeName-" + [Guid]::NewGuid().ToString('N'))
$archiveTestRoot = Join-Path $env:TEMP ("jvman-archive-" + [Guid]::NewGuid().ToString('N'))
$stateRoot = Join-Path $testRoot 'state'
$oldHome = $env:JVMAN_HOME
$oldJavaHome = $env:JAVA_HOME
$oldPath = $env:Path
$oldProgramFiles = $env:ProgramFiles
$oldProgramFilesX86 = [Environment]::GetEnvironmentVariable('ProgramFiles(x86)', 'Process')
$oldLocalAppData = $env:LOCALAPPDATA
$oldUserProfile = $env:USERPROFILE
$oldDiscoveryJavaBin = $env:JVMAN_TEST_JAVA_BIN
$oldLanguage = $env:JVMAN_LANG
$registryView = [Microsoft.Win32.RegistryView]::Default
$currentUserRegistry = [Microsoft.Win32.RegistryKey]::OpenBaseKey(
    [Microsoft.Win32.RegistryHive]::CurrentUser, $registryView)
$preferencesRegistryPath = 'Software\jvman\Preferences'
$installerRegistryPath = 'Software\jvman\Installer'
$languageRegistryName = 'Language'
$zhBanner = -join ([char[]]@(
    0x8f7b, 0x91cf, 0x7ea7, 0x20, 0x4a, 0x61, 0x76, 0x61, 0x20,
    0x7248, 0x672c, 0x7ba1, 0x7406, 0x5668
))
$zhUsageLabel = -join ([char[]]@(0x4f7f, 0x7528, 0x65b9, 0x6cd5))
$zhErrorUsageLabel = -join ([char[]]@(0x7528, 0x6cd5))
$zhLanguageLabel = -join ([char[]]@(0x8bed, 0x8a00))
$zhSimplifiedChinese = -join ([char[]]@(0x7b80, 0x4f53, 0x4e2d, 0x6587))
$zhName = -join ([char[]]@(0x540d, 0x79f0))
$zhTemplate = -join ([char[]]@(0x6a21, 0x677f))
$zhCustomSource = -join ([char[]]@(
    0x81ea, 0x5b9a, 0x4e49, 0x4e0b, 0x8f7d, 0x6e90
))
$zhCustomLabel = (-join ([char[]]@(0x81ea, 0x5b9a, 0x4e49))) +
    [char]0xff1a
$zhSourceLimit = $zhCustomSource + (-join ([char[]]@(
    0x6570, 0x91cf, 0x5df2, 0x8fbe, 0x4e0a, 0x9650
)))
$zhUrlRequirement = -join ([char[]]@(
    0x5fc5, 0x987b, 0x4f7f, 0x7528
))
$zhSupportedPlaceholders = -join ([char[]]@(
    0x652f, 0x6301, 0x7684, 0x5360, 0x4f4d, 0x7b26
))
$zhRegistered = -join ([char[]]@(0x5df2, 0x6ce8, 0x518c))
$zhUninstallHelp = "  jvman uninstall [<$zhName>]"
$zhSourceUsage = $zhErrorUsageLabel + [char]0xff1a +
    "jvman source [--list|--reset|<$zhName>|add <$zhName> " +
    "<HTTPS$zhTemplate>|remove <$zhName>]"
$consoleCodePageProbe = @'
using System.Runtime.InteropServices;
public static class JvmanConsoleCodePageProbe {
    [DllImport("kernel32.dll")]
    public static extern uint GetConsoleOutputCP();

    [DllImport("kernel32.dll")]
    public static extern bool SetConsoleOutputCP(uint codePage);
}
'@
Add-Type -TypeDefinition $consoleCodePageProbe
$preferencesKey = $currentUserRegistry.OpenSubKey($preferencesRegistryPath, $false)
$preferencesKeyExisted = $null -ne $preferencesKey
$hadPreferenceLanguage = $false
$oldPreferenceLanguage = $null
$oldPreferenceLanguageKind = $null
if ($preferencesKey) {
    $hadPreferenceLanguage = $preferencesKey.GetValueNames() -contains $languageRegistryName
    if ($hadPreferenceLanguage) {
        $oldPreferenceLanguage = $preferencesKey.GetValue(
            $languageRegistryName, $null,
            [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
        $oldPreferenceLanguageKind = $preferencesKey.GetValueKind($languageRegistryName)
    }
    $preferencesKey.Dispose()
}
$installerKey = $currentUserRegistry.OpenSubKey($installerRegistryPath, $false)
$installerKeyExisted = $null -ne $installerKey
$installerValueNames = ''
$installerHadLanguage = $false
$installerLanguage = $null
$installerLanguageKind = $null
if ($installerKey) {
    $installerValueNames = (@($installerKey.GetValueNames()) | Sort-Object) -join "`0"
    $installerHadLanguage = $installerKey.GetValueNames() -contains $languageRegistryName
    if ($installerHadLanguage) {
        $installerLanguage = $installerKey.GetValue(
            $languageRegistryName, $null,
            [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames).ToString()
        $installerLanguageKind = $installerKey.GetValueKind($languageRegistryName)
    }
    $installerKey.Dispose()
}
$duplicateJunction = $null
try {
    $bufferSize = $Host.UI.RawUI.BufferSize
    if ($bufferSize.Width -lt 4096) {
        $bufferSize.Width = 4096
        $Host.UI.RawUI.BufferSize = $bufferSize
    }
}
catch {
    # Native output may wrap on hosts that do not expose a configurable buffer.
}

function Invoke-Jvman {
    param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Arguments)
    $output = & $binaryPath @Arguments 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "jvman $($Arguments -join ' ') failed:`n$($output -join [Environment]::NewLine)"
    }
    return $output
}

function Invoke-JvmanExpectFailure {
    param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Arguments)
    $ErrorActionPreference = 'Continue'
    & $binaryPath @Arguments *> $null
    $exitCode = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    if ($exitCode -eq 0) { throw "jvman $($Arguments -join ' ') unexpectedly succeeded" }
}

function Invoke-JvmanFailureOutput {
    param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Arguments)
    $previousErrorActionPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $output = @(& $binaryPath @Arguments 2>&1)
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previousErrorActionPreference
    }
    if ($exitCode -eq 0) {
        throw "jvman $($Arguments -join ' ') unexpectedly succeeded"
    }
    return $output
}

function Assert-JvmanRestoresConsoleOutputCodePage {
    param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Arguments)
    $originalCodePage = [JvmanConsoleCodePageProbe]::GetConsoleOutputCP()
    if ($originalCodePage -eq 0) { return }
    $probeCodePage = if ($originalCodePage -eq 437) { 850 } else { 437 }
    if (-not [JvmanConsoleCodePageProbe]::SetConsoleOutputCP($probeCodePage)) {
        throw "cannot set console output code page probe to $probeCodePage"
    }
    try {
        $ErrorActionPreference = 'Continue'
        & $binaryPath @Arguments *> $null
        $exitCode = $LASTEXITCODE
        $ErrorActionPreference = 'Stop'
        if ($exitCode -ne 0) {
            throw "jvman $($Arguments -join ' ') failed during code page probe"
        }
        $restoredCodePage = [JvmanConsoleCodePageProbe]::GetConsoleOutputCP()
        if ($restoredCodePage -ne $probeCodePage) {
            throw "jvman left console output code page $restoredCodePage; expected $probeCodePage"
        }
    }
    finally {
        $ErrorActionPreference = 'Stop'
        [JvmanConsoleCodePageProbe]::SetConsoleOutputCP($originalCodePage) |
            Out-Null
    }
}

function New-FakeJdk {
    param(
        [string]$Path,
        [string]$Id,
        [string]$Version = '17.0.0-test',
        [string]$Implementor = ''
    )
    New-Item -ItemType Directory -Force -Path (Join-Path $Path 'bin') | Out-Null
    New-Item -ItemType File -Force -Path (Join-Path $Path 'bin\java.exe') | Out-Null
    New-Item -ItemType File -Force -Path (Join-Path $Path 'bin\javac.exe') | Out-Null
    New-Item -ItemType File -Force -Path (Join-Path $Path "$Id.marker") | Out-Null
    $release = @("JAVA_VERSION=`"$Version`"")
    if ($Implementor) { $release += "IMPLEMENTOR=`"$Implementor`"" }
    Set-Content -LiteralPath (Join-Path $Path 'release') -Value $release -Encoding ASCII
}

function New-FakeJre([string]$Path, [string]$Version) {
    New-Item -ItemType Directory -Force -Path (Join-Path $Path 'bin') | Out-Null
    New-Item -ItemType File -Force -Path (Join-Path $Path 'bin\java.exe') | Out-Null
    New-Item -ItemType File -Force -Path (Join-Path $Path 'jre-only.marker') | Out-Null
    Set-Content -LiteralPath (Join-Path $Path 'release') -Encoding ASCII -Value @(
        "JAVA_VERSION=`"$Version`"",
        'IMPLEMENTOR="Amazon Corretto"'
    )
}

function Get-DiscoveryRowsByVersion([object[]]$Output, [string]$Version) {
    $escapedVersion = [Regex]::Escape($Version)
    return @($Output | Where-Object {
        $_.ToString() -match "^(JDK|JRE|INVALID)\s+$escapedVersion\s+"
    })
}

function Get-RegistrationContent([string]$Name) {
    $path = Join-Path $stateRoot "versions\$Name.conf"
    return [System.IO.File]::ReadAllText($path, [System.Text.Encoding]::UTF8)
}

try {
    $env:JVMAN_LANG = 'zh-CN'
    $chineseHelp = (Invoke-Jvman) -join "`n"
    $hasChineseBanner = $chineseHelp -match [regex]::Escape($zhBanner)
    $hasChineseUsage = $chineseHelp -match [regex]::Escape($zhUsageLabel)
    $hasChineseUninstall = $chineseHelp -match
        [regex]::Escape($zhUninstallHelp)
    if (-not $hasChineseBanner -or -not $hasChineseUsage -or
        -not $hasChineseUninstall) {
        throw "JVMAN_LANG=zh-CN did not localize help " +
            "(banner=$hasChineseBanner, usage=$hasChineseUsage, " +
            "uninstall=$hasChineseUninstall):`n$chineseHelp"
    }
    $languageList = (Invoke-Jvman language --list) -join "`n"
    if ($languageList -notmatch [regex]::Escape($zhLanguageLabel) -or
        $languageList -notmatch [regex]::Escape($zhSimplifiedChinese)) {
        throw "language list was not localized:`n$languageList"
    }
    # Prime Windows PowerShell's native-output decoder before changing the
    # console code page used by the restoration probe.
    Assert-JvmanRestoresConsoleOutputCodePage version

    $env:JVMAN_LANG = 'en'
    $englishHelp = (Invoke-Jvman) -join "`n"
    if ($englishHelp -notmatch 'lightweight Java version manager' -or
        $englishHelp -notmatch '(?m)^Usage:$' -or
        $englishHelp -notmatch '(?m)^  jvman uninstall \[<name>\]$' -or
        $englishHelp -match [regex]::Escape($zhUsageLabel)) {
        throw "JVMAN_LANG=en did not force English help:`n$englishHelp"
    }
    $uninstallProbeDir = Join-Path $testRoot 'uninstall command probe'
    $uninstallProbe = Join-Path $uninstallProbeDir 'jvman.exe'
    New-Item -ItemType Directory -Force -Path $uninstallProbeDir | Out-Null
    Copy-Item -LiteralPath $binaryPath -Destination $uninstallProbe
    $ErrorActionPreference = 'Continue'
    $uninstallProbeOutput = & $uninstallProbe uninstall 2>&1
    $uninstallProbeExit = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    if ($uninstallProbeExit -eq 0 -or
        ($uninstallProbeOutput -join "`n") -notmatch
            'cannot start jvman uninstaller') {
        throw 'an unregistered executable copy reached the self-uninstall path'
    }
    if (-not $installerKeyExisted) {
        $registeredProbeDir = Join-Path $testRoot 'registered uninstall probe'
        $registeredProbeData = Join-Path $testRoot 'registered uninstall data'
        $registeredProbe = Join-Path $registeredProbeDir 'jvman.exe'
        $registeredUninstaller = Join-Path $registeredProbeDir 'uninstall.exe'
        $registeredMarker = Join-Path $registeredProbeDir 'install.marker'
        $registeredId = 'cli-test-' + [Guid]::NewGuid().ToString('N')
        New-Item -ItemType Directory -Force -Path $registeredProbeDir | Out-Null
        Copy-Item -LiteralPath $binaryPath -Destination $registeredProbe
        Copy-Item -LiteralPath $binaryPath -Destination $registeredUninstaller
        [IO.File]::WriteAllBytes(
            $registeredMarker,
            [Text.Encoding]::Unicode.GetBytes($registeredId))
        $probeInstallerKey = $currentUserRegistry.CreateSubKey(
            $installerRegistryPath, $true)
        try {
            $probeStateKey = $probeInstallerKey.CreateSubKey('State0', $true)
            try {
                $probeStateKey.SetValue(
                    'Version', '0.2.1',
                    [Microsoft.Win32.RegistryValueKind]::String)
                $probeStateKey.SetValue(
                    'InstallDir', $registeredProbeDir,
                    [Microsoft.Win32.RegistryValueKind]::String)
                $probeStateKey.SetValue(
                    'DataHome', $registeredProbeData,
                    [Microsoft.Win32.RegistryValueKind]::String)
                $probeStateKey.SetValue(
                    'InstallId', $registeredId,
                    [Microsoft.Win32.RegistryValueKind]::String)
            }
            finally {
                $probeStateKey.Dispose()
            }
            $probeInstallerKey.SetValue(
                'ActiveState', 0,
                [Microsoft.Win32.RegistryValueKind]::DWord)
        }
        finally {
            $probeInstallerKey.Dispose()
        }
        try {
            $ErrorActionPreference = 'Continue'
            & $registeredProbe uninstall *> $null
            $registeredExit = $LASTEXITCODE
            $ErrorActionPreference = 'Stop'
            if ($registeredExit -ne 0) {
                throw 'registered metadata without ARP did not launch the uninstaller'
            }
            [IO.File]::WriteAllBytes(
                $registeredMarker,
                [Text.Encoding]::Unicode.GetBytes('different-install-id'))
            $ErrorActionPreference = 'Continue'
            & $registeredProbe uninstall *> $null
            $mismatchedMarkerExit = $LASTEXITCODE
            $ErrorActionPreference = 'Stop'
            if ($mismatchedMarkerExit -eq 0) {
                throw 'jvman uninstall accepted a marker that did not match metadata'
            }
        }
        finally {
            $currentUserRegistry.DeleteSubKeyTree($installerRegistryPath, $false)
        }
    }
    Assert-JvmanRestoresConsoleOutputCodePage language zh-CN
    $writtenPreferencesKey = $currentUserRegistry.OpenSubKey(
        $preferencesRegistryPath, $false)
    if (-not $writtenPreferencesKey -or
        $writtenPreferencesKey.GetValueKind($languageRegistryName) -ne
            [Microsoft.Win32.RegistryValueKind]::String -or
        $writtenPreferencesKey.GetValue($languageRegistryName) -ne 'zh-CN') {
        throw 'language command did not persist an exact REG_SZ zh-CN preference'
    }
    $writtenPreferencesKey.Dispose()
    Remove-Item Env:JVMAN_LANG -ErrorAction SilentlyContinue
    $persistedChineseHelp = (Invoke-Jvman) -join "`n"
    if ($persistedChineseHelp -notmatch [regex]::Escape($zhUsageLabel)) {
        throw "the persisted zh-CN language was not loaded:`n$persistedChineseHelp"
    }
    Invoke-Jvman language en | Out-Null
    $persistedEnglishHelp = (Invoke-Jvman) -join "`n"
    if ($persistedEnglishHelp -notmatch '(?m)^Usage:$' -or
        $persistedEnglishHelp -match [regex]::Escape($zhUsageLabel)) {
        throw "the persisted English language was not loaded:`n$persistedEnglishHelp"
    }

    $installerKeyAfter = $currentUserRegistry.OpenSubKey($installerRegistryPath, $false)
    if (($null -ne $installerKeyAfter) -ne $installerKeyExisted) {
        throw 'language command created or removed the Installer metadata key'
    }
    if ($installerKeyAfter) {
        $installerValueNamesAfter = (@($installerKeyAfter.GetValueNames()) |
            Sort-Object) -join "`0"
        $installerHadLanguageAfter = $installerKeyAfter.GetValueNames() -contains
            $languageRegistryName
        if ($installerValueNamesAfter -ne $installerValueNames -or
            $installerHadLanguageAfter -ne $installerHadLanguage) {
            throw 'language command changed the Installer metadata values'
        }
        if ($installerHadLanguage) {
            $installerLanguageAfter = $installerKeyAfter.GetValue(
                $languageRegistryName, $null,
                [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames).ToString()
            $installerLanguageKindAfter = $installerKeyAfter.GetValueKind(
                $languageRegistryName)
            if ($installerLanguageAfter -ne $installerLanguage -or
                $installerLanguageKindAfter -ne $installerLanguageKind) {
                throw 'language command altered the Installer Language metadata'
            }
        }
        $installerKeyAfter.Dispose()
    }
    $env:JVMAN_LANG = 'en'

    $userHome = Join-Path $testRoot 'user home'
    $localAppData = Join-Path $testRoot 'local app data'
    $programFiles = Join-Path $localAppData 'Programs'
    $discoveredA = Join-Path $programFiles 'Eclipse Adoptium\jdk-discovery-a'
    $discoveredB = Join-Path $userHome '.jdks\jdk-discovery-b'
    $discoveredC = Join-Path $testRoot 'zz-fixtures\jdk-discovery-c'
    $discoveredJre = Join-Path $testRoot 'fixtures\embedded-jre'
    $discoveredInvalid = Join-Path $testRoot 'fixtures\invalid-java'
    $duplicateTarget = Join-Path $testRoot 'fixtures\junction-target'
    $duplicateJunction = Join-Path $userHome '.jdks\junction-alias'
    $nameConflict = Join-Path $testRoot 'fixtures\name-conflict'
    $discoveryVersion = '91.7.3-jvman-test'
    $discoveryName = "temurin-$discoveryVersion"
    $discoveryName2 = "$discoveryName-2"
    $discoveryName3 = "$discoveryName-3"

    New-FakeJdk $discoveredA 'discovered-a' $discoveryVersion 'Eclipse Adoptium'
    New-FakeJdk $discoveredB 'discovered-b' $discoveryVersion 'Eclipse Adoptium'
    New-FakeJdk $discoveredC 'discovered-c' $discoveryVersion 'Eclipse Adoptium'
    New-FakeJre $discoveredJre '77.0.1-jvman-test'
    New-FakeJdk $discoveredInvalid 'invalid' 'unterminated' 'Test Vendor'
    Set-Content -LiteralPath (Join-Path $discoveredInvalid 'release') `
        -Value 'JAVA_VERSION="unterminated' -Encoding ASCII
    New-FakeJdk $nameConflict 'conflict' '92.0.0-jvman-test' 'Test Vendor'
    New-FakeJdk $duplicateTarget 'junction-target' '93.0.0-jvman-test' 'Microsoft'
    New-Item -ItemType Junction -Path $duplicateJunction `
             -Target $duplicateTarget | Out-Null

    $env:JVMAN_HOME = $stateRoot
    $env:JAVA_HOME = "`"$discoveredA`""
    $env:JVMAN_TEST_JAVA_BIN = Join-Path $duplicateTarget 'bin'
    $env:Path = @(
        "`"$(Join-Path $discoveredA 'bin')`"",
        (Join-Path $discoveredB 'bin'),
        (Join-Path $discoveredC 'bin'),
        (Join-Path $discoveredJre 'bin'),
        (Join-Path $discoveredInvalid 'bin'),
        '%JVMAN_TEST_JAVA_BIN%',
        $oldPath
    ) -join ';'
    $env:ProgramFiles = $programFiles
    [Environment]::SetEnvironmentVariable(
        'ProgramFiles(x86)', (Join-Path $testRoot 'program files x86'), 'Process')
    $env:LOCALAPPDATA = $localAppData
    $env:USERPROFILE = $userHome

    $env:JVMAN_LANG = 'zh-CN'
    try {
        $sourceUsageError = (Invoke-JvmanFailureOutput source --list extra) -join "`n"
        if ($sourceUsageError -notmatch [regex]::Escape($zhSourceUsage)) {
            throw "source usage error was not localized:`n$sourceUsageError"
        }
        $sourceUrlError = (Invoke-JvmanFailureOutput source add invalid-url `
            'http://example.test/{major}') -join "`n"
        if ($sourceUrlError -notmatch [regex]::Escape($zhCustomSource) -or
            $sourceUrlError -notmatch [regex]::Escape($zhUrlRequirement) -or
            $sourceUrlError -notmatch [regex]::Escape($zhSupportedPlaceholders) -or
            $sourceUrlError -notmatch [regex]::Escape('{archive}')) {
            throw "custom source URL error was not localized:`n$sourceUrlError"
        }
    }
    finally {
        $env:JVMAN_LANG = 'en'
    }

    $preview = @(Invoke-Jvman discover)
    $previewText = $preview -join "`n"
    if ($previewText -notmatch
        'TYPE\s+VERSION\s+VENDOR\s+NAME\s+STATUS\s+SOURCES\s+JAVA_HOME') {
        throw 'discover did not print the documented columns'
    }
    if (Test-Path -LiteralPath $stateRoot) {
        throw 'discover preview created JVMAN_HOME'
    }
    $discoveryRows = @(Get-DiscoveryRowsByVersion $preview $discoveryVersion)
    $rowsA = @($discoveryRows | Where-Object { $_ -match 'program-files' })
    $rowsB = @($discoveryRows | Where-Object { $_ -match 'user-jdks' })
    $rowsJre = @(Get-DiscoveryRowsByVersion $preview '77.0.1-jvman-test')
    $rowsDuplicate = @(Get-DiscoveryRowsByVersion $preview '93.0.0-jvman-test')
    $rowsInvalid = @($preview | Where-Object {
        $_ -match '^INVALID\s+' -and $_ -match '\sinvalid\s+' -and $_ -match '\sPATH\s+'
    })
    if ($rowsA.Count -ne 1 -or $rowsA[0] -notmatch '^JDK\s+' -or
        $rowsA[0] -notmatch '\snew\s+' -or
        $rowsA[0] -notmatch 'JAVA_HOME,PATH,program-files') {
        throw "discover did not merge JAVA_HOME, PATH and common-root sources:`n$previewText"
    }
    if ($rowsB.Count -ne 1 -or $rowsB[0] -notmatch '^JDK\s+' -or
        $rowsB[0] -notmatch '\snew\s+' -or
        $rowsB[0] -notmatch 'PATH,user-jdks') {
        throw "discover did not merge PATH and user-jdks sources:`n$($rowsB -join "`n")"
    }
    if ($rowsJre.Count -ne 1 -or $rowsJre[0] -notmatch '^JRE\s+' -or
        $rowsJre[0] -notmatch '\sjre\s+') {
        throw "discover did not retain the JRE preview item:`n$($rowsJre -join "`n")"
    }
    if ($rowsDuplicate.Count -ne 1 -or
        $rowsDuplicate[0] -notmatch 'PATH,user-jdks') {
        throw "discover did not canonicalize the duplicate junction:`n$($rowsDuplicate -join "`n")"
    }
    if ($rowsInvalid.Count -ne 1 -or $rowsInvalid[0] -notmatch '^INVALID\s+' -or
        $rowsInvalid[0] -notmatch '\sinvalid\s+') {
        throw "discover did not retain the invalid preview item:`n$($rowsInvalid -join "`n")"
    }
    Invoke-JvmanExpectFailure discover --unknown

    # Update argument validation must finish before any network or state access.
    Invoke-JvmanExpectFailure update --unknown
    Invoke-JvmanExpectFailure update --version
    Invoke-JvmanExpectFailure update --version 1.2
    Invoke-JvmanExpectFailure update --check unexpected
    Invoke-JvmanExpectFailure update --check --check
    Invoke-JvmanExpectFailure update --version 0.2.0 --version 0.3.0
    $currentVersion = (Invoke-Jvman version).Trim()
    $updateCheck = (Invoke-Jvman update --check --version $currentVersion) -join "`n"
    if ($updateCheck -notmatch 'already up to date') {
        throw "same-version update check did not use the update command:`n$updateCheck"
    }
    $sameVersionUpdate = (Invoke-Jvman update --version $currentVersion) -join "`n"
    if ($sameVersionUpdate -notmatch 'already up to date') {
        throw "same-version update did not verify the running executable marker:`n$sameVersionUpdate"
    }
    $savedHome = $env:JVMAN_HOME
    try {
        $env:JVMAN_HOME = 'x' * 5000
        $independentCheck = (Invoke-Jvman update --check --version $currentVersion) -join "`n"
        if ($independentCheck -notmatch 'already up to date') {
            throw 'update unexpectedly depended on JVMAN_HOME initialization'
        }
    } finally {
        $env:JVMAN_HOME = $savedHome
    }
    if (Test-Path -LiteralPath $stateRoot) {
        throw 'update argument checks created JVMAN_HOME'
    }

    if ((Invoke-Jvman source).Trim() -ne 'auto') {
        throw 'default download source is not automatic'
    }
    Invoke-JvmanExpectFailure source unknown
    Invoke-Jvman source foojay | Out-Null
    if ((Invoke-Jvman source).Trim() -ne 'foojay' -or
        (Get-Content -LiteralPath (Join-Path $stateRoot 'source.conf') -Raw).Trim() -ne 'foojay') {
        throw 'download source selection was not persisted'
    }
    $sourceList = (Invoke-Jvman source --list) -join "`n"
    if ($sourceList -notmatch '(?m)^\* foojay' -or
        $sourceList -notmatch '(?m)^  auto' -or
        $sourceList -notmatch '(?m)^  adoptium' -or
        $sourceList -notmatch '(?m)^  tsinghua' -or
        $sourceList -notmatch '(?m)^  huawei' -or
        $sourceList -notmatch '(?m)^  aliyun') {
        throw 'download source list did not mark the active source'
    }
    Invoke-JvmanExpectFailure install 21 --source unknown
    Invoke-Jvman source auto | Out-Null
    if ((Invoke-Jvman source).Trim() -ne 'auto' -or
        (Get-Content -LiteralPath (Join-Path $stateRoot 'source.conf') -Raw).Trim() -ne 'auto') {
        throw 'automatic download source selection was not persisted'
    }
    Invoke-Jvman source --reset | Out-Null
    if ((Invoke-Jvman source).Trim() -ne 'auto' -or
        (Test-Path -LiteralPath (Join-Path $stateRoot 'source.conf'))) {
        throw 'download source reset did not restore the default'
    }
    Invoke-JvmanExpectFailure source add insecure 'http://example.test/{major}'
    Invoke-JvmanExpectFailure source add incomplete 'https://example.test/latest'
    Invoke-JvmanExpectFailure source add adoptium 'https://example.test/{major}'
    $customTemplate = 'https://jdk.example.test/v3/{major}?os={os}&arch={arch}&ext={archive}'
    Invoke-Jvman source add company $customTemplate | Out-Null
    $customList = (Invoke-Jvman source --list) -join "`n"
    if ($customList -notmatch '(?m)^  company\s+Custom: company') {
        throw 'custom download source was not listed'
    }
    $env:JVMAN_LANG = 'zh-CN'
    try {
        $localizedCustomList = (Invoke-Jvman source --list) -join "`n"
        $localizedCustomPattern = '(?m)^  company\s+' +
            [regex]::Escape($zhCustomLabel + 'company') + '$'
        if ($localizedCustomList -notmatch $localizedCustomPattern) {
            throw "custom download source label was not localized:`n$localizedCustomList"
        }
    }
    finally {
        $env:JVMAN_LANG = 'en'
    }
    Invoke-Jvman source company | Out-Null
    if ((Invoke-Jvman source).Trim() -ne 'company') {
        throw 'custom download source could not be selected'
    }
    Invoke-JvmanExpectFailure source remove company
    Invoke-Jvman source auto | Out-Null
    Invoke-Jvman source remove company | Out-Null
    if (Test-Path -LiteralPath (Join-Path $stateRoot 'sources\company.conf')) {
        throw 'custom download source configuration was not removed'
    }
    $brokenSource = Join-Path $stateRoot 'sources\broken.conf'
    Set-Content -LiteralPath $brokenSource -Encoding ASCII -Value 'invalid=true'
    Invoke-JvmanExpectFailure source --list
    Invoke-Jvman source remove broken | Out-Null
    if (Test-Path -LiteralPath $brokenSource) {
        throw 'invalid custom source could not be removed for recovery'
    }
    0..31 | ForEach-Object {
        $limitSource = Join-Path $stateRoot ("sources\\limit{0}.conf" -f $_)
        Set-Content -LiteralPath $limitSource -Encoding ASCII -Value @(
            'type=adoptium'
            ("url=https://limit{0}.example.test/{{major}}" -f $_)
        )
    }
    $env:JVMAN_LANG = 'zh-CN'
    try {
        $sourceLimitError = (Invoke-JvmanFailureOutput source add overflow `
            'https://overflow.example.test/{major}') -join "`n"
        if ($sourceLimitError -notmatch [regex]::Escape($zhSourceLimit)) {
            throw "custom source limit error was not localized:`n$sourceLimitError"
        }
    }
    finally {
        $env:JVMAN_LANG = 'en'
    }
    if (Test-Path -LiteralPath (Join-Path $stateRoot 'sources\overflow.conf')) {
        throw 'custom source limit failure still created a configuration'
    }
    0..31 | ForEach-Object {
        Remove-Item -LiteralPath (Join-Path $stateRoot ("sources\\limit{0}.conf" -f $_))
    }

    Invoke-Jvman add $discoveryName $nameConflict | Out-Null
    Invoke-Jvman add manual-discovered $discoveredA | Out-Null
    $manualConfig = Get-RegistrationContent 'manual-discovered'
    $canonicalDiscoveredA = @($manualConfig -split "\r?\n" |
        Where-Object { $_ -like 'home=*' })[0].Substring(5)
    $registeredPreview = @(Invoke-Jvman discover)
    $registeredRowsA = @(Get-DiscoveryRowsByVersion `
        $registeredPreview $discoveryVersion | Where-Object {
            $_ -match 'program-files' -and $_ -match 'registered:manual-discovered'
        })
    if ($registeredRowsA.Count -ne 1 -or
        $registeredRowsA[0] -notmatch 'registered:manual-discovered') {
        throw 'discover did not match an existing registration by canonical home'
    }
    $env:JVMAN_LANG = 'zh-CN'
    try {
        $localizedRegisteredPreview = (Invoke-Jvman discover) -join "`n"
        if ($localizedRegisteredPreview -notmatch
            [regex]::Escape("${zhRegistered}:manual-discovered")) {
            throw "registered discovery status was not localized:`n$localizedRegisteredPreview"
        }
    }
    finally {
        $env:JVMAN_LANG = 'en'
    }

    Invoke-Jvman discover --register | Out-Null
    $configurationFiles = @(
        Get-ChildItem -LiteralPath (Join-Path $stateRoot 'versions') `
                      -Filter '*.conf' -File
    )
    $sameHomeConfigurations = @($configurationFiles | Where-Object {
        ([System.IO.File]::ReadAllText(
            $_.FullName, [System.Text.Encoding]::UTF8) -split "\r?\n") -contains
            "home=$canonicalDiscoveredA"
    })
    if ($sameHomeConfigurations.Count -ne 1 -or
        $sameHomeConfigurations[0].BaseName -ne 'manual-discovered') {
        $configurationDump = $configurationFiles | ForEach-Object {
            "$($_.Name):$([System.IO.File]::ReadAllText($_.FullName, [System.Text.Encoding]::UTF8))"
        }
        throw "discover replaced or aliased the existing same-home registration; expected $canonicalDiscoveredA; configs: $($configurationDump -join ' | ')"
    }
    $autoConfig = Get-RegistrationContent $discoveryName2
    $autoHome = @($autoConfig -split "\r?\n" |
        Where-Object { $_ -like 'home=*' })[0].Substring(5)
    if (-not (Test-Path -LiteralPath (Join-Path $autoHome 'discovered-b.marker'))) {
        throw 'discover did not allocate the stable -2 conflict name'
    }
    $autoConfig3 = Get-RegistrationContent $discoveryName3
    $autoHome3 = @($autoConfig3 -split "\r?\n" |
        Where-Object { $_ -like 'home=*' })[0].Substring(5)
    if (-not (Test-Path -LiteralPath (Join-Path $autoHome3 'discovered-c.marker'))) {
        throw 'discover did not allocate the stable -3 conflict name'
    }
    $nonJdkRegistered = @($configurationFiles | Where-Object {
        $lines = [System.IO.File]::ReadAllText(
            $_.FullName, [System.Text.Encoding]::UTF8) -split "\r?\n"
        $homeLine = @($lines | Where-Object { $_ -like 'home=*' })[0]
        $registeredHome = $homeLine.Substring(5)
        (Test-Path -LiteralPath (Join-Path $registeredHome 'jre-only.marker')) -or
            (Test-Path -LiteralPath (Join-Path $registeredHome 'invalid.marker'))
    })
    if ($nonJdkRegistered.Count -ne 0) {
        throw 'discover --register registered a JRE or invalid candidate'
    }
    if ($autoConfig -notmatch '(?m)^managed=0$') {
        throw 'discovered JDK was not registered with external semantics'
    }
    if ((Test-Path -LiteralPath (Join-Path $stateRoot 'current')) -or
        (Test-Path -LiteralPath (Join-Path $stateRoot 'current.version'))) {
        throw 'discover --register selected a current JDK'
    }
    $configurationCount = @(
        Get-ChildItem -LiteralPath (Join-Path $stateRoot 'versions') `
                      -Filter '*.conf' -File
    ).Count
    Invoke-Jvman discover --register | Out-Null
    $configurationCountAgain = @(
        Get-ChildItem -LiteralPath (Join-Path $stateRoot 'versions') `
                      -Filter '*.conf' -File
    ).Count
    if ($configurationCountAgain -ne $configurationCount) {
        throw 'discover --register was not idempotent'
    }
    Invoke-Jvman remove $discoveryName2 | Out-Null
    Invoke-Jvman remove $discoveryName3 | Out-Null
    if (-not (Test-Path -LiteralPath (Join-Path $discoveredB 'bin\javac.exe'))) {
        throw 'removing a discovered registration deleted the external JDK'
    }
    if (-not (Test-Path -LiteralPath (Join-Path $discoveredC 'bin\javac.exe'))) {
        throw 'removing a discovered registration deleted the external JDK'
    }

    $env:JAVA_HOME = $oldJavaHome
    $env:Path = $oldPath
    $env:ProgramFiles = $oldProgramFiles
    [Environment]::SetEnvironmentVariable('ProgramFiles(x86)', $oldProgramFilesX86,
                                           'Process')
    $env:LOCALAPPDATA = $oldLocalAppData
    $env:USERPROFILE = $oldUserProfile

    $jdkA = Join-Path $testRoot 'fixtures\jdk-a'
    $jdkB = Join-Path $testRoot 'fixtures\jdk-b'
    $legacyUninstallJdk = Join-Path $testRoot 'fixtures\jdk-legacy-uninstall'
    New-FakeJdk $jdkA 'a'
    New-FakeJdk $jdkB 'b'
    New-FakeJdk $legacyUninstallJdk 'legacy-uninstall'

    Invoke-Jvman add a $jdkA | Out-Null
    Invoke-Jvman add b $jdkB | Out-Null
    Invoke-Jvman add legacy-uninstall $legacyUninstallJdk | Out-Null
    Invoke-Jvman uninstall legacy-uninstall | Out-Null
    if ((Test-Path -LiteralPath (Join-Path $stateRoot 'versions\legacy-uninstall.conf')) -or
        -not (Test-Path -LiteralPath (Join-Path $legacyUninstallJdk 'bin\javac.exe'))) {
        throw 'uninstall <name> no longer preserves the JDK removal alias'
    }
    $list = (Invoke-Jvman list) -join "`n"
    if ($list -notmatch 'a' -or $list -notmatch 'b') { throw 'list did not show both JDKs' }

    $useOutput = (Invoke-Jvman use a) -join "`n"
    if ($useOutput -notmatch 'jvman init') {
        throw "use did not explain how to initialize the current shell:`n$useOutput"
    }
    if ((Invoke-Jvman current) -ne 'a') { throw 'current did not return a' }
    if (-not (Test-Path -LiteralPath (Join-Path $stateRoot 'current\bin\java.exe'))) {
        throw 'current junction does not expose the selected JDK'
    }
    $cmdInitProbe = Join-Path $testRoot 'cmd-init-probe.cmd'
    $oldCmdBinaryDir = [Environment]::GetEnvironmentVariable(
        'JVMAN_TEST_BINARY_DIR', 'Process')
    $oldCmdExpectedHome = [Environment]::GetEnvironmentVariable(
        'JVMAN_TEST_EXPECTED_HOME', 'Process')
    try {
        $cmdExpectedHome = Join-Path ((Invoke-Jvman home).Trim()) 'current'
        [Environment]::SetEnvironmentVariable(
            'JVMAN_TEST_BINARY_DIR', (Split-Path -Parent $binaryPath), 'Process')
        [Environment]::SetEnvironmentVariable(
            'JVMAN_TEST_EXPECTED_HOME', $cmdExpectedHome, 'Process')
        Set-Content -LiteralPath $cmdInitProbe -Encoding ASCII -Value @(
            '@echo off',
            'setlocal DisableDelayedExpansion',
            'set "JAVA_HOME="',
            'set "PATH=%JVMAN_TEST_BINARY_DIR%;%SystemRoot%\System32"',
            'for /f "delims=" %%L in (''jvman.exe init cmd'') do @call %%L',
            'if /i not "%JAVA_HOME%"=="%JVMAN_TEST_EXPECTED_HOME%" (',
            '  echo CMD JAVA_HOME mismatch: actual="%JAVA_HOME%" expected="%JVMAN_TEST_EXPECTED_HOME%" 1>&2',
            '  exit /b 61',
            ')',
            'for /f "tokens=1 delims=;" %%J in ("%PATH%") do (',
            '  if /i not "%%~fJ"=="%JVMAN_TEST_EXPECTED_HOME%\bin" (',
            '    echo CMD PATH mismatch: actual="%%~fJ" expected="%JVMAN_TEST_EXPECTED_HOME%\bin" 1>&2',
            '    exit /b 62',
            '  )',
            '  if not exist "%%~fJ\java.exe" exit /b 63',
            '  exit /b 0',
            ')',
            'exit /b 64'
        )
        & cmd.exe /d /c $cmdInitProbe
        if ($LASTEXITCODE -ne 0) {
            throw "the suggested CMD initialization command failed with exit code $LASTEXITCODE"
        }
    }
    finally {
        [Environment]::SetEnvironmentVariable(
            'JVMAN_TEST_BINARY_DIR', $oldCmdBinaryDir, 'Process')
        [Environment]::SetEnvironmentVariable(
            'JVMAN_TEST_EXPECTED_HOME', $oldCmdExpectedHome, 'Process')
        Remove-Item -LiteralPath $cmdInitProbe -Force -ErrorAction SilentlyContinue
    }
    $pathBeforeInitializedUse = $env:Path
    try {
        $env:Path = (Join-Path $stateRoot 'current\bin') + ';' + $env:Path
        $initializedUseOutput = (Invoke-Jvman use a) -join "`n"
        if ($initializedUseOutput -match 'jvman init') {
            throw "use printed an initialization hint for an initialized shell:`n$initializedUseOutput"
        }
    }
    finally {
        $env:Path = $pathBeforeInitializedUse
    }
    $registeredA = (Invoke-Jvman which a).Trim()
    $childHome = (& $binaryPath exec a -- cmd.exe /d /c 'echo %JAVA_HOME%').Trim()
    if ($LASTEXITCODE -ne 0 -or $childHome -ne $registeredA) { throw 'exec did not set JAVA_HOME' }

    Invoke-JvmanExpectFailure remove a
    Invoke-JvmanExpectFailure remove A

    $stateStream = [System.IO.File]::Open(
        (Join-Path $stateRoot 'current.version'),
        [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::Read,
        [System.IO.FileShare]::Read)
    try {
        Invoke-JvmanExpectFailure use b
    }
    finally {
        $stateStream.Dispose()
    }
    if ((Invoke-Jvman current) -ne 'a' -or
        -not (Test-Path -LiteralPath (Join-Path $stateRoot 'current\a.marker')) -or
        (Test-Path -LiteralPath (Join-Path $stateRoot 'current\b.marker'))) {
        throw 'failed state update changed the current junction'
    }

    $cmdDir = Join-Path $testRoot 'command path'
    New-Item -ItemType Directory -Force -Path $cmdDir | Out-Null
    Set-Content -LiteralPath (Join-Path $cmdDir 'auditcmd.cmd') -Encoding ASCII -Value @(
        '@echo off',
        'echo CMD:%~1'
    )
    Set-Content -LiteralPath (Join-Path $cmdDir 'auditcmd') -Encoding ASCII -Value 'not an executable'
    $env:Path = "$cmdDir;$env:Path"
    $batchOutput = (& $binaryPath exec a -- auditcmd 'hello world').Trim()
    if ($LASTEXITCODE -ne 0 -or $batchOutput -ne 'CMD:hello world') {
        throw 'exec did not resolve or quote a PATH .cmd command'
    }

    Invoke-Jvman add via-current (Join-Path $stateRoot 'current') | Out-Null
    if ((Invoke-Jvman which via-current).Trim() -ne $registeredA) {
        throw 'registration through current was not canonicalized'
    }
    Invoke-Jvman use via-current | Out-Null
    if (-not (Test-Path -LiteralPath (Join-Path $stateRoot 'current\a.marker'))) {
        throw 'canonicalized current registration produced a self-junction'
    }
    Invoke-Jvman use b | Out-Null
    Set-Content -LiteralPath (Join-Path $stateRoot 'current.version') -Value 'a' -Encoding ASCII
    $doctorOutput = (& $binaryPath doctor 2>&1) -join "`n"
    if ($LASTEXITCODE -eq 0 -or $doctorOutput -notmatch 'current state and directory link do not match') {
        throw 'doctor did not detect a mismatched current state and junction'
    }
    Set-Content -LiteralPath (Join-Path $stateRoot 'current.version') -Value 'b' -Encoding ASCII
    Invoke-Jvman remove via-current | Out-Null
    Invoke-Jvman remove a | Out-Null

    $archiveSource = Join-Path $archiveTestRoot 'archive-source'
    $packedJdk = Join-Path $archiveSource 'fake-jdk'
    $archive = Join-Path $archiveTestRoot 'fake-jdk.tar'
    $env:JVMAN_HOME = Join-Path $archiveTestRoot 'state'
    New-FakeJdk $packedJdk 'packed'
    & tar.exe -cf $archive -C $archiveSource 'fake-jdk'
    if ($LASTEXITCODE -ne 0) { throw 'could not create local test archive' }
    $archiveHash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
    Invoke-JvmanExpectFailure install packed --archive $archive --sha256 ('0' * 64)
    Invoke-JvmanExpectFailure install packed --archive $archive --source foojay
    $untrustedCwd = Join-Path $testRoot 'untrusted cwd'
    New-Item -ItemType Directory -Force -Path $untrustedCwd | Out-Null
    New-Item -ItemType File -Force -Path (Join-Path $untrustedCwd 'tar.exe') | Out-Null
    Push-Location $untrustedCwd
    try {
        Invoke-Jvman install packed --archive $archive --sha256 $archiveHash | Out-Null
    }
    finally {
        Pop-Location
    }
    Invoke-JvmanExpectFailure install packed --archive $archive
    if (-not (Test-Path -LiteralPath $archive)) { throw 'failed duplicate install removed the source archive' }
    $packedHome = (Invoke-Jvman which packed).Trim()
    if (-not (Test-Path -LiteralPath (Join-Path $packedHome 'bin\javac.exe'))) {
        throw 'local archive install is incomplete'
    }
    Invoke-Jvman remove packed | Out-Null

    Write-Host 'All CLI integration tests passed.'
}
finally {
    $restorePreferencesKey = $currentUserRegistry.CreateSubKey(
        $preferencesRegistryPath, $true)
    if ($hadPreferenceLanguage) {
        $restorePreferencesKey.SetValue(
            $languageRegistryName, $oldPreferenceLanguage,
            $oldPreferenceLanguageKind)
    } else {
        $restorePreferencesKey.DeleteValue($languageRegistryName, $false)
    }
    $removePreferencesKey = -not $preferencesKeyExisted -and
        $restorePreferencesKey.GetValueNames().Count -eq 0 -and
        $restorePreferencesKey.GetSubKeyNames().Count -eq 0
    $restorePreferencesKey.Dispose()
    if ($removePreferencesKey) {
        $currentUserRegistry.DeleteSubKey($preferencesRegistryPath, $false)
    }
    if ($duplicateJunction -and (Test-Path -LiteralPath $duplicateJunction)) {
        [System.IO.Directory]::Delete($duplicateJunction, $false)
    }
    if (Test-Path -LiteralPath (Join-Path $stateRoot 'current')) {
        # PowerShell 5.1 Remove-Item has a junction-specific NullReferenceException.
        [System.IO.Directory]::Delete((Join-Path $stateRoot 'current'), $false)
    }
    if (Test-Path -LiteralPath $testRoot) {
        Remove-Item -LiteralPath $testRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
    if (Test-Path -LiteralPath $archiveTestRoot) {
        Remove-Item -LiteralPath $archiveTestRoot -Recurse -Force `
                    -ErrorAction SilentlyContinue
    }
    $env:JVMAN_HOME = $oldHome
    $env:JAVA_HOME = $oldJavaHome
    $env:Path = $oldPath
    $env:ProgramFiles = $oldProgramFiles
    [Environment]::SetEnvironmentVariable('ProgramFiles(x86)', $oldProgramFilesX86,
                                           'Process')
    $env:LOCALAPPDATA = $oldLocalAppData
    $env:USERPROFILE = $oldUserProfile
    $env:JVMAN_TEST_JAVA_BIN = $oldDiscoveryJavaBin
    if ($null -eq $oldLanguage) {
        Remove-Item Env:JVMAN_LANG -ErrorAction SilentlyContinue
    } else {
        $env:JVMAN_LANG = $oldLanguage
    }
    $currentUserRegistry.Dispose()
}
