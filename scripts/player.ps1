param(
    [ValidateSet(
        "help",
        "install",
        "start-test",
        "start",
        "restart-test",
        "restart",
        "stop",
        "status",
        "logs",
        "logs-follow",
        "smoke",
        "test",
        "open"
    )]
    [string]$Action = "help",
    [string]$ConfigFile = "config\player.json",
    [string]$AirIp = "",
    [int]$WebPort = 0,
    [int]$GatewayPort = 0,
    [int]$MavlinkPort = 0,
    [string]$MavlinkSerial = "",
    [int]$MavlinkBaud = 0,
    [string]$BootstrapPython = "",
    [string]$PipIndexUrl = "",
    [switch]$DebugControl,
    [switch]$NoBrowser
)

$ErrorActionPreference = "Stop"
$script:RootDir = Split-Path -Parent $PSScriptRoot
$script:VenvDir = Join-Path $script:RootDir ".venv"
$script:PythonExe = Join-Path $script:VenvDir "Scripts\python.exe"

function Write-Usage {
    Write-Host @"
RK3588 player manager

Usage:
  player.cmd install       Create .venv and install dependencies
  player.cmd start-test    Start web + gateway in dry-run mode
  player.cmd start         Start web + real MAVLink gateway
  player.cmd restart-test  Restart in dry-run mode
  player.cmd restart       Restart in real MAVLink mode
  player.cmd stop          Stop web and gateway
  player.cmd status        Show service status and URLs
  player.cmd logs          Show recent logs
  player.cmd logs-follow   Follow button command and ACK logs
  player.cmd smoke         Send one button command and verify its ACK
  player.cmd test          Run local unit and syntax tests
  player.cmd open          Open the player in the default browser

Options:
  -ConfigFile PATH         Player and MAVLink JSON configuration
  -AirIp 192.168.31.14     Airborne MediaMTX/MAVLink address
  -WebPort 8090            Local web player port
  -GatewayPort 8091        Local WebSocket gateway port
  -MavlinkPort 14550       Airborne MAVLink UDP port
  -MavlinkSerial COM5      Use serial instead of UDP
  -MavlinkBaud 115200      Serial baud rate
  -BootstrapPython PATH    Explicit Python executable for first install
  -PipIndexUrl URL         Optional Python package mirror
  -DebugControl            Log raw button data and MAVLink parameters
  -NoBrowser               Do not open the browser after start
"@
}

function Test-VirtualEnvironmentPython {
    if (-not (Test-Path -LiteralPath $script:PythonExe)) {
        return $false
    }

    $previousErrorAction = $ErrorActionPreference
    $ErrorActionPreference = "SilentlyContinue"
    try {
        & $script:PythonExe --version 2>$null | Out-Null
        return $LASTEXITCODE -eq 0
    } finally {
        $ErrorActionPreference = $previousErrorAction
    }
}

function New-VirtualEnvironment {
    if (Test-VirtualEnvironmentPython) {
        return
    }

    if (Test-Path -LiteralPath $script:VenvDir) {
        Write-Host "Removing incomplete virtual environment: $script:VenvDir"
        Remove-Item -LiteralPath $script:VenvDir -Recurse -Force
    }

    Write-Host "Creating virtual environment: $script:VenvDir"
    if ($BootstrapPython) {
        if (-not (Test-Path -LiteralPath $BootstrapPython)) {
            throw "Python executable does not exist: $BootstrapPython"
        }
        & $BootstrapPython -m venv $script:VenvDir
    } else {
    $pyLauncher = Get-Command py -ErrorAction SilentlyContinue
    if ($null -ne $pyLauncher) {
        & $pyLauncher.Source -3 -m venv $script:VenvDir
    } else {
        $python = Get-Command python -ErrorAction SilentlyContinue
        if ($null -ne $python) {
            & $python.Source -m venv $script:VenvDir
        } else {
            $localPythonRoot = Join-Path $env:LOCALAPPDATA "Programs\Python"
            $pythonPath = $null
            foreach ($version in @("314", "313", "312", "311", "310")) {
                $candidate = Join-Path $localPythonRoot "Python${version}\python.exe"
                if (Test-Path -LiteralPath $candidate) {
                    $pythonPath = $candidate
                    break
                }
            }
            if ($null -eq $pythonPath) {
                $pythonExe = Get-ChildItem `
                    -Path $localPythonRoot `
                    -Filter python.exe `
                    -Recurse `
                    -ErrorAction SilentlyContinue |
                    Where-Object { $_.FullName -notmatch "\\Scripts\\" } |
                    Sort-Object FullName -Descending |
                    Select-Object -First 1
                if ($null -ne $pythonExe) {
                    $pythonPath = $pythonExe.FullName
                }
            }
            if ($null -eq $pythonPath) {
                throw "Python 3 was not found. Install Python 3.10+ and enable Add Python to PATH."
            }
            & $pythonPath -m venv $script:VenvDir
        }
    }
    }

    if (-not (Test-Path -LiteralPath $script:PythonExe)) {
        throw "Virtual environment creation failed: $script:PythonExe"
    }
}

function Install-Dependencies {
    New-VirtualEnvironment
    Write-Host "Installing ground gateway dependencies..."
    $pipArguments = @(
        "-m", "pip", "install",
        "--default-timeout", "300",
        "--retries", "10",
        "-r", (Join-Path $script:RootDir "ground_gateway\requirements.txt")
    )
    if ($PipIndexUrl) {
        $pipArguments += @("--index-url", $PipIndexUrl)
    }
    & $script:PythonExe @pipArguments
    if ($LASTEXITCODE -ne 0) {
        throw "Dependency installation failed."
    }
    Write-Host "Installation complete."
}

function Test-Environment {
    if (-not (Test-VirtualEnvironmentPython)) {
        return $false
    }

    $previousErrorAction = $ErrorActionPreference
    $ErrorActionPreference = "SilentlyContinue"
    try {
        & $script:PythonExe -c "import fastapi, uvicorn, pymavlink" 2>$null | Out-Null
        return $LASTEXITCODE -eq 0
    } finally {
        $ErrorActionPreference = $previousErrorAction
    }
}

function Ensure-Environment {
    if (-not (Test-Environment)) {
        Install-Dependencies
    }
}

function Start-Player([bool]$DryRun) {
    Ensure-Environment
    $arguments = @(
        (Join-Path $script:RootDir "scripts\service_manager.py"),
        "start",
        "--config", $ConfigFile
    )
    if ($AirIp) { $arguments += @("--air-ip", $AirIp) }
    if ($WebPort -gt 0) { $arguments += @("--web-port", "$WebPort") }
    if ($GatewayPort -gt 0) { $arguments += @("--gateway-port", "$GatewayPort") }
    if ($MavlinkPort -gt 0) { $arguments += @("--mavlink-port", "$MavlinkPort") }
    if ($MavlinkBaud -gt 0) { $arguments += @("--mavlink-baud", "$MavlinkBaud") }
    if ($MavlinkSerial) {
        $arguments += @("--mavlink-serial", $MavlinkSerial)
    }
    if ($DryRun) {
        $arguments += "--dry-run"
    }
    if ($NoBrowser) {
        $arguments += "--no-browser"
    }
    if ($DebugControl) {
        $arguments += "--debug-control"
    }
    & $script:PythonExe @arguments
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
}

function Stop-Player {
    if (-not (Test-Path -LiteralPath $script:PythonExe)) {
        Write-Host "The local environment is not installed."
        return
    }
    & $script:PythonExe (Join-Path $script:RootDir "scripts\service_manager.py") stop
}

function Show-Status {
    if (-not (Test-Path -LiteralPath $script:PythonExe)) {
        Write-Host "Web      STOPPED"
        Write-Host "Gateway  STOPPED"
        return
    }
    & $script:PythonExe `
        (Join-Path $script:RootDir "scripts\service_manager.py") `
        status `
        --config $ConfigFile
}

function Show-Logs {
    if (-not (Test-Path -LiteralPath $script:PythonExe)) {
        Write-Host "The local environment is not installed."
        return
    }
    & $script:PythonExe (Join-Path $script:RootDir "scripts\service_manager.py") logs
}

function Follow-Logs {
    $path = Join-Path $script:RootDir ".run\gateway.err.log"
    if (-not (Test-Path -LiteralPath $path)) {
        Write-Host "Gateway log does not exist. Start the service first."
        return
    }
    Write-Host "Following gateway commands. Press Ctrl+C to stop viewing."
    Get-Content -LiteralPath $path -Tail 40 -Wait
}

function Run-SmokeTest {
    Ensure-Environment
    & $script:PythonExe `
        (Join-Path $script:RootDir "scripts\service_manager.py") `
        smoke `
        --config $ConfigFile
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
}

function Run-Tests {
    Ensure-Environment
    Push-Location $script:RootDir
    try {
        & $script:PythonExe -m unittest discover -s ground_gateway/tests -v
        if ($LASTEXITCODE -ne 0) {
            exit $LASTEXITCODE
        }
        & $script:PythonExe -m compileall -q ground_gateway
        if ($LASTEXITCODE -ne 0) {
            exit $LASTEXITCODE
        }
        & $script:PythonExe -m py_compile scripts/service_manager.py
        if ($LASTEXITCODE -ne 0) {
            exit $LASTEXITCODE
        }
        Write-Host "All tests passed."
    } finally {
        Pop-Location
    }
}

switch ($Action) {
    "help" { Write-Usage }
    "install" { Install-Dependencies }
    "start-test" { Start-Player $true }
    "start" { Start-Player $false }
    "restart-test" { Stop-Player; Start-Player $true }
    "restart" { Stop-Player; Start-Player $false }
    "stop" { Stop-Player }
    "status" { Show-Status }
    "logs" { Show-Logs }
    "logs-follow" { Follow-Logs }
    "smoke" { Run-SmokeTest }
    "test" { Run-Tests }
    "open" {
        Ensure-Environment
        & $script:PythonExe `
            (Join-Path $script:RootDir "scripts\service_manager.py") `
            open `
            --config $ConfigFile
    }
}
