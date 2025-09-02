# Requires PowerShell (built into Windows 10/11). Run with right-click → Run with PowerShell if needed.

$ErrorActionPreference = 'Stop'

function Has-VCRedist {
  $keys = @(
    'HKLM:\SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64',
    'HKLM:\SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x86',
    'HKLM:\SOFTWARE\Microsoft\VisualStudio\VC\Runtimes\x64',
    'HKLM:\SOFTWARE\Microsoft\VisualStudio\VC\Runtimes\x86'
  )
  foreach ($k in $keys) {
    if (Test-Path $k) {
      $v = (Get-ItemProperty -Path $k -ErrorAction SilentlyContinue).Version
      if ($v) { return $true }
    }
  }
  return $false
}

$vcUrl = 'https://aka.ms/vs/17/release/vc_redist.x64.exe'  # Official Microsoft redirect (2015–2022)
$vcExe = Join-Path $env:TEMP 'vc_redist.x64.exe'

if (-not (Has-VCRedist)) {
  Write-Host 'Installing Microsoft Visual C++ Redistributable...'
  Invoke-WebRequest -UseBasicParsing -Uri $vcUrl -OutFile $vcExe
  # /install = full UI suppressed; /passive shows minimal progress; /norestart avoids reboot dialog
  Start-Process -FilePath $vcExe -ArgumentList '/install', '/passive', '/norestart' -Wait -Verb RunAs
}

# Launch the app from the same folder as the script
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
Start-Process -FilePath (Join-Path $here 'HDRViewer.exe')
