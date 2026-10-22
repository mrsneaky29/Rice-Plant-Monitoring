$ErrorActionPreference = 'Stop'
$taskPython = Join-Path $PSScriptRoot '.venv\Scripts\python.exe'
$taskConfigurator = Join-Path $PSScriptRoot 'deploy\configure_analysis.py'
Write-Host 'Rice Cloud: enter your OpenAI key privately. It will not be displayed.'
Write-Host 'This verifies model access and prepares private configuration; it does not analyze photos.'
& $taskPython $taskConfigurator --prompt-key
if ($LASTEXITCODE -ne 0) { throw 'Key verification did not finish. Live analysis remains unchanged.' }
