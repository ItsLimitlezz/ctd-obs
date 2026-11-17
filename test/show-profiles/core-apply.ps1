<#
.SYNOPSIS
  Show Profiles core check (T8): ctd_show_apply / ctd_show_refresh (C1) and the output guard (C2),
  run against a built CTD OBS over obs-websocket.

.DESCRIPTION
  Needs CTD OBS from branch ctd/show-profiles-core (or later), running, with obs-websocket ENABLED
  (Tools > WebSocket Server Settings > Enable WebSocket server). Talks to the "ctd-obs" obs-websocket
  vendor, which forwards to the global procs ctd_show_apply / ctd_show_refresh.

  Checks:
    A. Re-apply the ACTIVE scene collection from a staged file. A probe scene that only exists in OBS
       must be gone from OBS and from the file on disk (OBS didn't save over the staged file), and
       obs-websocket must see CurrentSceneCollectionChanged (frontend events are delivered).
    B. Re-apply the ACTIVE profile from a staged folder with a marker in basic.ini. The marker must be
       readable through GetProfileParameter afterwards. Then re-apply the original to remove it.
    C. Apply a NEW scene collection + NEW profile (copies with new names) in one call. Both become
       active. Then switch back with OBS's own SetCurrentProfile / SetCurrentSceneCollection, which
       must work while nothing is active (C2 never blocks then).
    D. Refusals: nothing to apply; a collection inside basic/scenes; invalid JSON; a profile folder
       without basic.ini. OBS's active collection and profile must not change.
    E. show_refresh: delete the test collection file, refresh, and it's gone from the list.
    F. (-TestGuard) With a recording running: show_apply refuses with "outputs active", and OBS's own
       SetCurrentSceneCollection / SetCurrentProfile / CreateSceneCollection don't switch.
       With -TestVirtualCam the same with the virtual camera instead (needs the CTD virtual camera installed).

  Leaves the collection and profile that were active before. Removes everything it created.
  Makes no network calls except to the local obs-websocket.

  Runs on Windows PowerShell 5.1 and PowerShell 7. For a portable copy, pass -ConfigRoot
  <portable folder>\config\ctd-obs.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File test\show-profiles\core-apply.ps1 -Password (Read-Host -AsSecureString) -TestGuard

.EXAMPLE
  pwsh -File test\show-profiles\core-apply.ps1 -Password (Read-Host -AsSecureString) -TestGuard
#>
param(
    [string]$WsHost = "localhost",
    [int]$Port = 4455,
    [SecureString]$Password,
    [string]$ConfigRoot = (Join-Path $env:APPDATA "ctd-obs"),
    [switch]$TestGuard,
    [switch]$TestVirtualCam
)

$ErrorActionPreference = "Stop"
$script:failures = 0
$script:events = [System.Collections.Generic.List[object]]::new()

function Pass($msg) { Write-Host "PASS  $msg" -ForegroundColor Green }
function Fail($msg) { Write-Host "FAIL  $msg" -ForegroundColor Red; $script:failures++ }
function Info($msg) { Write-Host "      $msg" }
function Check([bool]$ok, $msg) { if ($ok) { Pass $msg } else { Fail $msg } }

$utf8 = [Text.UTF8Encoding]::new($false)

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

# Windows PowerShell 5.1 returns a VoidTaskResult from GetResult() on a plain Task; drop it so it
# doesn't end up in the caller's output.
function Send-Json($obj) {
    $bytes = [Text.Encoding]::UTF8.GetBytes(($obj | ConvertTo-Json -Depth 20 -Compress))
    $null = $ws.SendAsync([ArraySegment[byte]]::new($bytes), 'Text', $true, $ct).GetAwaiter().GetResult()
}

function Get-Sha256B64([string]$s) {
    $sha = [Security.Cryptography.SHA256]::Create()
    [Convert]::ToBase64String($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($s)))
}

function Connect-Obs {
    $null = $ws.ConnectAsync([Uri]"ws://$($WsHost):$Port", $ct).GetAwaiter().GetResult()
    $hello = Receive-Json
    # eventSubscriptions 2 = Config events (CurrentSceneCollectionChanged, CurrentProfileChanged, ...)
    $identify = @{ op = 1; d = @{ rpcVersion = 1; eventSubscriptions = 2 } }
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

# Returns the response data and throws on failure. With -AllowFail it never throws and returns
# @{ requestStatus = @{ result; code; comment }; responseData } instead, so a caller can tell a refusal
# (requestStatus.result -eq $false, plus the code) from a success that has no response data.
function Invoke-Obs([string]$type, $data = @{}, [switch]$AllowFail) {
    $id = [guid]::NewGuid().ToString()
    Send-Json @{ op = 6; d = @{ requestType = $type; requestId = $id; requestData = $data } }
    while ($true) {
        $msg = Receive-Json
        if ($msg.op -eq 5) { $script:events.Add($msg.d); continue }
        if ($msg.op -eq 7 -and $msg.d.requestId -eq $id) {
            $status = $msg.d.requestStatus
            if ($AllowFail) {
                return [pscustomobject]@{
                    requestStatus = [pscustomobject]@{
                        result  = [bool]$status.result
                        code    = [int]$status.code
                        comment = [string]$status.comment
                    }
                    responseData  = $msg.d.responseData
                }
            }
            if (-not $status.result) { throw "$type failed: $($status.code) $($status.comment)" }
            return $msg.d.responseData
        }
    }
}

# obs-websocket RequestStatus codes used below
$ResourceCreationFailed = 700

function Invoke-Ctd([string]$request, $data = @{}) {
    $r = Invoke-Obs "CallVendorRequest" @{ vendorName = "ctd-obs"; requestType = $request; requestData = $data }
    $r.responseData
}

function Show-Apply([string]$Col = "", [string]$Prof = "") {
    Invoke-Ctd "show_apply" @{ collection = $Col; profile = $Prof }
}

function Get-ActiveNames {
    @{
        Collection = (Invoke-Obs "GetSceneCollectionList").currentSceneCollectionName
        Profile    = (Invoke-Obs "GetProfileList").currentProfileName
    }
}

# Not called Wait-Event: that is a built-in cmdlet name.
function Wait-ObsEvent([string]$type, [int]$seconds = 5) {
    $deadline = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $deadline) {
        if ($script:events | Where-Object { $_.eventType -eq $type }) { return $true }
        Invoke-Obs "GetVersion" | Out-Null   # pumps pending events into $script:events
        Start-Sleep -Milliseconds 200
    }
    return $false
}

# ---------------------------------------------------------------- files

# [IO.File] reads UTF-8 without a BOM correctly on both PowerShell versions; Windows PowerShell 5.1's
# Get-Content would read OBS's files in the ANSI code page and mangle non-ASCII names.
function Read-Text([string]$path) { [IO.File]::ReadAllText($path, $utf8) }
function Read-Json([string]$path) { Read-Text $path | ConvertFrom-Json }

$scenesDir = Join-Path $ConfigRoot "basic\scenes"
$profilesDir = Join-Path $ConfigRoot "basic\profiles"

function Find-CollectionFile([string]$name) {
    Get-ChildItem $scenesDir -Filter *.json | Where-Object {
        (Read-Json $_.FullName).name -eq $name
    } | Select-Object -First 1
}

function Find-ProfileDir([string]$name) {
    Get-ChildItem $profilesDir -Directory | Where-Object {
        $ini = Join-Path $_.FullName "basic.ini"
        (Test-Path $ini) -and ([IO.File]::ReadAllLines($ini, $utf8) -match "^Name=$([regex]::Escape($name))$")
    } | Select-Object -First 1
}

function Set-IniName([string]$iniPath, [string]$name) {
    $text = [IO.File]::ReadAllText($iniPath)
    $text = [regex]::new('(?m)^Name=.*$').Replace($text, "Name=$name", 1)
    [IO.File]::WriteAllText($iniPath, $text, $utf8)
}

function Set-CollectionName([string]$jsonPath, [string]$name) {
    $json = Read-Json $jsonPath
    $json.name = $name
    [IO.File]::WriteAllText($jsonPath, ($json | ConvertTo-Json -Depth 100 -Compress), $utf8)
}

# ---------------------------------------------------------------- setup

Connect-Obs
Info "Connected to obs-websocket on port $Port"

$orig = Get-ActiveNames
Info "Active collection: $($orig.Collection) / profile: $($orig.Profile)"

$activeFile = Find-CollectionFile $orig.Collection
if (-not $activeFile) { throw "No file in $scenesDir has name '$($orig.Collection)'" }
$activeProfileDir = Find-ProfileDir $orig.Profile
if (-not $activeProfileDir) { throw "No folder in $profilesDir has basic.ini Name=$($orig.Profile)" }
Info "Collection file: $($activeFile.Name) / profile folder: $($activeProfileDir.Name)"

$tag = [guid]::NewGuid().ToString().Substring(0, 8)
$stageDir = Join-Path $env:TEMP "ctd-core-apply-$tag"
New-Item -ItemType Directory -Force $stageDir | Out-Null

# Snapshot of the active collection and profile as they are now (used to restore B).
$stagedCollection = Join-Path $stageDir $activeFile.Name
Copy-Item $activeFile.FullName $stagedCollection
$stagedSceneCount = @((Read-Json $stagedCollection).scene_order).Count
$stagedProfileOrig = Join-Path $stageDir "profile-orig"
Copy-Item $activeProfileDir.FullName $stagedProfileOrig -Recurse

$testCollectionName = "CTD Core Test $tag"
$testProfileName = "CTD Core Test $tag"

try {
    # ------------------------------------------------------------ A. re-apply the active collection
    Write-Host "`n[A] Re-apply the active scene collection"
    $probe = "Core Probe $tag"
    Invoke-Obs "CreateScene" @{ sceneName = $probe } | Out-Null
    Start-Sleep -Seconds 2   # let OBS run its own queued save, as in normal use
    $script:events.Clear()

    $r = Show-Apply -Col $stagedCollection
    Check $r.success "show_apply (active collection) succeeded $(if (-not $r.success) { ": $($r.error)" })"
    Check (-not $r.restart_required) "restart_required is false with no profile"
    Check ((Get-ActiveNames).Collection -eq $orig.Collection) "active collection is still '$($orig.Collection)'"
    Check (Wait-ObsEvent "CurrentSceneCollectionChanged") "obs-websocket saw CurrentSceneCollectionChanged"

    $sceneNames = @((Invoke-Obs "GetSceneList").scenes | ForEach-Object { $_.sceneName })
    Check (-not ($sceneNames -contains $probe)) "probe scene gone from OBS (the staged file was loaded)"
    Check ($sceneNames.Count -eq $stagedSceneCount) "scene count matches the staged file ($stagedSceneCount)"
    Start-Sleep -Seconds 2   # any save after loading must write the staged state, not the old one
    Check (-not ((Read-Text $activeFile.FullName) -match [regex]::Escape($probe))) `
        "probe scene not in the collection file on disk"

    # ------------------------------------------------------------ B. re-apply the active profile
    Write-Host "`n[B] Re-apply the active profile"
    $marker = "marker-$tag"
    $stagedProfileMarked = Join-Path $stageDir "profile-marked"
    Copy-Item $stagedProfileOrig $stagedProfileMarked -Recurse
    [IO.File]::AppendAllText((Join-Path $stagedProfileMarked "basic.ini"), "`r`n[CtdCoreTest]`r`nMarker=$marker`r`n", $utf8)
    $script:events.Clear()

    $r = Show-Apply -Prof $stagedProfileMarked
    Check $r.success "show_apply (active profile) succeeded $(if (-not $r.success) { ": $($r.error)" })"
    Check (-not $r.restart_required) "restart_required is false (same audio settings)"
    Check ((Get-ActiveNames).Profile -eq $orig.Profile) "active profile is still '$($orig.Profile)'"
    Check (Wait-ObsEvent "CurrentProfileChanged") "obs-websocket saw CurrentProfileChanged"
    $p = Invoke-Obs "GetProfileParameter" @{ parameterCategory = "CtdCoreTest"; parameterName = "Marker" }
    Check ($p.parameterValue -eq $marker) "new basic.ini is in use (marker = '$($p.parameterValue)')"
    Check ((Read-Text (Join-Path $activeProfileDir.FullName "basic.ini")) -match [regex]::Escape($marker)) `
        "new basic.ini is on disk"

    $r = Show-Apply -Prof $stagedProfileOrig
    Check $r.success "show_apply (restore original profile) succeeded $(if (-not $r.success) { ": $($r.error)" })"
    $p = Invoke-Obs "GetProfileParameter" @{ parameterCategory = "CtdCoreTest"; parameterName = "Marker" }
    Check ([string]::IsNullOrEmpty($p.parameterValue)) "marker gone after restoring the original profile"

    # ------------------------------------------------------------ C. new collection + new profile
    Write-Host "`n[C] Apply a new scene collection and a new profile in one call"
    $newCollection = Join-Path $stageDir "new-collection.json"
    Copy-Item $stagedCollection $newCollection
    Set-CollectionName $newCollection $testCollectionName
    $newProfile = Join-Path $stageDir "new-profile"
    Copy-Item $stagedProfileOrig $newProfile -Recurse
    Set-IniName (Join-Path $newProfile "basic.ini") $testProfileName
    $script:events.Clear()

    $r = Show-Apply -Col $newCollection -Prof $newProfile
    Check $r.success "show_apply (new collection + profile) succeeded $(if (-not $r.success) { ": $($r.error)" })"
    $now = Get-ActiveNames
    Check ($now.Collection -eq $testCollectionName) "active collection is '$testCollectionName' (got '$($now.Collection)')"
    Check ($now.Profile -eq $testProfileName) "active profile is '$testProfileName' (got '$($now.Profile)')"
    Check ([bool](Find-CollectionFile $testCollectionName)) "new collection file is in basic/scenes"
    Check ([bool](Find-ProfileDir $testProfileName)) "new profile folder is in basic/profiles"
    Check (Wait-ObsEvent "CurrentSceneCollectionChanged") "obs-websocket saw CurrentSceneCollectionChanged"

    Invoke-Obs "SetCurrentProfile" @{ profileName = $orig.Profile } | Out-Null
    Invoke-Obs "SetCurrentSceneCollection" @{ sceneCollectionName = $orig.Collection } | Out-Null
    Start-Sleep -Seconds 1
    $now = Get-ActiveNames
    Check ($now.Collection -eq $orig.Collection -and $now.Profile -eq $orig.Profile) `
        "OBS's own switch back works while nothing is active"

    # ------------------------------------------------------------ D. refusals
    Write-Host "`n[D] Refusals leave OBS as it is"
    $r = Show-Apply
    Check (-not $r.success -and $r.error -eq "nothing to apply") "empty call refused: '$($r.error)'"

    $r = Show-Apply -Col $activeFile.FullName
    Check (-not $r.success -and $r.error -like "*outside basic/scenes*") "collection inside basic/scenes refused: '$($r.error)'"

    $bad = Join-Path $stageDir "bad.json"
    [IO.File]::WriteAllText($bad, "{ not json", $utf8)
    $r = Show-Apply -Col $bad
    Check (-not $r.success -and $r.error -like "*not valid JSON*") "invalid JSON refused: '$($r.error)'"

    $emptyProfile = Join-Path $stageDir "empty-profile"
    New-Item -ItemType Directory -Force $emptyProfile | Out-Null
    $r = Show-Apply -Prof $emptyProfile
    Check (-not $r.success -and $r.error -like "*basic.ini*") "profile without basic.ini refused: '$($r.error)'"

    $now = Get-ActiveNames
    Check ($now.Collection -eq $orig.Collection -and $now.Profile -eq $orig.Profile) "active collection and profile unchanged"

    # ------------------------------------------------------------ E. refresh
    Write-Host "`n[E] show_refresh picks up removed files"
    Remove-Item (Find-CollectionFile $testCollectionName).FullName
    $r = Invoke-Ctd "show_refresh"
    Check $r.success "show_refresh succeeded $(if (-not $r.success) { ": $($r.error)" })"
    $list = @((Invoke-Obs "GetSceneCollectionList").sceneCollections)
    Check (-not ($list -contains $testCollectionName)) "removed collection is gone from the list"
    Check ($list -contains $orig.Collection) "active collection is still in the list"

    # ------------------------------------------------------------ F. guard
    $guardOutputs = @()
    if ($TestGuard) { $guardOutputs += , @("recording", "StartRecord", "StopRecord") }
    if ($TestVirtualCam) { $guardOutputs += , @("virtual camera", "StartVirtualCam", "StopVirtualCam") }
    foreach ($g in $guardOutputs) {
        Write-Host "`n[F] Guard with the $($g[0]) running"
        # A second collection to switch to, dropped into basic/scenes while idle and picked up by show_refresh.
        $guardFile = Join-Path $scenesDir "ctd-guard-test-$tag.json"
        if (-not (Test-Path $guardFile)) {
            Copy-Item $stagedCollection $guardFile
            Set-CollectionName $guardFile "CTD Guard Test $tag"
            Invoke-Ctd "show_refresh" | Out-Null
        }

        Invoke-Obs $g[1] | Out-Null
        Start-Sleep -Seconds 2
        try {
            $r = Show-Apply -Col $stagedCollection
            Check (-not $r.success -and $r.error -eq "outputs active") "show_apply refused: '$($r.error)'"

            $created = Invoke-Obs "CreateSceneCollection" @{ sceneCollectionName = "CTD Guard Create $tag" } -AllowFail
            $st = $created.requestStatus
            Check ($st.result -eq $false -and $st.code -eq $ResourceCreationFailed) `
                "CreateSceneCollection refused (result $($st.result), code $($st.code)$(if ($st.comment) { ", $($st.comment)" }))"
            $list = @((Invoke-Obs "GetSceneCollectionList").sceneCollections)
            Check (-not ($list -contains "CTD Guard Create $tag")) "refused collection is not in the list"

            $others = @((Invoke-Obs "GetSceneCollectionList").sceneCollections | Where-Object { $_ -ne $orig.Collection })
            if ($others) {
                Invoke-Obs "SetCurrentSceneCollection" @{ sceneCollectionName = $others[0] } -AllowFail | Out-Null
                Start-Sleep -Seconds 1
                Check ((Get-ActiveNames).Collection -eq $orig.Collection) "SetCurrentSceneCollection didn't switch"
            } else { Info "only one scene collection: SetCurrentSceneCollection not checked" }

            $otherProfiles = @((Invoke-Obs "GetProfileList").profiles | Where-Object { $_ -ne $orig.Profile })
            if ($otherProfiles) {
                Invoke-Obs "SetCurrentProfile" @{ profileName = $otherProfiles[0] } -AllowFail | Out-Null
                Start-Sleep -Seconds 1
                Check ((Get-ActiveNames).Profile -eq $orig.Profile) "SetCurrentProfile didn't switch"
            } else { Info "only one profile: SetCurrentProfile not checked" }
        } finally {
            Invoke-Obs $g[2] | Out-Null
            Start-Sleep -Seconds 2
        }

        $r = Show-Apply -Col $stagedCollection
        Check $r.success "show_apply works again once the $($g[0]) stopped $(if (-not $r.success) { ": $($r.error)" })"
    }
    if (-not $guardOutputs) { Info "Guard not tested (pass -TestGuard and/or -TestVirtualCam)" }
}
finally {
    # ------------------------------------------------------------ cleanup
    try {
        $now = Get-ActiveNames
        if ($now.Profile -ne $orig.Profile) { Invoke-Obs "SetCurrentProfile" @{ profileName = $orig.Profile } -AllowFail | Out-Null }
        if ($now.Collection -ne $orig.Collection) {
            Invoke-Obs "SetCurrentSceneCollection" @{ sceneCollectionName = $orig.Collection } -AllowFail | Out-Null
        }
        if (Find-ProfileDir $testProfileName) { Invoke-Obs "RemoveProfile" @{ profileName = $testProfileName } -AllowFail | Out-Null }
        foreach ($n in @($testCollectionName, "CTD Guard Test $tag", "CTD Guard Create $tag")) {
            $f = Find-CollectionFile $n
            if ($f) { Remove-Item $f.FullName }
        }
        Invoke-Ctd "show_refresh" | Out-Null
    } catch { Info "cleanup: $_" }
    $ws.Dispose()
    Remove-Item $stageDir -Recurse -Force -ErrorAction SilentlyContinue
}

if ($script:failures) { Write-Host "`n$($script:failures) check(s) failed" -ForegroundColor Red; exit 1 }
Write-Host "`nAll checks passed" -ForegroundColor Green
