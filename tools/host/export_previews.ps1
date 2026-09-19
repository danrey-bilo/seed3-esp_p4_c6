# Export native LVGL framebuffer captures; no image alteration or mock UI.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$captures = @{
    'parallel-routing.bmp' = 'pedalboard-routing.png'
    'patch-selected.bmp' = 'pedalboard-selected.png'
    'knobs.bmp' = 'pedalboard-knobs.png'
    'cabinet-editor.bmp' = 'pedalboard-cabinet.png'
    'categories.bmp' = 'pedalboard-categories.png'
    'wifi-connected.bmp' = 'wifi-settings.png'
    'wifi-password.bmp' = 'wifi-password.png'
    'grid-placement.bmp' = 'pedalboard-grid.png'
    'grid-scrolled.bmp' = 'pedalboard-grid-scrolled.png'
    'grid-drag.bmp' = 'pedalboard-grid-drag.png'
}
foreach ($name in $captures.Keys) {
    $source = Join-Path $repo ('.tmp\host-ui\' + $name)
    $destination = Join-Path $repo ('docs\assets\' + $captures[$name])
    $bitmap = [System.Drawing.Image]::FromFile($source)
    try { $bitmap.Save($destination, [System.Drawing.Imaging.ImageFormat]::Png) }
    finally { $bitmap.Dispose() }
    Write-Output $destination
}
