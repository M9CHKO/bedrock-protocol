param([Parameter(Mandatory=$true)][string]$Exe)
$ErrorActionPreference = 'Stop'
$binary = (Resolve-Path -LiteralPath $Exe).Path
$pipeName = 'CpeRelay-BundleSmoke-' + [guid]::NewGuid().ToString('N')
$options = [IO.Pipes.PipeOptions]::Asynchronous -bor [IO.Pipes.PipeOptions]::CurrentUserOnly
$pipe = [IO.Pipes.NamedPipeServerStream]::new($pipeName, [IO.Pipes.PipeDirection]::InOut, 1, [IO.Pipes.PipeTransmissionMode]::Byte, $options)
$child = $null
function Read-Exact([int]$count) {
    $buffer = [byte[]]::new($count)
    $offset = 0
    while ($offset -lt $count) {
        $read = $pipe.ReadAsync($buffer, $offset, $count - $offset)
        if (!$read.Wait(10000)) { throw 'Worker response timed out' }
        if ($read.Result -eq 0) { throw 'Worker pipe closed' }
        $offset += $read.Result
    }
    return ,$buffer
}
function Exchange([hashtable]$request) {
    $bytes = [Text.Encoding]::UTF8.GetBytes(($request | ConvertTo-Json -Compress))
    $header = [BitConverter]::GetBytes([int]$bytes.Length)
    $pipe.Write($header, 0, 4)
    $pipe.Write($bytes, 0, $bytes.Length)
    $pipe.Flush()
    $count = [BitConverter]::ToInt32((Read-Exact 4), 0)
    if ($count -lt 1 -or $count -gt 4194304) { throw 'Invalid IPC length' }
    return [Text.Encoding]::UTF8.GetString((Read-Exact $count)) | ConvertFrom-Json
}
try {
    # No windows, user settings, auth, gameplay commands or user UDP port.
    $child = Start-Process -FilePath $binary -ArgumentList '--relay-worker',$pipeName -WindowStyle Hidden -PassThru
    if (!$pipe.WaitForConnectionAsync().Wait(10000)) { throw 'Worker connection timed out' }
    $versions = Exchange @{action='versions'}
    if ($versions -notcontains '1.21.2' -or $versions -notcontains '1.21.100') { throw 'Bundled codecs missing' }
    for ($i = 0; $i -lt 100; $i++) {
        $state = Exchange @{action='snapshot'}
        if ($state.running) { throw 'Isolated worker unexpectedly started a relay' }
    }
    $result = Exchange @{action='stop'}
    if (!$result.ok -or !$child.WaitForExit(10000) -or $child.ExitCode -ne 0) { throw 'Bundled worker failed graceful close' }
    Write-Output 'PASS: standalone EXE extracts/loads native DLL and both codecs, 100 IPC snapshots, graceful worker shutdown. No user session modified.'
} finally {
    $pipe.Dispose()
    if ($child) {
        # Only the exact child process created by this test, never a name match.
        if (!$child.HasExited) { $child.Kill(); $child.WaitForExit(5000) | Out-Null }
        $child.Dispose()
    }
}
