# Regenerate docs/weigh-station-quickrefs.pdf from the four quickref Markdown
# files, one ref per Letter page. Needs Node.js and Microsoft Edge (or set
# $env:BROWSER to another Chromium browser's exe).
#
#   powershell -ExecutionPolicy Bypass -File tools\quickrefs\build-pdf.ps1

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = Resolve-Path (Join-Path $here '..\..')
$docs = Join-Path $repo 'docs'
$html = Join-Path $env:TEMP 'weigh-station-quickrefs.html'
$pdf  = Join-Path $docs 'weigh-station-quickrefs.pdf'

$refs = 'quickref-weighing.md', 'quickref-onboarding.md',
        'quickref-inventorying.md', 'quickref-tag-erase.md' |
        ForEach-Object { Join-Path $docs $_ }
node (Join-Path $here 'build.js') $html @refs
if ($LASTEXITCODE) { throw "build.js failed" }

$browser = $env:BROWSER
if (-not $browser) { $browser = "${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe" }
if (-not (Test-Path $browser)) { throw "No browser at $browser; set `$env:BROWSER" }

if (Test-Path $pdf) { Remove-Item $pdf }
$url = 'file:///' + ($html -replace '\\', '/')
& $browser --headless=new --disable-gpu --no-pdf-header-footer "--print-to-pdf=$pdf" $url 2>$null
for ($i = 0; $i -lt 20 -and -not (Test-Path $pdf); $i++) { Start-Sleep -Milliseconds 500 }
if (-not (Test-Path $pdf)) { throw "No PDF produced" }

$text  = [IO.File]::ReadAllText($pdf, [Text.Encoding]::GetEncoding('latin1'))
$pages = ([regex]::Matches($text, '/Type\s*/Page[^s]')).Count
"wrote $pdf ($pages pages)"
if ($pages -ne $refs.Count) { Write-Warning "expected $($refs.Count) pages: a ref no longer fits on one page" }
