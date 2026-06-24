[CmdletBinding()]
param([string]$SamplesPath = '.\samples')
$ErrorActionPreference = 'Stop'
foreach ($tool in @('ffprobe', 'ffmpeg')) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) { throw "$tool is not on PATH" }
}
$files = @(Get-ChildItem -LiteralPath $SamplesPath -Filter '*.mp4' -File -ErrorAction Stop)
if ($files.Count -eq 0) { throw "No MP4 files found under $SamplesPath" }
$failed = $false
foreach ($file in $files) {
    Write-Host "`n=== $($file.Name) ==="
    $json = & ffprobe -v error -count_frames -select_streams v:0 `
        -show_entries 'stream=codec_name,width,height,pix_fmt,color_range,color_space,color_primaries,color_transfer,chroma_location,r_frame_rate,avg_frame_rate,duration,bit_rate,nb_read_frames' `
        -of json -- $file.FullName
    if ($LASTEXITCODE -ne 0) { Write-Error "ffprobe failed: $($file.FullName)"; $failed = $true; continue }
    $stream = ($json | ConvertFrom-Json).streams[0]
    $stream | Format-List codec_name,width,height,pix_fmt,color_range,color_space,color_primaries,color_transfer,chroma_location,r_frame_rate,avg_frame_rate,duration,bit_rate,nb_read_frames
    if ($stream.codec_name -ne 'h264' -or $stream.width -ne 1280 -or $stream.height -ne 720 -or
        $stream.pix_fmt -notin @('yuv420p', 'yuvj420p')) {
        Write-Error "Unsupported mandatory-path properties in $($file.Name)"
        $failed = $true
    }
    & ffmpeg -hide_banner -loglevel error -i $file.FullName -map '0:v:0' -f null -
    if ($LASTEXITCODE -ne 0) { Write-Error "Full decode failed: $($file.FullName)"; $failed = $true }
}
if ($failed) { exit 1 }
Write-Host "`nAll $($files.Count) sample(s) passed probe and decode validation."

