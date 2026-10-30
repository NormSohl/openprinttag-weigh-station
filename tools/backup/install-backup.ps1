# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Norm Sohl
#
# One-time setup of the daily weigh station backup on the staff PC. See
# README.md. Run it from this folder, as the user who is normally logged in:
#
#   powershell -ExecutionPolicy Bypass -File install-backup.ps1
#
# It asks for the station's IP address, copies weighstation-backup.ps1 into
# the backup folder, runs one backup to prove it works, and schedules it
# daily. Running it again updates the address and the schedule.
param(
    [string]$Dest = 'C:\Inventory Backups',
    [string]$Station,
    [string]$At = '12:00'      # daily; a PC that was off then catches up when it's next on
)
$ErrorActionPreference = 'Stop'

if (-not $Station) { $Station = (Read-Host "Weigh station IP address (e.g. 192.168.1.42)").Trim() }
if (-not $Station) { throw "no address given" }

New-Item -ItemType Directory -Force -Path $Dest | Out-Null
Copy-Item -Path (Join-Path $PSScriptRoot 'weighstation-backup.ps1') -Destination $Dest -Force
Set-Content -Path (Join-Path $Dest 'station.txt') -Value $Station
$script = Join-Path $Dest 'weighstation-backup.ps1'

Write-Host "Running a first backup from http://$Station/ ..."
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $script
if ($LASTEXITCODE -ne 0) {
    Write-Warning "The first backup failed; see '$Dest\BACKUP FAILED.txt'. Scheduling it anyway."
} else {
    Write-Host "OK -- see '$Dest'."
}

# Daily at $At. StartWhenAvailable runs a missed time as soon as the PC is
# next on, so a PC that is off at noon still backs up that day. Runs as this
# user, only while logged on, so no password is stored and no administrator
# rights are needed; the window is hidden.
$action  = New-ScheduledTaskAction -Execute 'powershell.exe' `
             -Argument "-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File `"$script`""
$trigger = New-ScheduledTaskTrigger -Daily -At $At
$settings = New-ScheduledTaskSettingsSet -StartWhenAvailable -DontStopIfGoingOnBatteries `
              -AllowStartIfOnBatteries -ExecutionTimeLimit (New-TimeSpan -Minutes 10)
Register-ScheduledTask -TaskName 'Weigh station backup' -Action $action -Trigger $trigger `
    -Settings $settings -Description 'Daily backup of the filament weigh station (event log + Settings).' `
    -Force | Out-Null
Write-Host "Scheduled 'Weigh station backup': daily at $At, or as soon as the PC is next on. (Task Scheduler shows it.)"
