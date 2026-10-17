<#
  serial-capture.ps1 -- dump a WS63 serial port to stdout AND to a file (ASCII ONLY, PS 5.1 safe)

  WHY THIS EXISTS (do not replace it with watch.ps1 for evidence collection)
    1. watch.ps1 prints with Write-Host, which in PS 5.1 goes to the Information
       stream -- `powershell -File watch.ps1 > log.txt` therefore produces an
       EMPTY file, and the raw log is exactly what the work order wants as
       evidence. This script writes through [Console]::Out and a StreamWriter,
       both of which redirect normally.
    2. The board's RTS line drives the module EN pin (active LOW). Anything that
       asserts RTS on open holds the board in reset and you "see a dead board".
       DTR and RTS are forced LOW here before Open(), same as watch.ps1.
    3. Lines are flushed to the file as they arrive (AutoFlush), so a killed run
       still leaves the whole capture on disk.

  USAGE (from Windows PowerShell or from WSL via powershell.exe)
    powershell -NoProfile -ExecutionPolicy Bypass -File tools\serial-capture.ps1 `
      -Port COM14 -Seconds 70 -Out ws63-firmware\spike\out\master.log

  OUTPUT FORMAT
    <epoch_ms>|<text>          -- one line per received line; epoch lets you
    measure the real inter-frame interval even if the port delivers bursty data.
    A trailing header line `# EOF port=.. seconds=.. lines=..` closes the file.
#>
param(
  [string]$Port = "COM14",
  [int]$Baud = 115200,
  [int]$Seconds = 60,
  [string]$Out = "",
  [switch]$Quiet,         # do not echo to stdout (file only)
  [switch]$Reset,         # pulse RTS to reboot the board first (see below)
  [int]$ResetHoldMs = 300
)

$ErrorActionPreference = "Continue"
[Console]::OutputEncoding = [System.Text.Encoding]::ASCII

$writer = $null
if ($Out -ne "") {
  $dir = Split-Path -Parent $Out
  if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
  $writer = New-Object System.IO.StreamWriter($Out, $false, [System.Text.Encoding]::ASCII)
  $writer.AutoFlush = $true
}

function Emit([string]$text) {
  $line = "{0}|{1}" -f ([DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()), $text
  if ($writer) { $writer.WriteLine($line) }
  if (-not $Quiet) { [Console]::Out.WriteLine($line) }
}

# ---------------------------------------------------------------- open port
try {
  $p = New-Object System.IO.Ports.SerialPort $Port, $Baud, "None", 8, "One"
  $p.ReadTimeout = 200
  $p.WriteTimeout = 1000
  # CRITICAL: never assert RTS/DTR, or the board is held in reset (EN is active low).
  $p.DtrEnable = $false
  $p.RtsEnable = $false
  $p.Open()
} catch {
  $msg = "OPEN-FAIL $Port : $($_.Exception.Message)"
  if (-not $Quiet) { [Console]::Out.WriteLine($msg) }
  if ($writer) { $writer.WriteLine("# $msg"); $writer.Close() }
  exit 1
}

# ------------------------------------------------------- optional reboot
# RTS drives the module EN pin (active LOW): RTS high holds the board in reset,
# RTS low lets it run. Two boards started from ONE capture call with -Reset
# therefore boot within milliseconds of each other, which is what a two-board
# smoke test needs (a board that boots alone may spend its first scan on a peer
# that is not up yet, or may still be wedged on a connection that the other
# board's reflash just killed).
if ($Reset) {
  $p.RtsEnable = $true
  if (-not $Quiet) { [Console]::Out.WriteLine("# RESET: RTS asserted (EN low) for $ResetHoldMs ms") }
  Start-Sleep -Milliseconds $ResetHoldMs
  $p.RtsEnable = $false
  if (-not $Quiet) { [Console]::Out.WriteLine("# RESET: RTS released (board boots now)") }
}

$started = Get-Date
if (-not $Quiet) { [Console]::Out.WriteLine("# OPEN ok $Port @ $Baud 8N1 seconds=$Seconds rts=low reset=$Reset") }

$buf = New-Object System.Text.StringBuilder
$sw = [System.Diagnostics.Stopwatch]::StartNew()
$n = 0
try {
  while ($sw.Elapsed.TotalSeconds -lt $Seconds) {
    $chunk = ""
    try { $chunk = $p.ReadExisting() } catch { $chunk = "" }
    if ($chunk.Length -gt 0) {
      [void]$buf.Append($chunk)
      while ($true) {
        $s = $buf.ToString()
        $i = $s.IndexOf("`n")
        if ($i -lt 0) { break }
        $line = $s.Substring(0, $i).TrimEnd("`r")
        [void]$buf.Remove(0, $i + 1)
        if ($line.Length -gt 0) { Emit $line; $n++ }
      }
      if ($buf.Length -gt 1024) { Emit $buf.ToString(); [void]$buf.Clear(); $n++ }
    }
    Start-Sleep -Milliseconds 25
  }
} finally {
  if ($buf.Length -gt 0) { Emit $buf.ToString(); $n++ }
  $p.Close()
  $elapsed = [int]((Get-Date) - $started).TotalSeconds
  $tail = "# EOF port={0} seconds={1} lines={2}" -f $Port, $elapsed, $n
  if ($writer) { $writer.WriteLine($tail); $writer.Close() }
  if (-not $Quiet) { [Console]::Out.WriteLine($tail) }
}
