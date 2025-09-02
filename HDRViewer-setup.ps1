$ErrorActionPreference = 'Stop'
$here   = Split-Path -Parent $MyInvocation.MyCommand.Path
$log    = Join-Path $here 'HDRViewer-Setup.log'

function Write-Log($msg){ $ts = Get-Date -Format o; "$ts  $msg" | Tee-Object -FilePath $log -Append }

# 1) Ensure elevation
function Ensure-Admin {
  $id = [Security.Principal.WindowsIdentity]::GetCurrent()
  $p  = New-Object Security.Principal.WindowsPrincipal($id)
  if (-not $p.IsInRole([Security.Principal.WindowsBuiltinRole]::Administrator)) {
    Write-Log "Elevating..."
    Start-Process -FilePath "powershell.exe" -ArgumentList "-ExecutionPolicy Bypass -NoProfile -File `"$PSCommandPath`"" -Verb RunAs
    exit
  }
}
Ensure-Admin

# 2) Check VC++ Redist (x64) presence
function Has-VCRedist {
  $k = 'HKLM:\SOFTWARE\Microsoft\VisualStudio\VC\Runtimes\x64'
  if (Test-Path $k) {
    $p = Get-ItemProperty $k
    return ($p.Installed -eq 1)
  }
  return $false
}

# 3) Download & install if missing
function Install-VCRedist {
  $url = 'https://aka.ms/vs/17/release/vc_redist.x64.exe'   # official Microsoft redirect
  $dst = Join-Path $env:TEMP 'vc_redist.x64.exe'
  Write-Log "Downloading VC++ Redist from $url"
  Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $dst
  Write-Log "Installing VC++ Redist..."
  $p = Start-Process -FilePath $dst -ArgumentList '/install','/passive','/norestart' -Wait -PassThru
  Write-Log "VC++ installer exited with $($p.ExitCode)"
}

if (-not (Has-VCRedist)) {
  Install-VCRedist
  if (-not (Has-VCRedist)) {
    Write-Log "ERROR: VC++ Redist not detected after install."
    [System.Windows.Forms.MessageBox]::Show("Failed to install the Microsoft Visual C++ Redistributable. See HDRViewer-Setup.log.", "HDRViewer", 'OK', 'Error') | Out-Null
    exit 1
  }
} else {
  Write-Log "VC++ Redist already installed."
}

# 4) Optional: ensure D3DCompiler_47.dll for older systems (skip on most Win10/11)
function Needs-D3DCompiler {
  $sys = Join-Path $env:WINDIR 'System32\D3DCompiler_47.dll'
  return -not (Test-Path $sys)
}
if (Needs-D3DCompiler) {
  Write-Log "D3DCompiler_47.dll missing; attempting to install platform feature via DISM (requires Windows Update)."
  try {
    # Often satisfies on Win10/11 with latest updates; otherwise advise the user.
    dism /online /add-capability /capabilityname:Tools.Graphics.DirectX  | Out-Null
  } catch { Write-Log "DISM attempt failed: $_" }
  # If still missing, we simply proceed; your app will error only if it compiles shaders at runtime.
}

# 5) Launch app
$exe = Join-Path $here 'HDRViewer.exe'
Write-Log "Launching $exe"
Start-Process -FilePath $exe
