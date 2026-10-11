# Dual Input mod for Halo: The Master Chief Collection - installer / uninstaller.
# Run through install.bat or uninstall.bat.
param(
    [switch]$Uninstall,
    [string]$GameDir
)

$ErrorActionPreference = 'Stop'
$AppId     = '976730'
$GameFolder = 'Halo The Master Chief Collection'
$TargetSub = 'MCC\Binaries\Win64'
$ExeName   = 'MCC-Win64-Shipping.exe'
$ModFiles  = @('xinput1_3.dll')
$IniName   = 'xinput_mouse.ini'
$LogNames  = @('mixed_input.log', 'mixed_input.old.log')
$BackupDir = 'DualInput_backup'
# SHA-256 of the Steam build the mod was made and tested on (2025.08.16.178512).
$KnownExe  = 'BE70D6DCD1A884F10CEB342A7A2DCB35EE0FA43181B66A1D19C3D830E9834691'
$ModDir    = Join-Path $PSScriptRoot 'mod'

# Returns the Win64 folder (the one with MCC-Win64-Shipping.exe), or $null.
function Resolve-Target($dir) {
    if (-not $dir) { return $null }
    foreach ($d in $dir, (Join-Path $dir $TargetSub)) {
        if (Test-Path (Join-Path $d $ExeName)) { return $d }
    }
    return $null
}

function Find-GameDir {
    $steam = $null
    foreach ($key in 'HKCU:\Software\Valve\Steam', 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam') {
        try {
            $p = Get-ItemProperty $key -ErrorAction Stop
            $steam = if ($p.SteamPath) { $p.SteamPath } else { $p.InstallPath }
            if ($steam) { break }
        } catch {}
    }
    if (-not $steam) { return $null }

    $libs = @($steam)
    $vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
    if (Test-Path $vdf) {
        foreach ($m in [regex]::Matches((Get-Content $vdf -Raw), '"path"\s+"([^"]+)"')) {
            $libs += $m.Groups[1].Value -replace '\\\\', '\'
        }
    }
    foreach ($lib in $libs | Select-Object -Unique) {
        $apps = Join-Path $lib 'steamapps'
        # Prefer the folder named in the game's Steam manifest, fall back to the default name.
        $names = @()
        $acf = Join-Path $apps "appmanifest_$AppId.acf"
        if (Test-Path $acf) {
            $m = [regex]::Match((Get-Content $acf -Raw), '"installdir"\s+"([^"]+)"')
            if ($m.Success) { $names += $m.Groups[1].Value }
        }
        $names += $GameFolder
        foreach ($n in $names | Select-Object -Unique) {
            $t = Resolve-Target (Join-Path $apps "common\$n")
            if ($t) { return $t }
        }
    }
    return $null
}

Write-Host ''
Write-Host '=== Dual Input mod for Halo: The Master Chief Collection ===' -ForegroundColor Cyan
Write-Host ''

$GameDir = Resolve-Target $GameDir
if (-not $GameDir) { $GameDir = Find-GameDir }
while (-not $GameDir) {
    Write-Host 'The game was not found automatically (or the folder is wrong).' -ForegroundColor Yellow
    $in = (Read-Host "Paste the game folder (the one with $GameFolder, or MCC\Binaries\Win64), or leave empty to cancel").Trim('" ')
    if (-not $in) { Write-Host 'Cancelled.'; exit 1 }
    $GameDir = Resolve-Target $in
    if (-not $GameDir) { Write-Host "$ExeName not found in: $in" -ForegroundColor Yellow }
}
Write-Host "Install folder: $GameDir"

$backup = Join-Path $GameDir $BackupDir

if ($Uninstall) {
    foreach ($f in $ModFiles + $LogNames) {
        $path = Join-Path $GameDir $f
        if (Test-Path $path) { Remove-Item $path -Force; Write-Host "  removed  $f" }
    }
    if (Test-Path $backup) {
        Get-ChildItem $backup | ForEach-Object {
            Move-Item $_.FullName (Join-Path $GameDir $_.Name) -Force
            Write-Host "  restored $($_.Name) (from before the mod)"
        }
        Remove-Item $backup -Force -Recurse
    }
    $ini = Join-Path $GameDir $IniName
    if (Test-Path $ini) {
        $a = Read-Host "Delete your settings file $IniName too? (y/N)"
        if ($a -match '^[yY]') { Remove-Item $ini -Force; Write-Host "  removed  $IniName" }
        else { Write-Host "  kept     $IniName" }
    }
    Write-Host ''
    Write-Host 'Mod uninstalled.' -ForegroundColor Green
    exit 0
}

$hash = (Get-FileHash (Join-Path $GameDir $ExeName) -Algorithm SHA256).Hash
if ($hash -ne $KnownExe) {
    Write-Host ''
    Write-Host "WARNING: your $ExeName is a different version than the one the mod" -ForegroundColor Yellow
    Write-Host 'was tested on (Steam, build 2025.08.16.178512). The mod checks the game code' -ForegroundColor Yellow
    Write-Host 'before hooking it and turns itself off if it does not match, so the game' -ForegroundColor Yellow
    Write-Host 'then just behaves as without the mod (gyro will not mix with the gamepad).' -ForegroundColor Yellow
    $a = Read-Host 'Install anyway? (y/N)'
    if ($a -notmatch '^[yY]') { Write-Host 'Cancelled.'; exit 1 }
}

foreach ($f in $ModFiles) {
    $src = Join-Path $ModDir $f
    $dst = Join-Path $GameDir $f
    if ((Test-Path $dst) -and (Get-FileHash $dst).Hash -ne (Get-FileHash $src).Hash) {
        # Another mod (or an older copy) uses the same file name: keep it, unless it is
        # an earlier version of this mod (it then reads xinput_mouse.ini).
        $isOurs = Select-String -Path $dst -Pattern $IniName -SimpleMatch -Quiet
        if (-not $isOurs -and -not (Test-Path (Join-Path $backup $f))) {
            New-Item -ItemType Directory -Path $backup -Force | Out-Null
            Copy-Item $dst (Join-Path $backup $f)
            Write-Host "  backed up existing $f to $BackupDir\" -ForegroundColor Yellow
        }
    }
    Copy-Item $src $dst -Force
    Write-Host "  installed $f"
}

$ini = Join-Path $GameDir $IniName
if (Test-Path $ini) {
    Write-Host "  kept your existing $IniName (settings unchanged)"
} else {
    Copy-Item (Join-Path $ModDir $IniName) $ini
    Write-Host "  installed $IniName"
}

Write-Host ''
Write-Host 'Done. Next steps:' -ForegroundColor Green
Write-Host '  1. Steam Input: gyro -> "As Mouse", right trackpad -> "Joystick" (see README).'
Write-Host '  2. Start MCC WITHOUT Easy Anti-Cheat (Steam launch option "Play without Anti-Cheat").'
Write-Host "     Settings: $ini"
Write-Host '  3. To remove the mod, run uninstall.bat.'
