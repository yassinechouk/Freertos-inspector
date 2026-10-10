<#
  Copies protocol/hil_proto.h to the places that build with it:
    firmware/Core/Inc/hil_proto.h   (STM32, CubeIDE)
    supervisor/hil_proto.h          (Elyssa, Arduino IDE: a sketch can only include files from its own folder)
  Edit protocol/hil_proto.h, then run this script from anywhere. The copies get a header line saying so.
#>
$root = Split-Path -Parent $PSScriptRoot
$src  = Join-Path $root 'protocol\hil_proto.h'
$note = "/* GENERATED COPY of protocol/hil_proto.h - do not edit; run tools/sync_proto.ps1 */`r`n"
foreach ($dst in 'firmware\Core\Inc\hil_proto.h', 'supervisor\hil_proto.h') {
  $text = [System.IO.File]::ReadAllText($src)
  [System.IO.File]::WriteAllText((Join-Path $root $dst), $note + $text)
  Write-Host "updated $dst"
}
