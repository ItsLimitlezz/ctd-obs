<#
.SYNOPSIS
  Spike S1 check: re-apply the ACTIVE scene collection from a staged file through ctd_show_apply,
  and confirm CTD OBS did not save its in-memory state over the staged file first.

.DESCRIPTION
  Needs a CTD OBS build from branch ctd/spike-s1, running, with obs-websocket enabled.
  Talks to the "ctd-obs" obs-websocket vendor, which forwards to the global procs
  ctd_show_apply / ctd_show_refresh.

  Steps:
    1. Read the active scene collection file and snapshot it as the staged file (in %TEMP%).
    2. Create a probe scene in OBS. It exists only in OBS's memory and in OBS's own saves, never in the staged file.
    3. Call show_apply with the staged file. Its "name" is the active collection's, so this is a re-apply.
    4. Pass if the probe scene is gone from OBS and from the collection file on disk.
       If OBS had saved the active collection over the staged file first, the probe scene would come back.
    5. Call show_refresh.
    6. Optional (-TestGuard): with a recording running, show_apply must refuse with "outputs active".

  Leaves the collection as it was before the probe scene. Makes no network calls except to the local OBS.

.EXAMPLE
  pwsh -File test\show-profiles\s1-proto.ps1 -Password (Read-Host -AsSecureString)
#>
param(
    [string]$WsHost = "localhost",
    [int]$Port = 4455,
    [SecureString]$Password,
    [string]$ConfigRoot = (Join-Path $env:APPDATA "ctd-obs"),
    [switch]$TestGuard
)

$ErrorActionPreference = "Stop"
$script:failures = 0

function Pass($msg) { Write-Host "PASS  $msg" -ForegroundColor Green }
function Fail($msg) { Write-Host "FAIL  $msg" -ForegroundColor Red; $script:failures++ }
function Info($msg) { Write-Host "      $msg" }

# ---------------------------------------------------------------- obs-websocket v5 client

$ws = [System.Net.WebSockets.ClientWebSocket]::new()
$ct = [Threading.CancellationToken]::None

function Receive-Json {
    $buffer = [byte[]]::new(1MB)
    $stream = [IO.MemoryStream]::new()
    do {
        $seg = [ArraySegment[byte]]::new($buffer)
        $r = $ws.ReceiveAsync($seg, $ct).GetAwaiter().GetResult()
        $stream.Write($buffer, 0, $r.Count)
    } while (-not $r.EndOfMessage)
    [Text.Encoding]::UTF8.GetString($stream.ToArray()) | ConvertFrom-Json
}

function Send-Json($obj) {
    $bytes = [Text.Encoding]::UTF8.GetBytes(($obj | ConvertTo-Json -Depth 20 -Compress))
    $ws.SendAsync([ArraySegment[byte]]::new($bytes), 'Text', $true, $ct).GetAwaiter().GetResult()
}

function Get-Sha256B64([string]$s) {
    $sha = [Security.Cryptography.SHA256]::Create()
    [Convert]::ToBase64String($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($s)))
}

function Connect-Obs {
    $ws.ConnectAsync([Uri]"ws://$($WsHost):$Port", $ct).GetAwaiter().GetResult()
    $hello = Receive-Json
    $identify = @{ op = 1; d = @{ rpcVersion = 1; eventSubscriptions = 0 } }
    if ($hello.d.authentication) {
        if (-not $Password) { throw "OBS asks for a websocket password: pass -Password" }
        $plain = [Runtime.InteropServices.Marshal]::PtrToStringBSTR(
            [Runtime.InteropServices.Marshal]::SecureStringToBSTR($Password))
        $secret = Get-Sha256B64 ($plain + $hello.d.authentication.salt)
        $identify.d.authentication = Get-Sha256B64 ($secret + $hello.d.authentication.challenge)
    }
    Send-Json $identify
    $identified = Receive-Json
    if ($identified.op -ne 2) { throw "obs-websocket identify failed" }
}

function Invoke-Obs([string]$type, $data = @{}) {
    $id = [guid]::NewGuid().ToString()
    Send-Json @{ op = 6; d = @{ requestType = $type; requestId = $id; requestData = $data } }
    while ($true) {
        $msg = Receive-Json
        if ($msg.op -eq 7 -and $msg.d.requestId -eq $id) {
            if (-not $msg.d.requestStatus.result) {
                throw "$type failed: $($msg.d.requestStatus.code) $($msg.d.requestStatus.comment)"
            }
            return $msg.d.responseData
        }
    }
}

function Invoke-Ctd([string]$request, $data = @{}) {
    $r = Invoke-Obs "CallVendorRequest" @{ vendorName = "ctd-obs"; requestType = $request; requestData = $data }
    $r.responseData
}

# ---------------------------------------------------------------- test

Connect-Obs
Info "Connected to obs-websocket on port $Port"

$collections = Invoke-Obs "GetSceneCollectionList"
$active = $collections.currentSceneCollectionName
Info "Active scene collection: $active"

$scenesDir = Join-Path $ConfigRoot "basic\scenes"
$activeFile = Get-ChildItem $scenesDir -Filter *.json | Where-Object {
    (Get-Content $_.FullName -Raw | ConvertFrom-Json).name -eq $active
} | Select-Object -First 1
if (-not $activeFile) { throw "No file in $scenesDir has name '$active'" }
Info "Active collection file: $($activeFile.Name)"

# 1. Stage a snapshot of the collection as it is now.
$stageDir = Join-Path $env:TEMP "ctd-s1-proto"
New-Item -ItemType Directory -Force $stageDir | Out-Null
$staged = Join-Path $stageDir $activeFile.Name
Copy-Item $activeFile.FullName $staged -Force
$stagedJson = Get-Content $staged -Raw | ConvertFrom-Json
$stagedSceneCount = @($stagedJson.scene_order).Count

# 2. Probe scene: only in OBS, never in the staged file.
$probe = "S1 Probe " + [guid]::NewGuid().ToString().Substring(0, 8)
Invoke-Obs "CreateScene" @{ sceneName = $probe } | Out-Null
Start-Sleep -Seconds 2   # let OBS run its own queued save, as in normal use
Info "Created probe scene '$probe'"

# 3. Re-apply the active collection from the staged file.
$apply = Invoke-Ctd "show_apply" @{ collection = $staged; profile = "" }
if ($apply.success) { Pass "show_apply returned success" } else { Fail "show_apply: $($apply.error)" }
if ($apply.restart_required) { Info "restart_required = true (unexpected with no profile)" }

# 4. Check memory and disk.
$current = (Invoke-Obs "GetSceneCollectionList").currentSceneCollectionName
if ($current -eq $active) { Pass "active collection is still '$active'" } else { Fail "active collection is '$current'" }

$sceneNames = @((Invoke-Obs "GetSceneList").scenes | ForEach-Object { $_.sceneName })
if ($sceneNames -contains $probe) { Fail "probe scene still loaded: OBS saved over the staged file before loading" }
else { Pass "probe scene gone from OBS (staged file was loaded)" }
if ($sceneNames.Count -eq $stagedSceneCount) { Pass "scene count matches staged file ($stagedSceneCount)" }
else { Fail "scene count $($sceneNames.Count), staged file has $stagedSceneCount" }

Start-Sleep -Seconds 2   # any save OBS runs after loading must write the staged state, not the old one
$onDisk = Get-Content $activeFile.FullName -Raw
if ($onDisk -match [regex]::Escape($probe)) { Fail "probe scene is in the collection file on disk" }
else { Pass "probe scene not in the collection file on disk" }

# 5. Refresh.
$refresh = Invoke-Ctd "show_refresh"
if ($refresh.success) { Pass "show_refresh returned success" } else { Fail "show_refresh: $($refresh.error)" }

# 6. Output guard (C2), optional because it starts a recording.
if ($TestGuard) {
    Invoke-Obs "StartRecord" | Out-Null
    Start-Sleep -Seconds 2
    $guard = Invoke-Ctd "show_apply" @{ collection = $staged; profile = "" }
    if (-not $guard.success -and $guard.error -eq "outputs active") { Pass "show_apply refused while recording" }
    else { Fail "show_apply while recording: success=$($guard.success) error=$($guard.error)" }
    Invoke-Obs "StopRecord" | Out-Null
}

$ws.Dispose()
Remove-Item $stageDir -Recurse -Force

if ($script:failures) { Write-Host "$($script:failures) check(s) failed" -ForegroundColor Red; exit 1 }
Write-Host "All checks passed" -ForegroundColor Green
