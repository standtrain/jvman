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

$setupPath = Resolve-ExistingFile $Setup 'Setup'
$binaryPath = Resolve-ExistingFile $Binary 'Payload binary'
$root = Join-Path ([IO.Path]::GetTempPath()) ("jvman-installer-test-" + [guid]::NewGuid().ToString('N'))
$portableDir = Join-Path $root '中文 path with spaces'
$corruptDir = Join-Path $root 'corrupt-target'
$corruptSetup = Join-Path $root 'corrupt setup.exe'
$beforePath = [Environment]::GetEnvironmentVariable('Path', 'User')
$beforeJavaHome = [Environment]::GetEnvironmentVariable('JAVA_HOME', 'User')
$beforeInstallerState = Test-Path -LiteralPath 'Registry::HKEY_CURRENT_USER\Software\jvman\Installer'
$beforeArpState = Test-Path -LiteralPath 'Registry::HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Uninstall\jvman'

New-Item -ItemType Directory -Path $portableDir -Force | Out-Null
try {
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

    Assert-Equal $beforePath ([Environment]::GetEnvironmentVariable('Path', 'User')) 'Portable install changed user PATH'
    Assert-Equal $beforeJavaHome ([Environment]::GetEnvironmentVariable('JAVA_HOME', 'User')) 'Portable install changed user JAVA_HOME'
    Assert-Equal $beforeInstallerState (Test-Path -LiteralPath 'Registry::HKEY_CURRENT_USER\Software\jvman\Installer') 'Portable install changed installer registry state'
    Assert-Equal $beforeArpState (Test-Path -LiteralPath 'Registry::HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Uninstall\jvman') 'Portable install changed Add/Remove Programs state'
    Write-Output 'Installer portable and integrity tests passed.'
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
