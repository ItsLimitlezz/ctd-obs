[CmdletBinding()]
param(
    [ValidateSet('x64', 'arm64')]
    [string] $Target = 'x64',
    [ValidateSet('Debug', 'RelWithDebInfo', 'Release', 'MinSizeRel')]
    [string] $Configuration = 'RelWithDebInfo'
)

$ErrorActionPreference = 'Stop'

if ( $DebugPreference -eq 'Continue' ) {
    $VerbosePreference = 'Continue'
    $InformationPreference = 'Continue'
}

if ( $env:CI -eq $null ) {
    throw "Package-Windows.ps1 requires CI environment"
}

if ( ! ( [System.Environment]::Is64BitOperatingSystem ) ) {
    throw "obs-studio requires a 64-bit system to build and run."
}

if ( $PSVersionTable.PSVersion -lt '7.2.0' ) {
    Write-Warning 'The obs-studio packaging script requires PowerShell Core 7. Install or upgrade your PowerShell version: https://aka.ms/pscore6'
    exit 2
}

function Package {
    trap {
        Write-Error $_
        exit 2
    }

    $ScriptHome = $PSScriptRoot
    $ProjectRoot = Resolve-Path -Path "$PSScriptRoot/../.."

    $UtilityFunctions = Get-ChildItem -Path $PSScriptRoot/utils.pwsh/*.ps1 -Recurse

    foreach( $Utility in $UtilityFunctions ) {
        Write-Debug "Loading $($Utility.FullName)"
        . $Utility.FullName
    }

    Install-BuildDependencies -WingetFile "${ScriptHome}/.Wingetfile"

    $GitDescription = Invoke-External git describe --tags --long
    $Tokens = ($GitDescription -split '-')
    $CommitVersion = $Tokens[0..$($Tokens.Count - 3)] -join '-'
    $CommitHash = $($Tokens[-1]).SubString(1)
    $CommitDistance = $Tokens[-2]

    if ( $CommitDistance -gt 0 ) {
        $OutputName = "ctd-obs-${CommitVersion}-${CommitHash}"
    } else {
        $OutputName = "ctd-obs-${CommitVersion}"
    }

    $CpackArgs = @(
        '-C', "${Configuration}"
    )

    if ( $DebugPreference -eq 'Continue' ) {
        $CpackArgs += ('--verbose')
    }

    Log-Group "Packaging ctd-obs..."

    Push-Location -Stack PackageTemp "build_${Target}"

    cpack @CpackArgs

    $Package = Get-ChildItem -filter "ctd-obs-*-windows-${Target}.zip" -File
    Move-Item -Path $Package -Destination "${OutputName}-windows-${Target}.zip"

    Pop-Location -Stack PackageTemp

    if ( $Target -eq 'x64' ) {
        $InstallerArgs = @{
            ZipFile = "${ProjectRoot}/build_${Target}/${OutputName}-windows-${Target}.zip"
            Version = $OutputName -replace '^ctd-obs-', ''
            CommitVersion = $CommitVersion
            CommitDistance = $CommitDistance
        }
        New-Installer @InstallerArgs
    }
}

function Find-MakeNsis {
    $Command = Get-Command -ErrorAction SilentlyContinue makensis
    if ( $Command ) { return $Command.Source }

    $Candidates = @(
        "${env:ProgramFiles(x86)}\NSIS\makensis.exe",
        "${env:ProgramFiles}\NSIS\makensis.exe"
    )
    foreach ( $Candidate in $Candidates ) {
        if ( Test-Path $Candidate ) { return $Candidate }
    }

    Log-Information 'NSIS not found, installing it with Chocolatey'
    # Out-Host: keep choco output out of this function's return value
    Invoke-External choco install nsis -y --no-progress | Out-Host
    foreach ( $Candidate in $Candidates ) {
        if ( Test-Path $Candidate ) { return $Candidate }
    }

    throw 'makensis not found after installing NSIS'
}

function New-Installer {
    <#
        .SYNOPSIS
            Builds the CTD OBS Windows installer (CTD-OBS-Setup-<version>-x64.exe) from the zip package.
    #>
    param(
        [string] $ZipFile,
        [string] $Version,
        [string] $CommitVersion,
        [int] $CommitDistance
    )

    Log-Group "Building CTD OBS installer..."

    $BuildDir = Split-Path -Parent $ZipFile
    $WorkDir = "${BuildDir}/installer"
    $StagingDir = "${WorkDir}/staging"
    $NsisScript = "${ProjectRoot}/cmake/windows/installer/ctd-obs.nsi"
    $OutputFile = "${BuildDir}/CTD-OBS-Setup-${Version}-x64.exe"

    if ( Test-Path $WorkDir ) { Remove-Item -Recurse -Force $WorkDir }
    New-Item -ItemType Directory -Force -Path $StagingDir | Out-Null

    # Install exactly what goes into the zip.
    Expand-Archive -Path $ZipFile -DestinationPath $StagingDir

    if ( ! ( Test-Path "${StagingDir}/bin/64bit/ctd-obs64.exe" ) ) {
        throw "ctd-obs64.exe not found in the package at bin/64bit"
    }

    $VirtualCamDll = Get-ChildItem -Path $StagingDir -Recurse -File -Filter 'ctd-obs-virtualcam-module64.dll' | Select-Object -First 1
    if ( ! $VirtualCamDll ) {
        throw "ctd-obs-virtualcam-module64.dll not found in the package"
    }
    $VirtualCamDir = [System.IO.Path]::GetRelativePath((Resolve-Path $StagingDir).Path, $VirtualCamDll.DirectoryName)
    Log-Information "Virtual camera DLLs in ${VirtualCamDir}"

    # Top-level items the uninstaller (and an upgrade) removes; nothing else in the folder is touched.
    $ItemsFile = "${WorkDir}/installed-items.nsh"
    $ItemLines = Get-ChildItem -Path $StagingDir | ForEach-Object {
        if ( $_.PSIsContainer ) {
            'RMDir /r "$INSTDIR\' + $_.Name + '"'
        } else {
            'Delete "$INSTDIR\' + $_.Name + '"'
        }
    }
    Set-Content -Path $ItemsFile -Value $ItemLines -Encoding utf8

    # Microsoft Visual C++ runtime (x64), installed by the setup only when missing or older.
    $VcRedist = "${WorkDir}/vc_redist.x64.exe"
    Invoke-WebRequest -Uri 'https://aka.ms/vc14/vc_redist.x64.exe' -OutFile $VcRedist -UseBasicParsing
    $VcRedistVersion = (Get-Item $VcRedist).VersionInfo.FileVersion
    if ( ! ( $VcRedistVersion -match '^14\.(\d+)\.' ) ) {
        throw "Unexpected Visual C++ runtime version '${VcRedistVersion}'"
    }
    $VcRedistMinor = $Matches[1]
    Log-Information "Bundling Visual C++ runtime ${VcRedistVersion}"

    if ( $CommitVersion -match '^(\d+)\.(\d+)\.(\d+)' ) {
        $NumericVersion = "$($Matches[1]).$($Matches[2]).$($Matches[3]).$([Math]::Min($CommitDistance, 65535))"
    } else {
        $NumericVersion = "0.0.0.$([Math]::Min($CommitDistance, 65535))"
    }

    $NsisArgs = @(
        '/V3', '/INPUTCHARSET', 'UTF8',
        "/DSTAGING_DIR=$((Resolve-Path $StagingDir).Path)",
        "/DOUTPUT_FILE=${OutputFile}",
        "/DVERSION=${Version}",
        "/DVERSION_NUMERIC=${NumericVersion}",
        "/DVCAM_DIR=${VirtualCamDir}",
        "/DVCREDIST_FILE=$((Resolve-Path $VcRedist).Path)",
        "/DVCREDIST_MINOR=${VcRedistMinor}",
        "/DITEMS_FILE=$((Resolve-Path $ItemsFile).Path)",
        "/DICON_FILE=${ProjectRoot}\frontend\cmake\windows\obs-studio.ico",
        "/DLICENSE_FILE=${ProjectRoot}\frontend\data\license\gplv2.txt"
    )

    # ------------------------------------------------------------------------------------------
    # SIGNING HOOK (not active: no certificate yet)
    # When code signing is available:
    #   1. Sign the .exe and .dll files in $StagingDir here, before makensis packs them.
    #   2. Set CTD_SIGN_CMD to a command that signs the file passed as %1, e.g.
    #      signtool sign /fd sha256 /tr <timestamp server> /td sha256 /f <cert> "%1"
    #      makensis then signs the installer and the uninstaller inside it as they are written.
    # Never commit certificates, passwords or server addresses; pass them as secrets.
    if ( $env:CTD_SIGN_CMD ) {
        $NsisArgs += "/DSIGN_CMD=${env:CTD_SIGN_CMD}"
    }
    # ------------------------------------------------------------------------------------------

    $MakeNsis = Find-MakeNsis
    Log-Information "Running ${MakeNsis}"
    & $MakeNsis @NsisArgs $NsisScript
    if ( $LASTEXITCODE -ne 0 ) {
        throw "makensis failed with exit code ${LASTEXITCODE}"
    }

    Log-Information "Built $(Split-Path -Leaf $OutputFile)"
    Log-Group
}

Package
