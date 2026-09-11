[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string]$Reference,
    [Parameter(Mandatory)] [string]$Candidate,
    [int]$Tolerance = 1
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;

public static class Ns60PngCompare {
    static byte[] Pixels(Bitmap bitmap, out int stride) {
        var rect = new Rectangle(0, 0, bitmap.Width, bitmap.Height);
        var data = bitmap.LockBits(rect, ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
        try {
            stride = data.Stride;
            var bytes = new byte[data.Stride * data.Height];
            Marshal.Copy(data.Scan0, bytes, 0, bytes.Length);
            return bytes;
        } finally {
            bitmap.UnlockBits(data);
        }
    }

    public static string Compare(string referencePath, string candidatePath, int tolerance, out bool pass) {
        using (var reference = new Bitmap(referencePath))
        using (var candidate = new Bitmap(candidatePath)) {
            if (reference.Width != candidate.Width || reference.Height != candidate.Height) {
                pass = false;
                return String.Format("Size mismatch: reference {0}x{1}, candidate {2}x{3}",
                    reference.Width, reference.Height, candidate.Width, candidate.Height);
            }
            int strideA, strideB;
            var a = Pixels(reference, out strideA);
            var b = Pixels(candidate, out strideB);
            int maxDiff = 0, firstX = -1, firstY = -1;
            long over = 0;
            for (int y = 0; y < reference.Height; ++y) {
                for (int x = 0; x < reference.Width; ++x) {
                    int ia = y * strideA + x * 4, ib = y * strideB + x * 4;
                    for (int c = 0; c < 3; ++c) {
                        int diff = Math.Abs(a[ia + c] - b[ib + c]);
                        if (diff > maxDiff) maxDiff = diff;
                        if (diff > tolerance) {
                            if (firstX < 0) { firstX = x; firstY = y; }
                            ++over;
                        }
                    }
                }
            }
            pass = over == 0;
            return String.Format("Compared {0}x{1}: max channel diff {2}, channels over tolerance {3}: {4}{5}",
                reference.Width, reference.Height, maxDiff, tolerance, over,
                firstX >= 0 ? String.Format(", first at ({0},{1})", firstX, firstY) : "");
        }
    }
}
'@

$pass = $false
$summary = [Ns60PngCompare]::Compare((Resolve-Path -LiteralPath $Reference).Path,
    (Resolve-Path -LiteralPath $Candidate).Path, $Tolerance, [ref]$pass)
Write-Output $summary
if (-not $pass) { exit 1 }
