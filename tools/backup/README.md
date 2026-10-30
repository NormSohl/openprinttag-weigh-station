# tools/backup/ — daily backup on the staff PC

The station keeps its log on its own flash, and compacts it as it fills
(old weigh readings fold into monthly totals; old product edits are dropped).
Copies kept off the station are the only protection against losing it, or
the station, outright. These two scripts make that automatic on a Windows PC
that's on the same network as the station.

## Setup (once)

1. Copy this folder to the staff PC (or just the two `.ps1` files).
2. In that folder, open PowerShell and run:

   ```powershell
   powershell -ExecutionPolicy Bypass -File install-backup.ps1
   ```

3. Type the station's fixed IP address when asked.

It creates `C:\Inventory Backups`, copies `weighstation-backup.ps1` there,
saves the address in `station.txt`, runs one backup, and adds a Task
Scheduler task, **Weigh station backup**: daily at midnight. If the PC was off
at midnight, it runs as soon as it's next on. It runs while that user is logged on,
so no password is stored and no administrator rights are needed.

## What you'll find in `C:\Inventory Backups`

| File | What it is |
|---|---|
| `events-YYYY-MM-DD.ndjson` | The event log: what **Backup → Download** saves. **Restore** on the Backup page loads it back. |
| `config-YYYY-MM-DD.json` | The Settings tables. Restored separately on the Backup page. |
| `backup-log.txt` | One line per run. |
| `BACKUP FAILED.txt` | Only when the last run failed, with the reason. Goes away after the next good run. |

Copies older than 90 days are deleted (`-KeepDays` to change). Each day is
about the size of the log, so 90 days of a 1 MB log is about 90 MB.

**This folder is on the PC only.** If the PC's disk fails, the backups go
with it. Copy one to a USB stick or cloud storage now and then, or point
`-Dest` at a synced folder when installing.

## Options

```powershell
install-backup.ps1 -Station 192.168.1.42 -Dest 'C:\Inventory Backups' -At 00:00
weighstation-backup.ps1 -KeepDays 180        # run by hand, keep half a year
```

Running `install-backup.ps1` again updates the address and the schedule.
To remove it: delete the **Weigh station backup** task in Task Scheduler.
