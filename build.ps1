[CmdletBinding()]
param(
    [ValidateSet('build', 'all', 'app', 'web', 'pack', 'clean', 'rebuild', 'run')]
    [string]$Target = 'build',

    [switch]$SkipNpmInstall,

    [string]$Output,

    [string]$AdminUser = $(if ($env:RUN_ADMIN_USER) { $env:RUN_ADMIN_USER } else { 'admin' }),

    [string]$AdminPassword = $env:RUN_ADMIN_PASSWORD
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $Root

$IsWindowsHost = $env:OS -eq 'Windows_NT'
$ExeExt = if ($IsWindowsHost) { '.exe' } else { '' }

$BuildDir = Join-Path $Root 'build'
$WebDir = Join-Path $Root 'web'
$DistDir = Join-Path $WebDir 'dist'
$PackedC = Join-Path $BuildDir 'generated\packed_fs.c'
$DefaultOutput = Join-Path $BuildDir "mongoose-svelte$ExeExt"

if ([string]::IsNullOrWhiteSpace($Output)) {
    $Output = $DefaultOutput
}

function Split-CommandLine {
    param([string]$Text)

    if ([string]::IsNullOrWhiteSpace($Text)) {
        return @()
    }

    $items = @()
    $builder = [System.Text.StringBuilder]::new()
    $quote = [char]0
    $inToken = $false

    foreach ($ch in $Text.ToCharArray()) {
        if (($ch -eq '"' -or $ch -eq "'") -and $quote -eq [char]0) {
            $quote = $ch
            $inToken = $true
            continue
        }

        if ($quote -ne [char]0 -and $ch -eq $quote) {
            $quote = [char]0
            $inToken = $true
            continue
        }

        if ([char]::IsWhiteSpace($ch) -and $quote -eq [char]0) {
            if ($inToken) {
                $items += $builder.ToString()
                [void]$builder.Clear()
                $inToken = $false
            }
            continue
        }

        [void]$builder.Append($ch)
        $inToken = $true
    }

    if ($quote -ne [char]0) {
        throw "Unclosed quote in command-line value: $Text"
    }

    if ($inToken) {
        $items += $builder.ToString()
    }

    return $items
}

function Get-EnvArgs {
    param(
        [string]$Value,
        [string[]]$Default = @()
    )

    if ([string]::IsNullOrWhiteSpace($Value)) {
        return @($Default)
    }

    return @(Split-CommandLine $Value)
}

function Require-Command {
    param([string]$Name)

    if (-not (Get-Command $Name -ErrorAction SilentlyContinue)) {
        throw "Missing required command '$Name'. Install it or add it to PATH."
    }
}

function Invoke-External {
    param(
        [string]$FilePath,
        [string[]]$Arguments = @()
    )

    Write-Host "> $FilePath $($Arguments -join ' ')"
    & $FilePath @Arguments

    if ($LASTEXITCODE -ne 0) {
        throw "'$FilePath' failed with exit code $LASTEXITCODE."
    }
}

function Get-FlagsFromTool {
    param(
        [string]$CommandName,
        [string[]]$Arguments,
        [string[]]$Fallback = @()
    )

    if (-not (Get-Command $CommandName -ErrorAction SilentlyContinue)) {
        return @($Fallback)
    }

    $output = & $CommandName @Arguments 2>$null
    if ($LASTEXITCODE -ne 0) {
        return @($Fallback)
    }

    $text = (($output | Where-Object { $_ }) -join ' ').Trim()
    if ([string]::IsNullOrWhiteSpace($text)) {
        return @($Fallback)
    }

    return @(Split-CommandLine $text)
}

function Get-PythonCommand {
    foreach ($candidate in @('python3', 'python', 'py')) {
        if (Get-Command $candidate -ErrorAction SilentlyContinue) {
            return $candidate
        }
    }

    throw "Missing required command 'python'. Install Python 3 or add it to PATH."
}

function Ensure-WebDependencies {
    if ($SkipNpmInstall) {
        Write-Host 'Skipping npm dependency install.'
        return
    }

    Require-Command 'npm'

    $stamp = Join-Path $WebDir 'node_modules\.install-stamp'
    $packageFiles = @(
        (Join-Path $WebDir 'package.json'),
        (Join-Path $WebDir 'package-lock.json')
    ) | Where-Object { Test-Path $_ }

    $needsInstall = -not (Test-Path $stamp)
    if (-not $needsInstall) {
        $stampTime = (Get-Item $stamp).LastWriteTimeUtc
        foreach ($file in $packageFiles) {
            if ((Get-Item $file).LastWriteTimeUtc -gt $stampTime) {
                $needsInstall = $true
                break
            }
        }
    }

    if (-not $needsInstall) {
        Write-Host 'npm dependencies are up to date.'
        return
    }

    if (Test-Path (Join-Path $WebDir 'package-lock.json')) {
        Invoke-External 'npm' @('--prefix', $WebDir, 'ci')
    } else {
        Invoke-External 'npm' @('--prefix', $WebDir, 'install')
    }

    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $stamp) | Out-Null
    Set-Content -Path $stamp -Value (Get-Date -Format o) -Encoding ASCII
}

function Build-Web {
    Ensure-WebDependencies
    Invoke-External 'npm' @('--prefix', $WebDir, 'run', 'build')
}

function Build-Pack {
    Build-Web

    $indexHtml = Join-Path $DistDir 'index.html'
    if (-not (Test-Path $indexHtml)) {
        throw "Web build did not produce '$indexHtml'."
    }

    $python = Get-PythonCommand
    Invoke-External $python @(
        (Join-Path $Root 'tools\pack_assets.py'),
        $DistDir,
        $PackedC,
        '--mount',
        '/web'
    )
}

function Get-Compiler {
    $parts = if ($env:CC) { @(Split-CommandLine $env:CC) } else { @('gcc') }
    if ($parts.Count -eq 0) {
        throw 'CC is empty.'
    }

    Require-Command $parts[0]

    return [pscustomobject]@{
        Command = $parts[0]
        PrefixArgs = @($parts | Select-Object -Skip 1)
    }
}

function Build-App {
    Build-Pack

    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Output) | Out-Null

    $compiler = Get-Compiler
    $defaultCFlags = @(
        '-std=gnu11',
        '-O2',
        '-D_GNU_SOURCE',
        '-Ithird_party/mongoose',
        '-DMG_ENABLE_DIRLIST=0'
    )

    $cflags = @(Get-EnvArgs $env:CFLAGS $defaultCFlags)
    $ldflags = @(Get-EnvArgs $env:LDFLAGS)
    $pthreadFlags = @(Get-EnvArgs $env:PTHREAD_FLAGS @('-pthread'))
    $curlCFlags = @(Get-EnvArgs $env:CURL_CFLAGS (Get-FlagsFromTool 'curl-config' @('--cflags')))
    $curlLibs = @(Get-EnvArgs $env:CURL_LIBS (Get-FlagsFromTool 'curl-config' @('--libs') @('-lcurl')))
    $sqliteCFlags = @(Get-EnvArgs $env:SQLITE_CFLAGS (Get-FlagsFromTool 'pkg-config' @('--cflags', 'sqlite3')))
    $sqliteLibs = @(Get-EnvArgs $env:SQLITE_LIBS (Get-FlagsFromTool 'pkg-config' @('--libs', 'sqlite3') @('-lsqlite3')))

    $windowsLibs = @()
    if ($IsWindowsHost) {
        $windowsLibs = @('-lws2_32', '-liphlpapi')
    }

    $appSources = Get-ChildItem -Path (Join-Path $Root 'src') -Recurse -Filter '*.c' |
        Sort-Object FullName |
        ForEach-Object { $_.FullName }

    $arguments = @()
    $arguments += $compiler.PrefixArgs
    $arguments += $cflags
    $arguments += $pthreadFlags
    $arguments += $curlCFlags
    $arguments += $sqliteCFlags
    $arguments += '-Isrc'
    $arguments += '-o'
    $arguments += $Output
    $arguments += $appSources
    $arguments += (Join-Path $Root 'third_party\mongoose\mongoose.c')
    $arguments += $PackedC
    $arguments += $ldflags
    $arguments += $curlLibs
    $arguments += $sqliteLibs
    $arguments += $pthreadFlags
    $arguments += $windowsLibs

    Invoke-External $compiler.Command $arguments
    Write-Host "Built $Output"
}

function Clean-Build {
    foreach ($path in @($BuildDir, $DistDir)) {
        if (Test-Path $path) {
            Write-Host "Removing $path"
            Remove-Item -LiteralPath $path -Recurse -Force
        }
    }
}

function New-AdminPassword {
    $bytes = New-Object byte[] 30
    $rng = [System.Security.Cryptography.RandomNumberGenerator]::Create()
    try {
        $rng.GetBytes($bytes)
        return [Convert]::ToBase64String($bytes)
    } finally {
        $rng.Dispose()
    }
}

function Run-App {
    Build-App

    if ([string]::IsNullOrWhiteSpace($AdminPassword)) {
        $AdminPassword = New-AdminPassword
    }

    Write-Host ''
    Write-Host "Development login user: $AdminUser"
    Write-Host "Development login password: $AdminPassword"
    Write-Host ''

    $oldUser = $env:APP_ADMIN_USER
    $oldPassword = $env:APP_ADMIN_PASSWORD
    $oldReset = $env:APP_RESET_ADMIN_PASSWORD

    try {
        $env:APP_ADMIN_USER = $AdminUser
        $env:APP_ADMIN_PASSWORD = $AdminPassword
        $env:APP_RESET_ADMIN_PASSWORD = '1'
        Invoke-External $Output @()
    } finally {
        $env:APP_ADMIN_USER = $oldUser
        $env:APP_ADMIN_PASSWORD = $oldPassword
        $env:APP_RESET_ADMIN_PASSWORD = $oldReset
    }
}

switch ($Target) {
    'build' { Build-App }
    'all' { Build-App }
    'app' { Build-App }
    'web' { Build-Web }
    'pack' { Build-Pack }
    'clean' { Clean-Build }
    'rebuild' {
        Clean-Build
        Build-App
    }
    'run' { Run-App }
}
