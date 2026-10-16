$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$hostExe = Join-Path $repo 'target\release\easy-codex-host.exe'
$desktopExe = Join-Path $repo 'target\release\Codex Keyboard.exe'

function Invoke-HostControl([string]$Command) {
    $info = New-Object System.Diagnostics.ProcessStartInfo
    $info.FileName = $hostExe
    $info.Arguments = $Command
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $process = [System.Diagnostics.Process]::Start($info)
    try {
        # The detached daemon may inherit pipe handles from its installer.
        # Wait for the control process, not EOF on its inherited output pipes.
        if (!$process.WaitForExit(20000)) { throw 'Host control command timed out.' }
        return @{ Code = $process.ExitCode }
    } finally {
        $process.Dispose()
    }
}

try {
    if (!(Test-Path -LiteralPath $hostExe) -or !(Test-Path -LiteralPath $desktopExe)) {
        throw 'Build the Windows Host and desktop first. See docs/windows-startup.md.'
    }
    foreach ($binary in @($hostExe, $desktopExe)) {
        & icacls.exe $binary /setintegritylevel M *> $null
        if ($LASTEXITCODE -ne 0) { throw 'Cannot restore normal executable integrity. Run this launcher from your normal user desktop.' }
    }
    $health = Invoke-HostControl 'health'
    if ($health.Code -ne 0) {
        $install = Invoke-HostControl 'launch-agent-install'
        if ($install.Code -ne 0) { throw 'Host could not start. Run easy-codex-host.exe daemon to inspect the error.' }
    }
    Start-Process -FilePath $desktopExe
} catch {
    Write-Host $_ -ForegroundColor Red
    Read-Host 'Startup failed. Press Enter to close'
    exit 1
}
