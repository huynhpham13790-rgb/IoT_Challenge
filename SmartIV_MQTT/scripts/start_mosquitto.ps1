$ErrorActionPreference = 'Stop'

$MosquittoExe = 'C:\Program Files\mosquitto\mosquitto.exe'
$ConfigFile = 'E:\SmartIV_MQTT\config\mosquitto.conf'

if (-not (Test-Path -LiteralPath $MosquittoExe -PathType Leaf)) {
    throw "mosquitto.exe not found: $MosquittoExe"
}

if (-not (Test-Path -LiteralPath $ConfigFile -PathType Leaf)) {
    throw "Config file not found: $ConfigFile"
}

& $MosquittoExe -c $ConfigFile -v
exit $LASTEXITCODE
