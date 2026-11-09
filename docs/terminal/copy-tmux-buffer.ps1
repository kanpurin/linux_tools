$ErrorActionPreference = 'Stop'
$start = New-Object System.Diagnostics.ProcessStartInfo
$start.FileName = 'ssh.exe'
$start.Arguments = '-o BatchMode=yes -o ConnectTimeout=10 virtualbox-linux tmux save-buffer -'
$start.UseShellExecute = $false
$start.CreateNoWindow = $true
$start.RedirectStandardOutput = $true
$start.RedirectStandardError = $true
$start.StandardOutputEncoding = New-Object System.Text.UTF8Encoding($false)
$process = New-Object System.Diagnostics.Process
$process.StartInfo = $start
[void]$process.Start()
$errorRead = $process.StandardError.ReadToEndAsync()
$text = $process.StandardOutput.ReadToEnd()
$process.WaitForExit()
$errorText = $errorRead.GetAwaiter().GetResult()
if ($process.ExitCode -ne 0) { throw "SSH failed: $errorText" }
Set-Clipboard -Value $text
Write-Host ('Copied {0} characters from tmux to Windows clipboard.' -f $text.Length)
$process.Dispose()
