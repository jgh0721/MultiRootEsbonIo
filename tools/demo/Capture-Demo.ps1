<#
.SYNOPSIS
    docs/demo 워크스페이스를 열어 README 용 스크린샷과 APNG 를 만든다.

.DESCRIPTION
    화면을 실제로 점유한다. 도는 동안 다른 창을 띄우거나 마우스를 쓰면 그대로 찍힌다.

    결정성의 요령은 세션 파일이다. 앱은 인자 없이 실행되면
    <워크스페이스>/.multiroot/workspace.json 을 읽어 열 문서·캐럿·스플리터 배치를
    그대로 되살린다(MainWindow::restoreLastSession). 그래서 마우스로 탭을 열거나
    스플리터를 끌 필요가 없고, 같은 그림이 매번 나온다.

    준비 완료 판정은 MRST_PHASE_TRACE 가 남기는 phase.ready.end 를 본다.

.EXAMPLE
    .\Capture-Demo.ps1 -Scene overview -Theme dark -Lang ko
.EXAMPLE
    .\Capture-Demo.ps1 -All
#>
[CmdletBinding()]
param(
    [string]   $Scene = '',
    [ValidateSet('light', 'dark')] [string] $Theme = 'dark',
    [ValidateSet('ko', 'en')]      [string] $Lang  = 'ko',
    [switch]   $All,
    [switch]   $Shots,
    [switch]   $Videos,
    [string]   $Exe = '',
    [string]   $Ffmpeg = '',
    [int]      $Width  = 1280,
    [int]      $Height = 1024,
    # 프리뷰 첫 빌드와 Esbonio 콜드 스타트를 기다리는 시간(초).
    # 구문 강조의 3-state 는 LSP 가 directive 목록을 준 뒤에야 초록/빨강으로 확정된다.
    [int]      $SettleSeconds = 25
)

$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
Set-StrictMode -Version 2.0

# 녹화 프로세스 표식. StrictMode 아래에서는 미리 만들어 두어야 읽을 수 있다.
$script:__recorder = $null

$RepoRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$DemoRoot = Join-Path $RepoRoot 'docs\demo'
$ImageDir = Join-Path $RepoRoot 'docs\images'
$MediaDir = Join-Path $RepoRoot 'docs\media'

if ([string]::IsNullOrEmpty($Exe)) {
    $candidates = @(
        'cmake-build-relwithdebinfo-msvc2022\MultiRoot-reST Editor.exe',
        '_build\RelWithDebInfo\RelWithDebInfo\MultiRoot-reST Editor.exe',
        'cmake-build-debug-msvc2022\MultiRoot-reST Editor.exe'
    ) | ForEach-Object { Join-Path $RepoRoot $_ } | Where-Object { Test-Path $_ }
    if ($candidates.Count -eq 0) { throw '빌드된 실행 파일을 찾지 못했습니다. -Exe 로 지정하세요.' }
    $Exe = $candidates[0]
}
if ([string]::IsNullOrEmpty($Ffmpeg)) {
    $cmd = Get-Command ffmpeg -ErrorAction SilentlyContinue
    if ($null -eq $cmd) { throw 'ffmpeg 를 찾지 못했습니다. -Ffmpeg 로 지정하세요.' }
    $Ffmpeg = $cmd.Source
}

$ExeDir  = Split-Path $Exe -Parent
$IniPath = Join-Path $ExeDir 'MultiRoot-reST Editor.ini'
$IniBak  = Join-Path $ExeDir 'MultiRoot-reST Editor.ini.capture-backup'
# 핫 엑시트 백업이 남아 있으면 이전 실행의 미저장 내용이 복원되어 화면을 오염시킨다.
$HotExit = Join-Path $env:LOCALAPPDATA 'myHouse\MultiRoot reST Editor\TextHotExit'

# ---------------------------------------------------------------- Win32

if (-not ('MrstWin32' -as [type])) {
    Add-Type @'
using System;
using System.Runtime.InteropServices;
public class MrstWin32 {
    public delegate bool EnumProc(IntPtr hWnd, IntPtr lParam);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }

    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lParam);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern int  GetWindowTextLength(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint flags, int dx, int dy, int data, UIntPtr extra);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
    [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr h, int attr, out RECT r, int size);
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr ctx);

    // 이 프로세스가 DPI 를 모르면 SetWindowPos 에 준 크기가 시스템 배율로 가상화되고,
    // DWM 이 돌려주는 실제 경계는 물리 픽셀이라 두 좌표계가 섞인다. 요청한 크기와
    // 찍히는 크기가 어긋나는 원인이 그것이므로, 시작할 때 물리 픽셀로 못 박는다.
    public static readonly IntPtr DPI_PER_MONITOR_AWARE_V2 = new IntPtr(-4);

    public const uint SWP_NOZORDER = 0x0004, SWP_SHOWWINDOW = 0x0040;
    public const uint WM_CLOSE = 0x0010;
    public const uint MOUSEEVENTF_LEFTDOWN = 0x0002, MOUSEEVENTF_LEFTUP = 0x0004;
    public const uint MOUSEEVENTF_WHEEL = 0x0800;
    public const uint KEYEVENTF_KEYUP = 0x0002;
    // 창의 실제 시각적 경계. GetWindowRect 는 Windows 10 이후 보이지 않는
    // 리사이즈 테두리까지 포함해 좌우로 몇 픽셀씩 넓게 나온다.
    public const int DWMWA_EXTENDED_FRAME_BOUNDS = 9;
}
'@
}
Add-Type -AssemblyName System.Windows.Forms
[void][MrstWin32]::SetProcessDpiAwarenessContext([MrstWin32]::DPI_PER_MONITOR_AWARE_V2)

function Get-MainWindow([int] $ProcessId) {
    $found = [IntPtr]::Zero
    $cb = [MrstWin32+EnumProc] {
        param($h, $l)
        $pid2 = 0
        [void][MrstWin32]::GetWindowThreadProcessId($h, [ref] $pid2)
        if ($pid2 -eq $ProcessId -and [MrstWin32]::IsWindowVisible($h) `
                -and [MrstWin32]::GetWindowTextLength($h) -gt 0) {
            $script:__foundHwnd = $h
            return $false
        }
        return $true
    }
    $script:__foundHwnd = [IntPtr]::Zero
    [void][MrstWin32]::EnumWindows($cb, [IntPtr]::Zero)
    return $script:__foundHwnd
}

function Get-VisualRect([IntPtr] $Hwnd) {
    $r = New-Object MrstWin32+RECT
    $size = [System.Runtime.InteropServices.Marshal]::SizeOf([type]'MrstWin32+RECT')
    if ([MrstWin32]::DwmGetWindowAttribute($Hwnd, [MrstWin32]::DWMWA_EXTENDED_FRAME_BOUNDS, [ref] $r, $size) -ne 0) {
        [void][MrstWin32]::GetWindowRect($Hwnd, [ref] $r)
    }
    return $r
}

function Invoke-Click([int] $X, [int] $Y) {
    [void][MrstWin32]::SetCursorPos($X, $Y)
    Start-Sleep -Milliseconds 120
    [MrstWin32]::mouse_event([MrstWin32]::MOUSEEVENTF_LEFTDOWN, 0, 0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 40
    [MrstWin32]::mouse_event([MrstWin32]::MOUSEEVENTF_LEFTUP, 0, 0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 200
}

function Invoke-Wheel([int] $X, [int] $Y, [int] $Notches) {
    [void][MrstWin32]::SetCursorPos($X, $Y)
    Start-Sleep -Milliseconds 60
    [MrstWin32]::mouse_event([MrstWin32]::MOUSEEVENTF_WHEEL, 0, 0, (120 * $Notches), [UIntPtr]::Zero)
}

function Send-Keys([string] $Keys, [int] $DelayMs = 250) {
    [System.Windows.Forms.SendKeys]::SendWait($Keys)
    Start-Sleep -Milliseconds $DelayMs
}

# SendKeys 는 Ctrl+Space 를 표현하지 못한다. 조합키는 직접 만든다.
function Send-CtrlSpace {
    [MrstWin32]::keybd_event(0x11, 0, 0, [UIntPtr]::Zero)             # VK_CONTROL down
    [MrstWin32]::keybd_event(0x20, 0, 0, [UIntPtr]::Zero)             # VK_SPACE down
    Start-Sleep -Milliseconds 40
    [MrstWin32]::keybd_event(0x20, 0, [MrstWin32]::KEYEVENTF_KEYUP, [UIntPtr]::Zero)
    [MrstWin32]::keybd_event(0x11, 0, [MrstWin32]::KEYEVENTF_KEYUP, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 600
}

function Send-Text([string] $Text, [int] $PerCharMs = 90) {
    foreach ($ch in $Text.ToCharArray()) {
        $escaped = $ch.ToString()
        if ('+^%~(){}[]'.Contains($escaped)) { $escaped = '{' + $escaped + '}' }
        [System.Windows.Forms.SendKeys]::SendWait($escaped)
        Start-Sleep -Milliseconds $PerCharMs
    }
}

# ---------------------------------------------------------------- 설정/세션

# Set-Content -Encoding utf8 은 Windows PowerShell 에서 BOM 을 붙인다. Qt 의
# QSettings 도 QJsonDocument::fromJson 도 BOM 을 반긴 적이 없으므로 직접 쓴다.
function Write-Utf8NoBom([string] $Path, [string] $Text) {
    [System.IO.File]::WriteAllText($Path, $Text, (New-Object System.Text.UTF8Encoding($false)))
}

function Set-DemoIni([string] $ThemeName, [string] $LangCode) {
    $themeValue = 0
    if ($ThemeName -eq 'dark') { $themeValue = 1 }
    $root = $DemoRoot.Replace('\', '/')
    $text = @"
[General]
language=$LangCode
theme=$themeValue

[PythonEnv]
useExternalUv=false
autoBootstrap=true

[textView]
fontFamily=Cascadia Mono
fontSize=12
lineSpacing=1.15
showIndentationGuides=true
indentGuideStyle=1
showCodeFolding=true
braceHighlight=true
hotExitEnabled=false
wordWrapMode=2
wrapIndentMode=1
changeHistoryMode=1
tabWidth=4
useTabs=false
saveEncoding=UTF-8
saveBomMode=2

[preview]
applyUnsavedEdits=true
unsavedEditMaxReadMs=2000
allowRemoteContent=true

[update]
checkIntervalDays=0

[workspace]
lastRoot=$root
"@
    Write-Utf8NoBom $IniPath $text
}

function Set-DemoSession([string[]] $Documents, [int] $ActiveIndex,
                         [int[]] $CaretLines, [int[]] $FirstVisibleLines,
                         [int[]] $SideSizes, [int[]] $ContentSizes, [int[]] $PreviewSizes) {
    $dir = Join-Path $DemoRoot '.multiroot'
    if (-not (Test-Path $dir)) { [void] (New-Item -ItemType Directory -Path $dir) }

    $docs = @()
    for ($i = 0; $i -lt $Documents.Count; $i++) {
        $caret = 1; $first = 1
        if ($i -lt $CaretLines.Count)        { $caret = $CaretLines[$i] }
        if ($i -lt $FirstVisibleLines.Count) { $first = $FirstVisibleLines[$i] }
        $docs += [ordered]@{
            path             = (Join-Path $DemoRoot $Documents[$i]).Replace('\', '/')
            caretLine        = $caret
            caretColumn      = 1
            firstVisibleLine = $first
        }
    }
    $session = [ordered]@{
        schema               = 1
        workspaceRoot        = $DemoRoot.Replace('\', '/')
        documents            = $docs
        activeIndex          = $ActiveIndex
        sideSplitterSizes    = $SideSizes
        contentSplitterSizes = $ContentSizes
        previewSplitterSizes = $PreviewSizes
    }
    $json = $session | ConvertTo-Json -Depth 6
    Write-Utf8NoBom (Join-Path $dir 'workspace.json') $json
}

# ---------------------------------------------------------------- 앱 기동/종료

function Start-Demo {
    if (Test-Path $HotExit) { Remove-Item -LiteralPath $HotExit -Recurse -Force -ErrorAction SilentlyContinue }

    $trace = Join-Path $env:TEMP ('mrst-capture-{0}.trace' -f [guid]::NewGuid().ToString('N'))
    $env:MRST_PHASE_TRACE = $trace
    # 인자를 주면 세션 복원 경로를 타지 않는다. 반드시 인자 없이 띄운다.
    $proc = Start-Process -FilePath $Exe -PassThru
    $env:MRST_PHASE_TRACE = $null

    $deadline = (Get-Date).AddSeconds(90)
    $ready = $false
    while ((Get-Date) -lt $deadline) {
        if (Test-Path $trace) {
            if ((Get-Content -LiteralPath $trace -Raw -ErrorAction SilentlyContinue) -match 'phase\.ready\.end') {
                $ready = $true; break
            }
        }
        Start-Sleep -Milliseconds 400
    }
    if (-not $ready) { Write-Warning 'phase.ready.end 를 보지 못했습니다. 그대로 진행합니다.' }

    $hwnd = [IntPtr]::Zero
    $deadline = (Get-Date).AddSeconds(30)
    while ((Get-Date) -lt $deadline -and $hwnd -eq [IntPtr]::Zero) {
        $hwnd = Get-MainWindow $proc.Id
        if ($hwnd -eq [IntPtr]::Zero) { Start-Sleep -Milliseconds 300 }
    }
    if ($hwnd -eq [IntPtr]::Zero) { throw '앱 창을 찾지 못했습니다.' }

    # 복원된 창이 보조 모니터에 있으면 좌표 계산이 통째로 어긋난다. 주 모니터로 끌어온다.
    [void][MrstWin32]::ShowWindow($hwnd, 9)          # SW_RESTORE
    [void][MrstWin32]::SetWindowPos($hwnd, [IntPtr]::Zero, 40, 40, $Width, $Height,
            [MrstWin32]::SWP_NOZORDER -bor [MrstWin32]::SWP_SHOWWINDOW)
    Start-Sleep -Milliseconds 500
    [void][MrstWin32]::SetForegroundWindow($hwnd)

    Write-Host ("  기동 완료, 안정화 대기 {0}초..." -f $SettleSeconds)
    Start-Sleep -Seconds $SettleSeconds

    # 탭이 열리는 동안 레이아웃이 최소 폭을 다시 요구하면서 창이 커진다. 그래서 크기는
    # 안정화가 끝난 뒤에 한 번 더 못 박는다. 여기서도 밀린다면 그것이 이 창의 최소 크기다.
    [void][MrstWin32]::SetWindowPos($hwnd, [IntPtr]::Zero, 40, 40, $Width, $Height,
            [MrstWin32]::SWP_NOZORDER -bor [MrstWin32]::SWP_SHOWWINDOW)
    Start-Sleep -Milliseconds 1200
    [void][MrstWin32]::SetForegroundWindow($hwnd)
    Start-Sleep -Milliseconds 400

    $rect = Get-VisualRect $hwnd
    $raw = New-Object MrstWin32+RECT
    [void][MrstWin32]::GetWindowRect($hwnd, [ref] $raw)
    Write-Host ("  창 요청 {0}x{1} / GetWindowRect {2}x{3} / 실제 경계 {4}x{5} @({6},{7})" -f `
        $Width, $Height, ($raw.Right - $raw.Left), ($raw.Bottom - $raw.Top),
        ($rect.Right - $rect.Left), ($rect.Bottom - $rect.Top), $rect.Left, $rect.Top)
    return [pscustomobject]@{
        Process = $proc; Hwnd = $hwnd; Trace = $trace
        X = $rect.Left; Y = $rect.Top
        W = $rect.Right - $rect.Left; H = $rect.Bottom - $rect.Top
    }
}

function Stop-Demo($App) {
    [void][MrstWin32]::PostMessage($App.Hwnd, [MrstWin32]::WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero)
    if (-not $App.Process.WaitForExit(15000)) {
        Write-Warning '창이 닫히지 않아 강제 종료합니다.'
        Stop-Process -Id $App.Process.Id -Force -ErrorAction SilentlyContinue
    }
    Remove-Item -LiteralPath $App.Trace -Force -ErrorAction SilentlyContinue
}

# ---------------------------------------------------------------- 캡처

function Get-GrabArgs($App) {
    # gdigrab 은 짝수 크기를 좋아한다. 자르는 편이 늘리는 것보다 안전하다.
    $w = $App.W - ($App.W % 2)
    $h = $App.H - ($App.H % 2)
    return @('-f', 'gdigrab', '-offset_x', $App.X, '-offset_y', $App.Y,
             '-video_size', ("{0}x{1}" -f $w, $h), '-i', 'desktop')
}

function Save-Shot($App, [string] $Path) {
    if (Test-Path $Path) { Remove-Item -LiteralPath $Path -Force }
    $grab = @('-hide_banner', '-loglevel', 'error', '-draw_mouse', '0', '-framerate', '1') + (Get-GrabArgs $App)
    & $Ffmpeg @grab -frames:v 1 -y $Path
    if (-not (Test-Path $Path)) { throw ("캡처 실패: {0}" -f $Path) }
    Write-Host ("  -> {0} ({1:N0} KB)" -f (Split-Path $Path -Leaf), ((Get-Item $Path).Length / 1KB))
}

# 녹화는 백그라운드로 시작하고, 조작이 끝나면 q 를 보내 정상 종료시킨다.
function Start-Recording($App, [string] $Path, [int] $Fps = 10) {
    if (Test-Path $Path) { Remove-Item -LiteralPath $Path -Force }
    $grab = @('-hide_banner', '-loglevel', 'error', '-draw_mouse', '0', '-framerate', $Fps) + (Get-GrabArgs $App)
    $args = $grab + @('-c:v', 'apng', '-plays', '0', '-f', 'apng', '-y', $Path)
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Ffmpeg
    $psi.Arguments = ($args | ForEach-Object { if ("$_" -match '\s') { '"' + $_ + '"' } else { "$_" } }) -join ' '
    $psi.UseShellExecute = $false
    $psi.RedirectStandardInput = $true
    # 씬이 중간에 실패하면 이 프로세스가 남아 출력 파일을 계속 잡는다. 다음 실행이
    # 그 파일을 지우지 못해 엉뚱한 곳에서 멈추므로, 정리할 수 있게 밖에 걸어 둔다.
    $script:__recorder = [System.Diagnostics.Process]::Start($psi)
    return $script:__recorder
}

function Stop-StrayRecorder {
    if ($null -ne $script:__recorder -and -not $script:__recorder.HasExited) {
        Write-Warning '녹화 프로세스가 남아 있어 정리합니다.'
        try { $script:__recorder.Kill() } catch {}
    }
    $script:__recorder = $null
}

function Stop-Recording($Recorder, [string] $Path) {
    $Recorder.StandardInput.Write('q')
    $Recorder.StandardInput.Flush()
    if (-not $Recorder.WaitForExit(30000)) { $Recorder.Kill() }
    $script:__recorder = $null
    if (Test-Path $Path) {
        Write-Host ("  -> {0} ({1:N1} MB)" -f (Split-Path $Path -Leaf), ((Get-Item $Path).Length / 1MB))
    } else {
        Write-Warning ("녹화 실패: {0}" -f $Path)
    }
}

# ---------------------------------------------------------------- 씬

# 1280x1024 기준 배치. setSizes 는 비율로 맞춰지므로 정확할 필요는 없다.
$SideSizes    = @(215, 790)
$ContentSizes = @(600, 140)
$PreviewSizes = @(395, 395)

$DocKo = @('사용자 안내서/source/둘러보기.rst', 'handbook/source/directives-tour.rst',
           'api-reference/core.rst', 'release-notes/v2/source/index.rst', 'TODO.rst')
$DocEn = @('handbook/source/directives-tour.rst', '사용자 안내서/source/둘러보기.rst',
           'api-reference/core.rst', 'release-notes/v2/source/index.rst', 'TODO.rst')

function Get-SceneDocs([string] $LangCode) {
    if ($LangCode -eq 'ko') { return $DocKo } else { return $DocEn }
}

# 하단 패널의 탭(진단·로그·검색)은 세션에 저장되지 않는다. 기본값인 진단은 데모
# 워크스페이스가 깨끗해서 늘 비어 있으므로, 내용이 있는 로그로 옮겨 놓고 찍는다.
# 좌표는 좌측 패널 폭과 하단 패널 높이에서 나온 실측값이다.
# 탭 이름의 길이가 언어마다 달라 두 번째 탭의 x 도 달라진다("진단" 대 "Diagnostics").
function Select-LogTab($App, [string] $LangCode) {
    $offset = 87
    if ($LangCode -eq 'en') { $offset = 135 }
    $x = $App.X + [int]($SideSizes[0] * 1.25) + $offset
    $y = $App.Y + $App.H - 205
    Invoke-Click $x $y
    Start-Sleep -Milliseconds 400
}

function Invoke-SceneOverview($App, [string] $ThemeName, [string] $LangCode) {
    Select-LogTab $App $LangCode
    Save-Shot $App (Join-Path $ImageDir ("overview-{0}-{1}.png" -f $ThemeName, $LangCode))
}

function Invoke-SceneVirtual($App, [string] $ThemeName, [string] $LangCode) {
    # 로그에 "가상 프로젝트 생성" 이 찍힌다. 이 씬의 요점이 그 한 줄이다.
    Select-LogTab $App $LangCode
    Save-Shot $App (Join-Path $ImageDir ("virtual-project-{0}-{1}.png" -f $ThemeName, $LangCode))
}

function Invoke-SceneMultiroot($App, [string] $ThemeName, [string] $LangCode) {
    Select-LogTab $App $LangCode
    Save-Shot $App (Join-Path $ImageDir ("multiroot-{0}-{1}.png" -f $ThemeName, $LangCode))
}

# 편집기 안의 한 지점. 스플리터 값은 논리 픽셀이므로 배율을 곱해 화면 좌표로 옮긴다.
function Get-EditorPoint($App) {
    $x = $App.X + [int]($SideSizes[0] * 1.25) + [int]($PreviewSizes[0] * 1.25 * 0.45)
    # 위쪽 1/3 을 찍는다. 캐럿이 창 아래쪽에 있으면 자동완성 팝업이 창 밖으로 잘린다.
    $y = $App.Y + [int]($App.H * 0.30)
    return , @($x, $y)
}

function Get-PreviewPoint($App) {
    $x = $App.X + [int]($SideSizes[0] * 1.25) + [int]($PreviewSizes[0] * 1.25) + [int]($PreviewSizes[1] * 1.25 * 0.5)
    $y = $App.Y + [int]($App.H * 0.44)
    return , @($x, $y)
}

function Invoke-SceneCompletion($App, [string] $ThemeName, [string] $LangCode, [switch] $Record) {
    $pt = Get-EditorPoint $App
    Invoke-Click $pt[0] $pt[1]
    Start-Sleep -Milliseconds 400

    $recorder = $null
    $video = Join-Path $MediaDir ("completion-{0}.apng" -f $LangCode)
    if ($Record) { $recorder = Start-Recording $App $video; Start-Sleep -Milliseconds 800 }

    # 클릭한 줄의 끝에서 새 블록을 연다. 클릭 지점이 화면 위쪽이라 팝업이 온전히 펼쳐진다.
    Send-Keys '{END}' 400
    Send-Keys '{ENTER}{ENTER}' 500

    Send-Text '.. im'                       # 자동 트리거로 팝업이 뜬다
    Start-Sleep -Milliseconds 900
    if (-not $Record) {
        Save-Shot $App (Join-Path $ImageDir ("completion-directive-{0}-{1}.png" -f $ThemeName, $LangCode))
    }

    Send-Text 'age:: '                      # 경로 슬롯으로 넘어간다
    Send-CtrlSpace
    Start-Sleep -Milliseconds 1200
    # 이름만 치면 워크스페이스 전역에서 찾아 준다. 첫 후보가 이미지면 상세 패널에
    # 미리보기와 형식·치수·크기가 뜨는데, 그 화면이 이 기능의 요점이다.
    Send-Text 'orca' 160
    Start-Sleep -Milliseconds 2200
    if (-not $Record) {
        Save-Shot $App (Join-Path $ImageDir ("completion-path-{0}-{1}.png" -f $ThemeName, $LangCode))
    }

    if ($Record) { Stop-Recording $recorder $video }

    Send-Keys '{ESC}' 300
    # 편집 내용은 버린다. 저장하지 않고 되돌린다.
    for ($i = 0; $i -lt 20; $i++) { Send-Keys '^z' 90 }
}

function Invoke-SceneScrollSync($App, [string] $LangCode) {
    $pt = Get-EditorPoint $App
    Invoke-Click $pt[0] $pt[1]
    Start-Sleep -Milliseconds 400

    $video = Join-Path $MediaDir ("scroll-sync-{0}.apng" -f $LangCode)
    $recorder = Start-Recording $App $video
    Start-Sleep -Milliseconds 800

    for ($i = 0; $i -lt 14; $i++) { Invoke-Wheel $pt[0] $pt[1] -3; Start-Sleep -Milliseconds 260 }
    Start-Sleep -Milliseconds 700
    # 프리뷰 쪽에서 반대 방향으로
    $px = $App.X + 300 + 480 + [int](480 * 0.5)
    for ($i = 0; $i -lt 10; $i++) { Invoke-Wheel $px $pt[1] 3; Start-Sleep -Milliseconds 260 }
    Start-Sleep -Milliseconds 800

    Stop-Recording $recorder $video
}

function Invoke-SceneTabs($App, [string] $LangCode) {
    # Ctrl+Tab 은 이 창에서 탭을 넘기지 않는다(찍어 보니 109 프레임이 전부 같은 그림이었다).
    # 탭을 직접 누른다. x 는 파일 이름 길이에서 나오므로 언어마다 다르다.
    $tabsKo = @(345, 510, 645)
    $tabsEn = @(350, 510, 620)
    $tabs = $tabsKo
    if ($LangCode -eq 'en') { $tabs = $tabsEn }
    $tabY = $App.Y + 140

    $video = Join-Path $MediaDir ("multiroot-{0}.apng" -f $LangCode)
    $recorder = Start-Recording $App $video
    Start-Sleep -Milliseconds 1000

    # 탭을 옮기면 소속 프로젝트가 바뀌고, 프리뷰가 그 프로젝트의 테마로 다시 그려진다.
    foreach ($x in $tabs) {
        Invoke-Click ($App.X + $x) $tabY
        Start-Sleep -Milliseconds 2800
    }
    Invoke-Click ($App.X + $tabs[0]) $tabY
    Start-Sleep -Milliseconds 2000
    Stop-Recording $recorder $video
}

# ---------------------------------------------------------------- 실행

function Invoke-Capture([string] $SceneName, [string] $ThemeName, [string] $LangCode) {
    Write-Host ("[{0}] theme={1} lang={2}" -f $SceneName, $ThemeName, $LangCode) -ForegroundColor Cyan

    $docs = Get-SceneDocs $LangCode
    $active = 0
    $carets = @(40, 40, 18, 18, 1)
    $firsts = @(32, 32, 1, 1, 1)

    switch ($SceneName) {
        'virtual-project' { $active = 4 }
        'multiroot'       { $active = 2 }
        default           { $active = 0 }
    }

    Set-DemoIni $ThemeName $LangCode
    Set-DemoSession $docs $active $carets $firsts $SideSizes $ContentSizes $PreviewSizes

    $app = Start-Demo
    try {
        switch ($SceneName) {
            'overview'        { Invoke-SceneOverview  $app $ThemeName $LangCode }
            'virtual-project' { Invoke-SceneVirtual   $app $ThemeName $LangCode }
            'multiroot'       { Invoke-SceneMultiroot $app $ThemeName $LangCode }
            'completion'      { Invoke-SceneCompletion $app $ThemeName $LangCode }
            'video-completion' { Invoke-SceneCompletion $app $ThemeName $LangCode -Record }
            'video-scroll'    { Invoke-SceneScrollSync $app $LangCode }
            'video-tabs'      { Invoke-SceneTabs       $app $LangCode }
            default           { throw ("알 수 없는 씬: {0}" -f $SceneName) }
        }
    }
    finally {
        Stop-StrayRecorder
        Stop-Demo $app
    }
}

foreach ($d in @($ImageDir, $MediaDir)) {
    if (-not (Test-Path $d)) { [void] (New-Item -ItemType Directory -Path $d) }
}

Write-Host ''
Write-Host '화면을 그대로 찍습니다. 끝날 때까지 다른 창을 띄우거나 마우스를 쓰지 마세요.' -ForegroundColor Yellow
Write-Host ("실행 파일 : {0}" -f $Exe)
Write-Host ("ffmpeg    : {0}" -f $Ffmpeg)
Write-Host ''

if (Test-Path $IniPath) { Copy-Item -LiteralPath $IniPath -Destination $IniBak -Force }
try {
    $jobs = @()
    if ($All -or $Shots) {
        $jobs += @(
            @{ s = 'overview';        t = 'light'; l = 'ko' }, @{ s = 'overview';        t = 'dark';  l = 'ko' },
            @{ s = 'overview';        t = 'light'; l = 'en' }, @{ s = 'overview';        t = 'dark';  l = 'en' },
            @{ s = 'multiroot';       t = 'dark';  l = 'ko' }, @{ s = 'multiroot';       t = 'dark';  l = 'en' },
            @{ s = 'virtual-project'; t = 'light'; l = 'ko' }, @{ s = 'virtual-project'; t = 'light'; l = 'en' },
            @{ s = 'completion';      t = 'dark';  l = 'ko' }, @{ s = 'completion';      t = 'dark';  l = 'en' }
        )
    }
    if ($All -or $Videos) {
        $jobs += @(
            @{ s = 'video-tabs';       t = 'dark'; l = 'ko' }, @{ s = 'video-tabs';       t = 'dark'; l = 'en' },
            @{ s = 'video-scroll';     t = 'dark'; l = 'ko' }, @{ s = 'video-scroll';     t = 'dark'; l = 'en' },
            @{ s = 'video-completion'; t = 'dark'; l = 'ko' }, @{ s = 'video-completion'; t = 'dark'; l = 'en' }
        )
    }
    if ($jobs.Count -eq 0) {
        if ([string]::IsNullOrEmpty($Scene)) { throw '-Scene 이나 -All / -Shots / -Videos 중 하나가 필요합니다.' }
        $jobs = @(@{ s = $Scene; t = $Theme; l = $Lang })
    }

    foreach ($job in $jobs) { Invoke-Capture $job.s $job.t $job.l }
}
finally {
    if (Test-Path $IniBak) {
        Move-Item -LiteralPath $IniBak -Destination $IniPath -Force
        Write-Host '설정 파일을 원래대로 되돌렸습니다.'
    }
}

Write-Host ''
Write-Host '완료.' -ForegroundColor Green
