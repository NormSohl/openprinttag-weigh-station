# Regenerate the printable PDFs in docs/:
#   weigh-station-quickrefs.pdf  the four quickrefs, one Letter page each
#   weigh-station-manual.pdf     the user manual
# Needs Node.js and Microsoft Edge (or set $env:BROWSER to another Chromium
# browser's exe).
#
#   powershell -ExecutionPolicy Bypass -File tools\quickrefs\build-pdf.ps1

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = Resolve-Path (Join-Path $here '..\..')
$docs = Join-Path $repo 'docs'

$browser = $env:BROWSER
if (-not $browser) { $browser = "${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe" }
if (-not (Test-Path $browser)) { throw "No browser at $browser; set `$env:BROWSER" }

function Build-Pdf($mode, $name, [string[]]$sources) {
    $html = Join-Path $env:TEMP "$name.html"
    $pdf  = Join-Path $docs "$name.pdf"
    node (Join-Path $here 'build.js') $mode $html @sources
    if ($LASTEXITCODE) { throw "build.js failed for $name" }
    if (Test-Path $pdf) { Remove-Item $pdf }
    $url = 'file:///' + ($html -replace '\\', '/')
    & $browser --headless=new --disable-gpu --no-pdf-header-footer "--print-to-pdf=$pdf" $url 2>$null
    for ($i = 0; $i -lt 20 -and -not (Test-Path $pdf); $i++) { Start-Sleep -Milliseconds 500 }
    if (-not (Test-Path $pdf)) { throw "No PDF produced for $name" }
    $text = [IO.File]::ReadAllText($pdf, [Text.Encoding]::GetEncoding('latin1'))
    $pages = ([regex]::Matches($text, '/Type\s*/Page[^s]')).Count
    "wrote $pdf ($pages pages)"
    return $pages
}

$refs = 'quickref-weighing.md', 'quickref-onboarding.md',
        'quickref-inventorying.md', 'quickref-tag-erase.md' |
        ForEach-Object { Join-Path $docs $_ }
$pages = Build-Pdf 'refs' 'weigh-station-quickrefs' $refs
if ($pages[-1] -ne $refs.Count) { Write-Warning "expected $($refs.Count) quickref pages: a ref no longer fits on one page" }

Build-Pdf 'manual' 'weigh-station-manual' @(Join-Path $docs 'user-manual.md') | Out-Null
