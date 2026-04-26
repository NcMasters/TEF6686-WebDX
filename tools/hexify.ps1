$htmlPath = '.\tef6686 web\radio.html'
$dbPath = '.\tef6686 web\stations_db.js'

if (-not (Test-Path $htmlPath)) { Write-Error "Missing $htmlPath"; exit 1 }
if (-not (Test-Path $dbPath)) { Write-Error "Missing $dbPath"; exit 1 }

$html = Get-Content $htmlPath -Raw -Encoding UTF8
$db = Get-Content $dbPath -Raw -Encoding UTF8

function Get-HexGzip($str) {
    if ([string]::IsNullOrEmpty($str)) { return "", 0 }
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($str)
    $ms = New-Object System.IO.MemoryStream
    $gs = New-Object System.IO.Compression.GZipStream($ms, [System.IO.Compression.CompressionMode]::Compress)
    $gs.Write($bytes, 0, $bytes.Length)
    $gs.Close()
    $gz = $ms.ToArray()
    $hex = ""
    for ($i = 0; $i -lt $gz.Length; $i++) {
        $hex += "0x" + $gz[$i].ToString("X2") + ","
        if (($i + 1) % 16 -eq 0) { $hex += "`n" }
    }
    return $hex, $gz.Length
}

try {
    $hexHtml, $lenHtml = Get-HexGzip $html
    $outHtml = "const uint32_t WEB_HTML_LEN = $lenHtml;`nconst uint8_t WEB_HTML[] PROGMEM = {`n$hexHtml`n};"
    $outHtmlFile = Join-Path $PSScriptRoot "html_hex.h"
    $outHtml | Out-File -FilePath $outHtmlFile -Encoding ASCII

    $hexDb, $lenDb = Get-HexGzip $db
    $outDb = "const uint32_t WEB_DB_LEN = $lenDb;`nconst uint8_t WEB_DB[] PROGMEM = {`n$hexDb`n};"
    $outDbFile = Join-Path $PSScriptRoot "db_hex.h"
    $outDb | Out-File -FilePath $outDbFile -Encoding ASCII
    
    Write-Host "Successfully hexified web assets."
} catch {
    Write-Error "Error during hexification: $_"
    exit 1
}
