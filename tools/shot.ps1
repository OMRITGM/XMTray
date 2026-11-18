# shot.ps1 - run XMTray in --demo mode, optionally click/scroll, save a PNG of the window, then kill it.
# Usage:  powershell -File shot.ps1 -Out C:\path\name.png [-Steps "click 120,200; wheel 400,300,-3; key 9; wait 300"]
#   coordinates are client-area DIPs (96-dpi units). key takes a virtual-key code. wheel: x,y,notches (negative = down).
param([Parameter(Mandatory)][string]$Out, [string]$Steps = "", [string]$Exe = "")
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System; using System.Runtime.InteropServices;
public static class XS {
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string c,IntPtr t);
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h,out RECT r);
 [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h,ref POINT p);
 [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h,IntPtr dc,uint f);
 [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
 [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
 [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
 public struct RECT{public int L,T,R,B;} public struct POINT{public int X,Y;}
}
'@
[XS]::SetProcessDPIAware() | Out-Null
Get-Process XMTray -ea 0 | Stop-Process -Force -Confirm:$false
if (-not $Exe) { $Exe = Join-Path (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)) "b\XMTray.exe" }
$p = Start-Process $Exe -ArgumentList '--demo' -PassThru
$h = [IntPtr]::Zero
foreach ($i in 1..40) { Start-Sleep -Milliseconds 150; $h = [XS]::FindWindowW('XMTrayFlyout', [IntPtr]::Zero); if ($h -ne [IntPtr]::Zero -and [XS]::IsWindowVisible($h)) { break } }
if ($h -eq [IntPtr]::Zero) { "window not found (exited=$($p.HasExited))"; if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force -Confirm:$false }; exit 1 }
Start-Sleep -Milliseconds 700
$s = [XS]::GetDpiForWindow($h) / 96.0
function MkLP($x, $y) { [IntPtr]((([int]($y * $s)) -shl 16) -bor (([int]($x * $s)) -band 0xFFFF)) }
foreach ($step in ($Steps -split ';')) {
  $t = $step.Trim(); if (-not $t) { continue }
  $verb, $arg = $t -split '\s+', 2; $a = @(); if ($arg) { $a = $arg -split ',' | % { [double]$_ } }
  switch ($verb) {
    'click' { $l = MkLP $a[0] $a[1]; [XS]::PostMessageW($h,0x200,[IntPtr]0,$l)|Out-Null; [XS]::PostMessageW($h,0x201,[IntPtr]1,$l)|Out-Null; [XS]::PostMessageW($h,0x202,[IntPtr]0,$l)|Out-Null; Start-Sleep -Milliseconds 350 }
    'move'  { [XS]::PostMessageW($h,0x200,[IntPtr]0,(MkLP $a[0] $a[1]))|Out-Null; Start-Sleep -Milliseconds 200 }
    'wheel' { $pt = New-Object XS+POINT; $pt.X=[int]($a[0]*$s); $pt.Y=[int]($a[1]*$s); [XS]::ClientToScreen($h,[ref]$pt)|Out-Null
              $l=[IntPtr](($pt.Y -shl 16) -bor ($pt.X -band 0xFFFF)); $n=[int][math]::Abs($a[2]); $d = if ($a[2] -lt 0) { -120 } else { 120 }
              1..$n | % { [XS]::PostMessageW($h,0x20A,[IntPtr]($d -shl 16),$l)|Out-Null }; Start-Sleep -Milliseconds 350 }
    'key'   { [XS]::PostMessageW($h,0x100,[IntPtr][int]$a[0],[IntPtr]0)|Out-Null; [XS]::PostMessageW($h,0x101,[IntPtr][int]$a[0],[IntPtr]0)|Out-Null; Start-Sleep -Milliseconds 250 }
    'wait'  { Start-Sleep -Milliseconds ([int]$a[0]) }
  }
}
$rc = New-Object XS+RECT; [XS]::GetWindowRect($h,[ref]$rc)|Out-Null; $w=$rc.R-$rc.L; $ht=$rc.B-$rc.T
$bmp = New-Object System.Drawing.Bitmap $w,$ht; $g=[System.Drawing.Graphics]::FromImage($bmp); $dc=$g.GetHdc(); [XS]::PrintWindow($h,$dc,2)|Out-Null; $g.ReleaseHdc($dc)
$bmp.Save($Out); $g.Dispose(); $bmp.Dispose()
"saved $Out ${w}x$ht scale=$s alive=$(-not $p.HasExited)"
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force -Confirm:$false }
