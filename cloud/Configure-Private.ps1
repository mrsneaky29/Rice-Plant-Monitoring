$ErrorActionPreference = 'Stop'
$taskPython = Join-Path $PSScriptRoot '.venv\Scripts\python.exe'
if (-not (Test-Path -LiteralPath $taskPython)) {
    throw 'Create the project Python environment first; see README.md.'
}
& $taskPython (Join-Path $PSScriptRoot 'deploy\configure.py')
if ($LASTEXITCODE -ne 0) { throw 'Private setup did not finish.' }
$taskPrivateDirectory = Join-Path $PSScriptRoot '.private'
$taskCurrentIdentity = [System.Security.Principal.WindowsIdentity]::GetCurrent().Name
& icacls $taskPrivateDirectory /inheritance:r /grant:r "${taskCurrentIdentity}:(OI)(CI)F" 'SYSTEM:(OI)(CI)F' /Q | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'Could not restrict private configuration permissions.' }
& icacls (Join-Path $taskPrivateDirectory '*') /reset /T /Q | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'Could not apply inherited private file permissions.' }
Write-Host 'Private setup complete. Tell Codex it is ready; do not paste the files into chat.'
