param(
    [string]$OutputDirectory = "docs/osd_mockups"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

Add-Type -AssemblyName System.Drawing

function New-OsdMockup {
    param(
        [string]$Path,
        [bool]$Wireframe,
        [bool]$Transparent = $false
    )

    $width = 1920
    $height = 1080
    $bitmap = New-Object System.Drawing.Bitmap($width, $height)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    if ($Transparent) {
        $graphics.Clear([System.Drawing.Color]::Transparent)
    } else {
        $graphics.Clear([System.Drawing.Color]::Black)
    }
    $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $graphics.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit

    $white = [System.Drawing.Color]::White
    $red = [System.Drawing.Color]::FromArgb(255, 32, 32)
    $gray = [System.Drawing.Color]::FromArgb(96, 255, 255, 255)
    $faint = [System.Drawing.Color]::FromArgb(40, 255, 255, 255)

    $whitePen = New-Object System.Drawing.Pen($white, 3)
    $thinPen = New-Object System.Drawing.Pen($white, 2)
    $redPen = New-Object System.Drawing.Pen($red, 3)
    $guidePen = New-Object System.Drawing.Pen($gray, 1)
    $guidePen.DashStyle = [System.Drawing.Drawing2D.DashStyle]::Dash

    $whiteBrush = New-Object System.Drawing.SolidBrush($white)
    $redBrush = New-Object System.Drawing.SolidBrush($red)
    $grayBrush = New-Object System.Drawing.SolidBrush($gray)

    $fontSmall = New-Object System.Drawing.Font("Consolas", 18, [System.Drawing.FontStyle]::Regular, [System.Drawing.GraphicsUnit]::Pixel)
    $fontMedium = New-Object System.Drawing.Font("Consolas", 24, [System.Drawing.FontStyle]::Bold, [System.Drawing.GraphicsUnit]::Pixel)
    $fontLarge = New-Object System.Drawing.Font("Consolas", 30, [System.Drawing.FontStyle]::Bold, [System.Drawing.GraphicsUnit]::Pixel)
    $fontWatermark = New-Object System.Drawing.Font("Consolas", 22, [System.Drawing.FontStyle]::Regular, [System.Drawing.GraphicsUnit]::Pixel)

    $centerFormat = New-Object System.Drawing.StringFormat
    $centerFormat.Alignment = [System.Drawing.StringAlignment]::Center
    $centerFormat.LineAlignment = [System.Drawing.StringAlignment]::Center

    $nearFormat = New-Object System.Drawing.StringFormat
    $nearFormat.Alignment = [System.Drawing.StringAlignment]::Near
    $nearFormat.LineAlignment = [System.Drawing.StringAlignment]::Center

    $degree = [char]0x00B0

    if ($Wireframe) {
        $graphics.DrawRectangle($guidePen, 96, 54, 1728, 972)
        $graphics.DrawLine($guidePen, 960, 54, 960, 1026)
        $graphics.DrawLine($guidePen, 96, 540, 1824, 540)
        $graphics.DrawString("TITLE SAFE 5%", $fontSmall, $grayBrush, 110, 66)
        $graphics.DrawString("CENTER 960,540", $fontSmall, $grayBrush, 978, 552)
        $graphics.DrawString("1920 x 1080 / OSD WIREFRAME", $fontSmall, $grayBrush, 110, 990)
    }

    # Heading / yaw tape: -140 to +140 degrees, with 0 degrees at center.
    $headingLeft = 510
    $headingRight = 1410
    $headingY = 142
    $graphics.DrawString("+045.6$degree", $fontLarge, $whiteBrush, [System.Drawing.RectangleF]::new(760, 70, 400, 44), $centerFormat)
    $graphics.DrawLine($whitePen, $headingLeft, $headingY, $headingRight, $headingY)

    for ($angle = -140; $angle -le 140; $angle += 10) {
        $x = [int]($headingLeft + ((($angle + 140) / 280.0) * ($headingRight - $headingLeft)))
        $isPrimary = ($angle -eq -140) -or ($angle -eq -70) -or ($angle -eq 0) -or ($angle -eq 70) -or ($angle -eq 140)
        $tickHeight = if ($angle -eq 0) { 34 } elseif ($isPrimary) { 26 } elseif (($angle % 20) -eq 0) { 16 } else { 10 }
        $graphics.DrawLine($thinPen, $x, $headingY, $x, $headingY + $tickHeight)

        if ($isPrimary) {
            $labelText = if ($angle -gt 0) { "+$angle$degree" } else { "$angle$degree" }
            $labelWidth = if ([Math]::Abs($angle) -ge 100) { 90 } else { 72 }
            $graphics.DrawString($labelText, $fontSmall, $whiteBrush, [System.Drawing.RectangleF]::new($x - ($labelWidth / 2), 184, $labelWidth, 28), $centerFormat)
        }
    }

    $yawPointerX = 1107
    $pointer = New-Object System.Drawing.Point[] 3
    $pointer[0] = [System.Drawing.Point]::new($yawPointerX - 10, 118)
    $pointer[1] = [System.Drawing.Point]::new($yawPointerX + 10, 118)
    $pointer[2] = [System.Drawing.Point]::new($yawPointerX, 137)
    $graphics.FillPolygon($whiteBrush, $pointer)

    # Pitch slider stays vertical at the right side. The numeric value is below it.
    $pitchX = 1710
    $pitchTop = 270
    $pitchCenter = 540
    $pitchBottom = 810
    $graphics.DrawLine($whitePen, $pitchX, $pitchTop, $pitchX, $pitchBottom)

    $pitchTicks = @(
        @{ Y = 270; Text = "+30$degree"; Major = $true },
        @{ Y = 338; Text = ""; Major = $false },
        @{ Y = 405; Text = "+15$degree"; Major = $true },
        @{ Y = 472; Text = ""; Major = $false },
        @{ Y = 540; Text = "0$degree"; Major = $true },
        @{ Y = 585; Text = ""; Major = $false },
        @{ Y = 630; Text = "-30$degree"; Major = $true },
        @{ Y = 675; Text = ""; Major = $false },
        @{ Y = 720; Text = "-60$degree"; Major = $true },
        @{ Y = 765; Text = ""; Major = $false },
        @{ Y = 810; Text = "-90$degree"; Major = $true }
    )
    foreach ($tick in $pitchTicks) {
        $tickWidth = if ($tick.Major) { 28 } else { 15 }
        $graphics.DrawLine($thinPen, $pitchX - $tickWidth, $tick.Y, $pitchX, $tick.Y)
        if ($tick.Text) {
            $graphics.DrawString($tick.Text, $fontSmall, $whiteBrush, [System.Drawing.RectangleF]::new($pitchX - 112, $tick.Y - 14, 70, 28), $centerFormat)
        }
    }

    # Example current pitch is -12.3 degrees, placed below the zero point.
    $pitchPointerY = 577
    $pitchPointer = New-Object System.Drawing.Point[] 3
    $pitchPointer[0] = [System.Drawing.Point]::new($pitchX - 30, $pitchPointerY - 10)
    $pitchPointer[1] = [System.Drawing.Point]::new($pitchX - 30, $pitchPointerY + 10)
    $pitchPointer[2] = [System.Drawing.Point]::new($pitchX - 5, $pitchPointerY)
    $graphics.FillPolygon($whiteBrush, $pitchPointer)
    $graphics.DrawString("-012.3$degree", $fontLarge, $whiteBrush, [System.Drawing.RectangleF]::new(1510, 842, 400, 44), $centerFormat)

    # Center crosshair with an open target area.
    $cx = 960
    $cy = 540
    $graphics.DrawLine($whitePen, $cx - 72, $cy, $cx - 18, $cy)
    $graphics.DrawLine($whitePen, $cx + 18, $cy, $cx + 72, $cy)
    $graphics.DrawLine($whitePen, $cx, $cy - 72, $cx, $cy - 18)
    $graphics.DrawLine($whitePen, $cx, $cy + 18, $cx, $cy + 72)
    $graphics.DrawEllipse($thinPen, $cx - 8, $cy - 8, 16, 16)

    # Laser ranging: label stays white, active distance is red.
    $graphics.FillEllipse($redBrush, 846, 672, 12, 12)
    $graphics.DrawString("LASER", $fontMedium, $whiteBrush, [System.Drawing.RectangleF]::new(870, 658, 96, 40), $nearFormat)
    $graphics.DrawString("125.8 m", $fontMedium, $redBrush, [System.Drawing.RectangleF]::new(972, 658, 150, 40), $nearFormat)

    # Semi-transparent watermark sample.
    $watermarkBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(110, 255, 255, 255))
    $graphics.DrawString("RK3588 OSD PREVIEW", $fontWatermark, $watermarkBrush, [System.Drawing.RectangleF]::new(1480, 996, 340, 36), $centerFormat)

    if ($Wireframe) {
        $graphics.DrawLine($guidePen, 960, 196, 960, 416)
        $graphics.DrawLine($guidePen, 1078, 540, 1655, 540)
        $graphics.DrawString("TOP ANGLE SCALE", $fontSmall, $grayBrush, 1010, 212)
        $graphics.DrawString("SIDE ANGLE SCALE", $fontSmall, $grayBrush, 1530, 890)
        $graphics.DrawString("LASER RANGE INTERFACE", $fontSmall, $grayBrush, 1110, 710)
    }

    $outputParent = Split-Path -Parent $Path
    if ($outputParent) {
        [System.IO.Directory]::CreateDirectory($outputParent) | Out-Null
    }
    $bitmap.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)

    $watermarkBrush.Dispose()
    $nearFormat.Dispose()
    $centerFormat.Dispose()
    $fontWatermark.Dispose()
    $fontLarge.Dispose()
    $fontMedium.Dispose()
    $fontSmall.Dispose()
    $grayBrush.Dispose()
    $redBrush.Dispose()
    $whiteBrush.Dispose()
    $guidePen.Dispose()
    $redPen.Dispose()
    $thinPen.Dispose()
    $whitePen.Dispose()
    $graphics.Dispose()
    $bitmap.Dispose()
}

$resolvedOutput = Join-Path (Get-Location) $OutputDirectory
New-OsdMockup -Path (Join-Path $resolvedOutput "osd_wireframe_1920x1080.png") -Wireframe $true
New-OsdMockup -Path (Join-Path $resolvedOutput "osd_black_preview_1920x1080.png") -Wireframe $false
New-OsdMockup -Path (Join-Path $resolvedOutput "osd_watermark_rgba_1920x1080.png") -Wireframe $false -Transparent $true

Write-Output (Join-Path $resolvedOutput "osd_wireframe_1920x1080.png")
Write-Output (Join-Path $resolvedOutput "osd_black_preview_1920x1080.png")
Write-Output (Join-Path $resolvedOutput "osd_watermark_rgba_1920x1080.png")
