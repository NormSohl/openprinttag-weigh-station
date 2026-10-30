# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Norm Sohl
#
# Daily off-station backup of the weigh station, for the staff PC.
# install-backup.ps1 copies this into the backup folder and schedules it;
# see README.md. It can also be run by hand at any time.
#
# Saves, under dated names:
#   events-YYYY-MM-DD.ndjson   the event log (/export): every spool, weigh,
#                              product and edit -- what Backup -> Download saves
#   config-YYYY-MM-DD.json     the Settings tables (/config/export)
# Both are open reads: they need no API key and don't interrupt weighing.
#
# Keeps $KeepDays days of copies. A failure leaves "BACKUP FAILED.txt" in the
# folder, where someone will see it; the next good run removes it. Every run
# adds a line to backup-log.txt.
#
# Windows PowerShell 5.1 (built into Windows 10/11); no modules needed.
param(
    [string]$Station,                       # IP address; default: station.txt beside this script
    [string]$Dest = $PSScriptRoot,          # where backups go: this script's folder
    [int]$KeepDays = 90
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'   # 5.1's progress bar slows downloads to a crawl
$log    = Join-Path $Dest 'backup-log.txt'
$failed = Join-Path $Dest 'BACKUP FAILED.txt'
$stamp  = Get-Date -Format 'yyyy-MM-dd'

function Write-Log([string]$msg) {
    Add-Content -Path $log -Value ("{0}  {1}" -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $msg)
}

# Fetch one URL into a temp file, check it looks like what the station sends,
# then move it into place -- so a half-finished download never sits under a
# real backup's name.
function Save-Url([string]$url, [string]$file, [scriptblock]$looksRight) {
    $tmp = "$file.part"
    Invoke-WebRequest -Uri $url -OutFile $tmp -UseBasicParsing -TimeoutSec 120
    $len = (Get-Item $tmp).Length
    if ($len -eq 0) { throw "$url returned nothing" }
    $head = Get-Content -Path $tmp -TotalCount 1 -Encoding UTF8
    if (-not (& $looksRight $head)) { throw "$url did not return a backup (starts: $($head.Substring(0, [Math]::Min(60, $head.Length))))" }
    Move-Item -Path $tmp -Destination $file -Force
    return $len
}

try {
    if (-not $Station) {
        $cfg = Join-Path $PSScriptRoot 'station.txt'
        if (-not (Test-Path $cfg)) { throw "no station address: run install-backup.ps1, or pass -Station" }
        $Station = (Get-Content $cfg -TotalCount 1).Trim()
    }
    New-Item -ItemType Directory -Force -Path $Dest | Out-Null
    $base = "http://$Station"

    $ev  = Save-Url "$base/export"        (Join-Path $Dest "events-$stamp.ndjson") { param($l) $l.TrimStart().StartsWith('{') }
    $cf  = Save-Url "$base/config/export" (Join-Path $Dest "config-$stamp.json")   { param($l) $l.TrimStart().StartsWith('{') }

    # Prune old copies -- only files this script made.
    $cutoff = (Get-Date).AddDays(-$KeepDays)
    $old = Get-ChildItem -Path $Dest -File |
           Where-Object { $_.Name -match '^(events-\d{4}-\d{2}-\d{2}\.ndjson|config-\d{4}-\d{2}-\d{2}\.json)$' -and
                          $_.LastWriteTime -lt $cutoff }
    $old | Remove-Item -Force

    if (Test-Path $failed) { Remove-Item $failed -Force }
    Write-Log ("ok  events {0:N0} bytes, config {1:N0} bytes, removed {2} old file(s)" -f $ev, $cf, @($old).Count)
}
catch {
    Get-ChildItem -Path $Dest -Filter '*.part' -ErrorAction SilentlyContinue | Remove-Item -Force
    $msg = "Weigh station backup FAILED on $(Get-Date -Format 'yyyy-MM-dd HH:mm').`r`n`r`n" +
           "$($_.Exception.Message)`r`n`r`n" +
           "Check that the station is on and reachable at http://$Station/ from this PC.`r`n" +
           "This file goes away by itself after the next backup that works."
    Set-Content -Path $failed -Value $msg
    Write-Log "FAILED  $($_.Exception.Message)"
    exit 1
}
