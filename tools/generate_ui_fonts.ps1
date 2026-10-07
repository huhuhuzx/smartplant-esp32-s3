param([string]$FontPath = (Join-Path $env:WINDIR 'Fonts\NotoSansSC-VF.ttf'), [string]$Python = 'python', [string]$Node = 'node')
$ErrorActionPreference = 'Stop'
$project = Split-Path $PSScriptRoot -Parent
$converter = Join-Path $project '.local\font-tools\node_modules\lv_font_conv\lv_font_conv.js'
if (-not (Test-Path -LiteralPath $FontPath)) { throw 'Install Noto Sans SC or pass -FontPath to its TTF file.' }
if (-not (Test-Path -LiteralPath $converter)) { throw 'Run npm install --prefix .local/font-tools lv_font_conv@1.5.3 first.' }
$fixedFont = Join-Path $project '.local/NotoSansSC-Medium.ttf'
& $Python (Join-Path $PSScriptRoot 'prepare_ui_font.py') $FontPath $fixedFont
if ($LASTEXITCODE -ne 0) { throw 'Font preparation failed. Install fonttools for the selected Python.' }
$source = [IO.File]::ReadAllText((Join-Path $project 'main\screen.c'))
$characters = ([regex]::Matches($source, '[\u4e00-\u9fff]') | ForEach-Object Value | Sort-Object -Unique) -join ''
& $Node $converter --font $fixedFont --size 20 --bpp 4 --format lvgl --range '0x20-0x7E,0xA0-0xFF,0x2000-0x206F,0x3000-0x303F,0x4E00-0x9FFF,0xFF00-0xFFEF' --lv-font-name ui_font_20 -o (Join-Path $project 'main\ui_font_20.c')
if ($LASTEXITCODE -ne 0) { throw '20px Chinese font generation failed.' }
& $Node $converter --font $fixedFont --size 28 --bpp 4 --format lvgl --range '0x20-0x7E,0xA0-0xFF,0x2000-0x206F,0x3000-0x303F,0xFF00-0xFFEF' --symbols $characters --lv-font-name ui_font_28 -o (Join-Path $project 'main\ui_font_28.c')
if ($LASTEXITCODE -ne 0) { throw '28px Chinese font generation failed.' }
foreach ($name in @('ui_font_20.c','ui_font_28.c')) {
    $path = Join-Path $project ('main/' + $name)
    $generated = [IO.File]::ReadAllText($path)
    $generated = [regex]::Replace($generated, '(?s)^/\*.*?\*/', '// Noto Sans SC derivative. SIL OFL 1.1; see licenses/NotoSansSC-OFL.txt.')
    [IO.File]::WriteAllText($path, $generated, [Text.UTF8Encoding]::new($false))
}
