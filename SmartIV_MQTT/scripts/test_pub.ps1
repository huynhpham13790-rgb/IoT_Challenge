$ErrorActionPreference = 'Stop'

$MosquittoPub = 'C:\Program Files\mosquitto\mosquitto_pub.exe'

if (-not (Test-Path -LiteralPath $MosquittoPub -PathType Leaf)) {
    throw "mosquitto_pub.exe not found: $MosquittoPub"
}

& $MosquittoPub -h localhost -p 1885 -t 'smartiv/test' -m 'HELLO_SMART_IV'
exit $LASTEXITCODE
