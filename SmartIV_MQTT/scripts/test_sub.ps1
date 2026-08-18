$ErrorActionPreference = 'Stop'

$MosquittoSub = 'C:\Program Files\mosquitto\mosquitto_sub.exe'

if (-not (Test-Path -LiteralPath $MosquittoSub -PathType Leaf)) {
    throw "mosquitto_sub.exe not found: $MosquittoSub"
}

& $MosquittoSub -h localhost -p 1885 -t 'smartiv/test' -v
exit $LASTEXITCODE
