<#
.SYNOPSIS
  Download Apple's Developer Disk Image, which the rPlayHub engine mounts on the phone after
  every reboot. The Windows sibling of scripts/fetch-ddi.sh -- same source, same files.

.DESCRIPTION
  iOS 17+ drops the DDI on reboot, and the CoreDevice services rPlayHub uses stay quiet until one
  is mounted again. The engine mounts it by itself (host-c/ddi.c): it asks Apple's signing server
  for a ticket bound to your phone, then uploads the image. What it needs from you is the image
  files -- Apple's software, which rPlayHub cannot redistribute. This fetches them from
  doronz88/DeveloperDiskImage, the public mirror pymobiledevice3 downloads from, into the folder
  the engine looks in:

    powershell -ExecutionPolicy Bypass -File fetch-ddi.ps1           # -> %LOCALAPPDATA%\rplayhub\iOS_DDI
    powershell -ExecutionPolicy Bypass -File fetch-ddi.ps1 D:\ddi    # -> D:\ddi; run the engine with RPLAY_DDI=D:\ddi

  The DDI_REF environment variable (or -Ref) picks a branch, tag or commit of that repository
  (default: main).

.PARAMETER Dest
  Destination iOS_DDI folder. Default: %LOCALAPPDATA%\rplayhub\iOS_DDI.

.PARAMETER Ref
  Branch, tag or commit of doronz88/DeveloperDiskImage. Default: $env:DDI_REF, else main.
#>
param(
    [string]$Dest = "",
    [string]$Ref = ""
)
$ErrorActionPreference = "Stop"
# Windows PowerShell 5.1 defaults to TLS 1.0/1.1, which GitHub refuses.
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
# The progress bar makes Invoke-WebRequest many times slower on 5.1.
$ProgressPreference = "SilentlyContinue"

$defaultDest = Join-Path $env:LOCALAPPDATA "rplayhub\iOS_DDI"
if (-not $Dest) { $Dest = $defaultDest }
if (-not $Ref) { $Ref = if ($env:DDI_REF) { $env:DDI_REF } else { "main" } }
$base = "https://github.com/doronz88/DeveloperDiskImage/raw/$Ref/PersonalizedImages/Xcode_iOS_DDI_Personalized"

# The engine reads <dir>\Restore\BuildManifest.plist and takes the image and trust-cache paths
# from the manifest, which in this copy are Image.dmg and Image.dmg.trustcache.
$restore = Join-Path $Dest "Restore"
New-Item -ItemType Directory -Force -Path $restore | Out-Null
foreach ($f in @("BuildManifest.plist", "Image.dmg", "Image.dmg.trustcache")) {
    Write-Host "fetching $f"
    $out = Join-Path $restore $f
    $part = "$out.part"
    $ok = $false
    for ($try = 1; $try -le 3 -and -not $ok; $try++) {
        try {
            Invoke-WebRequest -Uri "$base/$f" -OutFile $part -UseBasicParsing
            $ok = $true
        } catch {
            if ($try -eq 3) { Remove-Item -Force -ErrorAction SilentlyContinue $part; throw "could not download ${f}: $($_.Exception.Message)" }
            Write-Host "  retrying ($($_.Exception.Message))"
            Start-Sleep -Seconds 2
        }
    }
    Move-Item -Force $part $out
}

# ProductBuildVersion from the manifest, for the summary line (a plain-text plist; best effort).
$build = ""
try {
    $m = [regex]::Match((Get-Content -Raw (Join-Path $restore "BuildManifest.plist")),
                        "<key>ProductBuildVersion</key>\s*<string>([^<]*)</string>")
    if ($m.Success) { $build = $m.Groups[1].Value }
} catch { }

Write-Host ""
Write-Host ("DDI " + $(if ($build) { "build $build " } else { "" }) + "in $Dest")
if ((Resolve-Path $Dest).Path.TrimEnd('\') -ieq $defaultDest.TrimEnd('\')) {
    Write-Host "the engine finds it there on its own."
} else {
    Write-Host "run the engine with RPLAY_DDI=$Dest"
}
Write-Host "keep the phone unlocked when the engine starts: the phone refuses the mount while locked."
