# Captures the existing controller debug stream and HTTP status without
# transmitting bus commands. Compatible with Windows PowerShell 5.1.
[CmdletBinding()]
param(
    [string]$Controller,
    [string]$ObservedStatus = 'Unknown',
    [ValidateRange(30, 3600)]
    [int]$Seconds = 900,
    [ValidateRange(1, 65535)]
    [int]$DebugPort = 7373,
    [string]$OutputDirectory
)

$ErrorActionPreference = 'Stop'
if (-not $Controller) {
    $Controller = Read-Host 'Controller IP address'
}

if ($Controller -notmatch '^https?://') { $Controller = 'http://' + $Controller }
$controllerUri = [uri]$Controller
if (-not $controllerUri.Host) { throw 'Please enter a valid controller address.' }
$baseUri = $controllerUri.GetLeftPart([System.UriPartial]::Authority)
if (-not $OutputDirectory) {
    $OutputDirectory = [Environment]::GetFolderPath('Desktop')
    if (-not $OutputDirectory) { $OutputDirectory = (Get-Location).Path }
}
$captureId = 'PoolChlorinator-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + ([guid]::NewGuid().ToString('N').Substring(0, 6))
$capturePath = Join-Path $OutputDirectory $captureId
$null = New-Item -ItemType Directory -Path $capturePath
$encoding = New-Object System.Text.UTF8Encoding($false)
$busPath = Join-Path $capturePath 'bus-timeline.txt'
$errorPath = Join-Path $capturePath 'capture-errors.txt'
$eventPath = Join-Path $capturePath 'display-observations.jsonl'
$markers = @{
    '0' = 'Normal / Auto shown'
    '1' = 'Manual Off shown'
    '2' = 'G4 fault visible'
    '3' = 'G4 cleared / communications restored'
    '4' = 'Cell production physically confirmed OFF'
    '5' = 'Cell production physically confirmed ON'
    '6' = 'Filter pump physically stopped'
    '7' = 'Filter pump physically running'
    '8' = 'Reset or restart initiated by operator'
    '9' = 'Touchscreen says chlorinator disconnected'
}
function Save-Marker([string]$Description) {
    $marker = [ordered]@{ captured_at = (Get-Date).ToString('o'); observation = $Description }
    [IO.File]::AppendAllText($eventPath, ($marker | ConvertTo-Json -Compress) + [Environment]::NewLine, $encoding)
    Write-Host ('  MARKED: ' + $Description) -ForegroundColor Cyan
}
Save-Marker $ObservedStatus
$observation = [ordered]@{
    capture_started = (Get-Date).ToString('o')
    controller = $baseUri
    observed_chlorinator_display = $ObservedStatus
    planned_duration_seconds = $Seconds
    bus_commands_sent_by_capture = 0
}
[IO.File]::WriteAllText((Join-Path $capturePath 'observation.json'), ($observation | ConvertTo-Json), $encoding)

function Save-Status([string]$Label) {
    try {
        $response = Invoke-WebRequest -Uri ($baseUri + '/status') -UseBasicParsing -TimeoutSec 5
        $parsed = $response.Content | ConvertFrom-Json
        $stamp = (Get-Date).ToString('o')
        $sample = [ordered]@{ captured_at = $stamp; status = $parsed }
        [IO.File]::WriteAllText((Join-Path $capturePath ('status-' + $Label + '.json')), ($sample | ConvertTo-Json -Depth 30), $encoding)
        $chlor = @($parsed.devices | Where-Object { $_.address -eq '0x0084' })
        foreach ($device in $chlor) {
            Write-Host ('  Viron last reply: {0} ms ago; longest gap: {1} ms' -f $device.last_seen_age_ms, $device.longest_message_gap_ms)
        }
    } catch {
        [IO.File]::AppendAllText($errorPath, ((Get-Date).ToString('o') + ' HTTP: ' + $_.Exception.Message + [Environment]::NewLine), $encoding)
        Write-Warning ('Status snapshot failed: ' + $_.Exception.Message)
    }
}

$client = New-Object System.Net.Sockets.TcpClient
$stream = $null
$busWriter = $null
$receivedBytes = 0L
$rawFrames = 0
Write-Host ('Capturing for {0} minutes. Keep this console selected when marking observations.' -f ($Seconds / 60))
Write-Host 'This script only reads: it does not change gateway settings or send bus commands.'
Write-Host 'Press a digit when you see a change (no Enter needed):'
foreach ($key in @('0','1','2','3','4','5','6','7','8','9')) { Write-Host ($key + ' = ' + $markers[$key]) }
Write-Host 'Only mark cell ON/OFF if confirmed by the chlorinator display or direct evidence, not pump operation alone.'
$canReadKeys = -not [Console]::IsInputRedirected
try {
    $connection = $client.ConnectAsync($controllerUri.Host, $DebugPort)
    if (-not $connection.Wait(5000)) { throw 'Timed out connecting to the controller debug port.' }
    $connection.GetAwaiter().GetResult()
    $stream = $client.GetStream()
    $stream.ReadTimeout = 1000
    $busWriter = New-Object System.IO.StreamWriter($busPath, $false, $encoding)
    $busWriter.AutoFlush = $true
    $buffer = New-Object byte[] 4096
    $pending = ''
    $clock = [Diagnostics.Stopwatch]::StartNew()
    $nextStatus = 0
    $nextProgress = 60
    while ($clock.Elapsed.TotalSeconds -lt $Seconds) {
        if ($canReadKeys) {
            try {
                while ([Console]::KeyAvailable) {
                    $key = [string][Console]::ReadKey($true).KeyChar
                    if ($markers.ContainsKey($key)) { Save-Marker $markers[$key] }
                }
            } catch {
                $canReadKeys = $false
                Write-Warning 'Live markers are unavailable in this host. Note the PC time and display changes separately.'
            }
        }
        if ($stream.DataAvailable) {
            $count = $stream.Read($buffer, 0, $buffer.Length)
            if ($count -eq 0) { throw 'Controller debug stream closed.' }
            if ($receivedBytes -eq 0) { Write-Host 'Debug stream is delivering data.' -ForegroundColor Green }
            $receivedBytes += $count
            $pending += [Text.Encoding]::ASCII.GetString($buffer, 0, $count)
            $lineEnd = $pending.IndexOf("`n")
            while ($lineEnd -ge 0) {
                $line = $pending.Substring(0, $lineEnd).TrimEnd("`r")
                $pending = $pending.Substring($lineEnd + 1)
                $busWriter.WriteLine((Get-Date).ToString('o') + ' ' + $line)
                if ($line -match '^\s*(?:[0-9A-Fa-f]{2}\s+){10,}[0-9A-Fa-f]{2}\s*$') {
                    $rawFrames++
                    if ($rawFrames -eq 1) { Write-Host 'Raw bus frames confirmed; capture is working.' -ForegroundColor Green }
                }
                $lineEnd = $pending.IndexOf("`n")
            }
        } else {
            if ($client.Client.Poll(0, [Net.Sockets.SelectMode]::SelectRead)) {
                throw 'Controller closed the debug connection.'
            }
            Start-Sleep -Milliseconds 50
        }
        if ($receivedBytes -eq 0 -and $clock.Elapsed.TotalSeconds -ge 10) {
            throw 'Port 7373 connected but sent no greeting or data for 10 seconds. The firmware accepts one debug client; close other PuTTY/debug/capture connections before retrying.'
        }
        if ($rawFrames -eq 0 -and $clock.Elapsed.TotalSeconds -ge 60) {
            throw 'No raw bus frames received for 60 seconds. This is not a working bus capture; see the saved debug log for details.'
        }
        if ($receivedBytes -gt 0 -and $clock.Elapsed.TotalSeconds -ge $nextStatus) {
            Save-Status ('{0:D4}s' -f [int]$clock.Elapsed.TotalSeconds)
            $nextStatus = $clock.Elapsed.TotalSeconds + 15
        }
        if ($clock.Elapsed.TotalSeconds -ge $nextProgress) {
            Write-Host ('  {0} seconds captured; {1} raw bus frames.' -f [int]$clock.Elapsed.TotalSeconds, $rawFrames)
            $nextProgress += 60
        }
    }
    if ($pending) { $busWriter.WriteLine((Get-Date).ToString('o') + ' ' + $pending) }
} catch {
    $failure = $_.Exception
    $detail = $failure.Message
    while ($failure.InnerException) {
        $failure = $failure.InnerException
        $detail += ' -> ' + $failure.Message
    }
    [IO.File]::AppendAllText($errorPath, ((Get-Date).ToString('o') + ' TCP: ' + $detail + [Environment]::NewLine), $encoding)
    Write-Warning ('Capture interrupted: ' + $detail)
} finally {
    if ($busWriter) { $busWriter.Dispose() }
    if ($stream) { $stream.Dispose() }
    $client.Dispose()
    Save-Status 'end'
}

try {
    $response = Invoke-WebRequest -Uri ($baseUri + '/unknown_msgs') -UseBasicParsing -TimeoutSec 5
    [IO.File]::WriteAllText((Join-Path $capturePath 'unknown-messages.json'), $response.Content, $encoding)
} catch {
    Write-Warning 'Could not collect Unknown Messages; other captured data is retained.'
}
$zipPath = $capturePath + '.zip'
[IO.File]::WriteAllText((Join-Path $capturePath 'capture-summary.json'), ([ordered]@{
    ended_at = (Get-Date).ToString('o')
    debug_bytes_received = $receivedBytes
    raw_bus_frames_received = $rawFrames
    has_bus_evidence = ($rawFrames -gt 0)
} | ConvertTo-Json), $encoding)
Compress-Archive -Path (Join-Path $capturePath '*') -DestinationPath $zipPath
Write-Host ''
Write-Host ('Capture saved: ' + $zipPath)
if ($rawFrames -eq 0) { Write-Warning 'No bus evidence was collected. Do not wait through another full capture until the debug connection delivers raw frames.' }
Write-Host 'Send that ZIP back in this chat. The original capture folder is also retained.'
Write-Host 'The ZIP includes the display observations marked with the number keys.'
