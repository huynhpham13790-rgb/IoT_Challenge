$ErrorActionPreference = 'Stop'

$Zigbee2MqttDirectory = Join-Path $PSScriptRoot 'zigbee2mqtt'
$ConfigurationFile = Join-Path $Zigbee2MqttDirectory 'data\configuration.yaml'
$ToolingDirectory = Join-Path $PSScriptRoot 'tools'

if (-not (Test-Path -LiteralPath $Zigbee2MqttDirectory -PathType Container)) {
    throw "Zigbee2MQTT directory not found: $Zigbee2MqttDirectory"
}

if (-not (Test-Path -LiteralPath $ConfigurationFile -PathType Leaf)) {
    throw "Configuration file not found: $ConfigurationFile"
}

$env:PATH = "$ToolingDirectory;$env:PATH"

Push-Location $Zigbee2MqttDirectory
try {
    & corepack pnpm start
    exit $LASTEXITCODE
}
finally {
    Pop-Location
}
