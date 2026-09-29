<#
  noknok_leds.ps1 - drive the noknok LEDs module from Windows PowerShell.

  Opens the module's USB CDC port via a raw Win32 handle (works regardless of the
  .NET SerialPort quirks). Auto-detects the COM port by USB VID 1209 / PID 4E4E.

  Usage (space-separated values; run with the execution policy bypassed):
    powershell -ExecutionPolicy Bypass -File noknok_leds.ps1 list
    powershell -ExecutionPolicy Bypass -File noknok_leds.ps1 all 255 0 0
    powershell -ExecutionPolicy Bypass -File noknok_leds.ps1 pixel 3 0 255 0
    powershell -ExecutionPolicy Bypass -File noknok_leds.ps1 bright 64
    powershell -ExecutionPolicy Bypass -File noknok_leds.ps1 off
    powershell -ExecutionPolicy Bypass -File noknok_leds.ps1 rainbow
    powershell -ExecutionPolicy Bypass -File noknok_leds.ps1 -Port COM12 all 0 0 255

  Protocol: 0x00 off | 0x01 R G B all | 0x02 i R G B one | 0x03 B brightness | 0xF0 id
#>
param(
    [Parameter(Position = 0, ValueFromRemainingArguments = $true)] $Cmd,
    [string] $Port
)

$VID_PID = 'VID_1209&PID_4E4E'

if (-not ([System.Management.Automation.PSTypeName]'ComRaw').Type) {
Add-Type @'
using System;
using System.IO;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
public static class ComRaw {
  [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Auto)]
  static extern SafeFileHandle CreateFile(string name, uint access, uint share,
      IntPtr sa, uint disp, uint flags, IntPtr templ);
  [StructLayout(LayoutKind.Sequential)]
  struct COMMTIMEOUTS { public uint RI, RM, RC, WM, WC; }
  [DllImport("kernel32.dll", SetLastError=true)]
  static extern bool SetCommTimeouts(SafeFileHandle h, ref COMMTIMEOUTS t);
  public static FileStream Open(string port){
    var h = CreateFile(@"\\.\"+port, 0xC0000000, 0, IntPtr.Zero, 3, 0x80, IntPtr.Zero);
    if (h.IsInvalid) throw new IOException("Cannot open "+port+" (err "+Marshal.GetLastWin32Error()+")");
    // Read returns after 500 ms even if fewer bytes arrive (so reads can't hang).
    var t = new COMMTIMEOUTS(); t.RC = 500; SetCommTimeouts(h, ref t);
    return new FileStream(h, FileAccess.ReadWrite);
  }
}
'@
}

function Find-NoknokPort {
    $devs = Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue |
            Where-Object { $_.DeviceID -like "*$VID_PID*" -and $_.Name -match '\((COM\d+)\)' }
    foreach ($d in $devs) { if ($d.Name -match '\((COM\d+)\)') { return $Matches[1] } }
    return $null
}
function Clamp([int]$v){ if($v -lt 0){0}elseif($v -gt 255){255}else{$v} }

# Normalise the verb + numeric args
if (-not $Cmd) { $Cmd = @('help') }
$verb = "$($Cmd[0])".ToLower()
$nums = @(); if ($Cmd.Count -gt 1) { foreach ($a in $Cmd[1..($Cmd.Count-1)]) { $nums += [int]$a } }

if ($verb -eq 'help' -or $verb -eq '-h' -or $verb -eq '--help') {
    Get-Content $PSCommandPath | Select-Object -First 22 | ForEach-Object { $_ -replace '^[<#>]*','' }
    return
}

if ($verb -eq 'list') {
    Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '\(COM\d+\)' } |
        ForEach-Object {
            $tag = if ($_.DeviceID -like "*$VID_PID*") { '  <-- noknok LEDs' } else { '' }
            "{0,-40} {1}{2}" -f $_.Name, $_.DeviceID, $tag
        }
    return
}

if (-not $Port) {
    $Port = Find-NoknokPort
    if (-not $Port) { Write-Error "noknok LEDs not found (VID 1209/PID 4E4E). Plugged in?"; exit 1 }
    Write-Host "Using $Port"
}

$fs = [ComRaw]::Open($Port)
function Send([byte[]]$b){ $fs.Write($b,0,$b.Length); $fs.Flush() }

try {
    switch ($verb) {
        'off'    { Send 0x00 }
        'version' {
            Send (0xB1)                                   # GET_VERSION
            $buf = New-Object byte[] 4
            $n = $fs.Read($buf, 0, 4)
            if ($n -ge 4) { "noknok LEDs firmware v{0}.{1}.{2}  (protocol {3})" -f $buf[1],$buf[2],$buf[3],$buf[0] }
            else { "No version reply ($n bytes) - firmware older than v1.5?" }
        }
        'id'      {
            Send (0xF0)                                   # identity query
            $buf = New-Object byte[] 3
            $n = $fs.Read($buf, 0, 3)
            if ($n -ge 3) { "identity: {0:X2} {1:X2} type={2}" -f $buf[0],$buf[1],$buf[2] }
            else { "No identity reply ($n bytes)" }
        }
        'all'    { if ($nums.Count -lt 3) { throw "all needs: R G B  (e.g. all 255 0 0)" }
                   Send (0x01,(Clamp $nums[0]),(Clamp $nums[1]),(Clamp $nums[2])) }
        'pixel'  { if ($nums.Count -lt 4) { throw "pixel needs: index R G B  (e.g. pixel 3 0 255 0)" }
                   Send (0x02,(Clamp $nums[0]),(Clamp $nums[1]),(Clamp $nums[2]),(Clamp $nums[3])) }
        'bright' { if ($nums.Count -lt 1) { throw "bright needs: value 0-255" }
                   Send (0x03,(Clamp $nums[0])) }
        'rainbow' {
            Write-Host "Rainbow - press Ctrl-C to stop."
            $step = 0
            while ($true) {
                $frame = New-Object System.Collections.Generic.List[byte]
                $frame.Add(0x04)
                for ($i=0; $i -lt 8; $i++) {
                    $hue = (($step + $i*32) % 256)
                    $region=[int][math]::Floor($hue/43); $rem=($hue-$region*43)*6
                    $p=0; $q=[int]((200*(255-$rem))/255); $t=[int]((200*$rem)/255)
                    switch ($region) {
                        0 {$r=200;$g=$t;$b=$p} 1 {$r=$q;$g=200;$b=$p} 2 {$r=$p;$g=200;$b=$t}
                        3 {$r=$p;$g=$q;$b=200} 4 {$r=$t;$g=$p;$b=200} default {$r=200;$g=$p;$b=$q}
                    }
                    $frame.Add([byte](Clamp $r)); $frame.Add([byte](Clamp $g)); $frame.Add([byte](Clamp $b))
                }
                Send $frame.ToArray()
                Start-Sleep -Milliseconds 30
                $step = ($step + 4) % 256
            }
        }
        default  { Write-Host "Unknown command '$verb'. Try: list | all R G B | pixel i R G B | bright N | off | rainbow | version | id" }
    }
}
finally { $fs.Close() }