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
    New-FakeJdk $jdkA 'a'
    New-FakeJdk $jdkB 'b'

    Invoke-Jvman add a $jdkA | Out-Null
    Invoke-Jvman add b $jdkB | Out-Null
    $list = (Invoke-Jvman list) -join "`n"
    if ($list -notmatch 'a' -or $list -notmatch 'b') { throw 'list did not show both JDKs' }

    Invoke-Jvman use a | Out-Null
    if ((Invoke-Jvman current) -ne 'a') { throw 'current did not return a' }
    if (-not (Test-Path -LiteralPath (Join-Path $stateRoot 'current\bin\java.exe'))) {
        throw 'current junction does not expose the selected JDK'
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
}
