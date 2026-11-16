param([string]$FFmpeg, [string]$Directory)
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force $Directory | Out-Null
& $FFmpeg -hide_banner -loglevel error -y -f lavfi -i 'testsrc2=size=160x90:rate=24' `
    -frames:v 24 -c:v libx264 -threads 2 -bf 3 -pix_fmt yuv420p (Join-Path $Directory 'h264.mp4')
if ($LASTEXITCODE -ne 0) { throw 'H264 test fixture generation failed' }
& $FFmpeg -hide_banner -loglevel error -y -f lavfi -i 'testsrc2=size=160x90:rate=24' `
    -frames:v 24 -c:v ffv1 -threads 2 -pix_fmt yuv444p (Join-Path $Directory 'ffv1.mkv')
if ($LASTEXITCODE -ne 0) { throw 'FFV1 test fixture generation failed' }
& $FFmpeg -hide_banner -loglevel error -y -i (Join-Path $Directory 'ffv1.mkv') `
    -c copy -bsf:v 'noise=amount=if(gte(n\,8)\,1\,0)' (Join-Path $Directory 'damaged.mkv')
if ($LASTEXITCODE -ne 0) { throw 'Damaged decoder fixture generation failed' }
# A previous cancelled-import test must not affect the next run.
$cancelled = Join-Path $Directory 'cancelled.mp4'
if (Test-Path -LiteralPath $cancelled) { Remove-Item -LiteralPath $cancelled }
