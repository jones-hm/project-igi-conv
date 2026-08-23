param(
    [string]$Executable = "",
    [int]$HttpPort = 18767,
    [string]$QtBin = $env:IGI1CONV_QT_BIN
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$root = (Resolve-Path (Join-Path $PSScriptRoot "..\")).Path
if ([string]::IsNullOrWhiteSpace($Executable)) {
    $Executable = Join-Path $root "bin\Release\igi1conv.exe"
}
if (-not (Test-Path -LiteralPath $Executable)) {
    throw "MCP executable not found: $Executable"
}
if (-not [string]::IsNullOrWhiteSpace($QtBin) -and (Test-Path -LiteralPath $QtBin)) {
    $env:Path = "$QtBin;$env:Path"
}

function Assert-Condition([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function Start-Child([string[]]$Arguments) {
    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $Executable
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardInput = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    foreach ($argument in $Arguments) { [void]$startInfo.ArgumentList.Add($argument) }
    $child = [System.Diagnostics.Process]::new()
    $child.StartInfo = $startInfo
    [void]$child.Start()
    return $child
}

function Json-Line($Object) {
    return ($Object | ConvertTo-Json -Compress -Depth 20)
}

$fixture = Join-Path $root "tests\fixtures\mcp_game_objects.qsc"
Assert-Condition (Test-Path -LiteralPath $fixture) "MCP fixture is missing: $fixture"
$objectOutput = Join-Path ([System.IO.Path]::GetTempPath()) ("igi1conv_mcp_smoke_" + [guid]::NewGuid().ToString("N") + ".qsc")

# Real stdio process: lifecycle, tool discovery, and a game-data operation.
$stdio = Start-Child @("mcp", "--transport", "stdio")
try {
    $initialize = [ordered]@{
        jsonrpc = "2.0"; id = 1; method = "initialize"
        params = [ordered]@{ protocolVersion = "2025-11-25"; capabilities = @{}; clientInfo = [ordered]@{ name = "smoke"; version = "1" } }
    }
    $initialized = [ordered]@{ jsonrpc = "2.0"; method = "notifications/initialized" }
    $call = [ordered]@{
        jsonrpc = "2.0"; id = 2; method = "tools/call"
        params = [ordered]@{
            name = "igi_game_command"
            arguments = [ordered]@{
                command = "qsc.list-objects"
                args = @($fixture, "--json")
                working_directory = $root
            }
        }
    }
    $objectCall = [ordered]@{
        jsonrpc = "2.0"; id = 3; method = "tools/call"
        params = [ordered]@{
            name = "igi_game_object_edit"
            arguments = [ordered]@{
                input_file = $fixture; output_file = $objectOutput
                selector = [ordered]@{ task_id = 701 }
                position = @(100, 200, 300); rotation = 1.25; model_id = "updated_model"; team = 3
            }
        }
    }
    $stdio.StandardInput.WriteLine((Json-Line $initialize))
    $stdio.StandardInput.WriteLine((Json-Line $initialized))
    $stdio.StandardInput.WriteLine((Json-Line $call))
    $stdio.StandardInput.WriteLine((Json-Line $objectCall))
    $stdio.StandardInput.Close()
    $stdout = $stdio.StandardOutput.ReadToEnd()
    $stderr = $stdio.StandardError.ReadToEnd()
    $stdio.WaitForExit()
    Assert-Condition ($stdio.ExitCode -eq 0) "stdio server failed: $stderr"
    $responses = @($stdout -split "`r?`n" | Where-Object { $_.Length -gt 0 } | ForEach-Object { $_ | ConvertFrom-Json })
    Assert-Condition ($responses.Count -eq 3) "stdio response count was $($responses.Count)"
    Assert-Condition ($responses[0].result.protocolVersion -eq "2025-11-25") "stdio initialize negotiation failed"
    Assert-Condition ($responses[1].result.structuredContent.exit_code -eq 0) "real game operation failed: $($responses[1] | ConvertTo-Json -Depth 20)"
    Assert-Condition ($responses[1].result.structuredContent.stdout.Contains("SmokeAlpha")) "real QSC listing did not reach the game operation"
    Assert-Condition ($responses[2].result.structuredContent.exit_code -eq 0) "real game object edit failed: $($responses[2] | ConvertTo-Json -Depth 20)"
    Assert-Condition (Test-Path -LiteralPath $objectOutput) "real game object edit did not create its explicit output"
    $editedText = Get-Content -LiteralPath $objectOutput -Raw
    Assert-Condition ($editedText.Contains('701, "HumanSoldier", "SmokeAlpha", 100, 200, 300, 1.25, "updated_model", 3')) "real game object edit did not apply position/rotation/model/team"
    Assert-Condition ($editedText.Contains('702, "Weapon", "SmokeRifle"')) "real game object edit changed unrelated game data"
    Write-Output "stdio: PASS"
}
finally {
    if ($stdio -and -not $stdio.HasExited) { $stdio.Kill(); $stdio.WaitForExit() }
    if (Test-Path -LiteralPath $objectOutput) { Remove-Item -LiteralPath $objectOutput -Force }
}

# Real Streamable HTTP process: successful localhost request and Origin guard.
$http = Start-Child @("mcp", "--transport", "http", "--host", "127.0.0.1", "--port", ([string]$HttpPort))
try {
    $ready = $false
    for ($attempt = 0; $attempt -lt 30; ++$attempt) {
        if ($http.HasExited) { break }
        $listeners = [System.Net.NetworkInformation.IPGlobalProperties]::GetIPGlobalProperties().GetActiveTcpListeners()
        if ($listeners | Where-Object { $_.Port -eq $HttpPort }) {
            $ready = $true
            break
        }
        Start-Sleep -Milliseconds 100
    }
    Assert-Condition $ready "HTTP MCP server did not start"
    $httpBody = Json-Line $initialize
    $httpResponse = Invoke-WebRequest -Uri "http://127.0.0.1:$HttpPort/mcp" -Method Post -ContentType "application/json" -Headers @{ Origin = "http://127.0.0.1:$HttpPort"; Accept = "application/json, text/event-stream"; "MCP-Protocol-Version" = "2025-11-25" } -Body $httpBody -UseBasicParsing
    $httpJson = $httpResponse.Content | ConvertFrom-Json
    Assert-Condition ($httpResponse.StatusCode -eq 200) "HTTP status was $($httpResponse.StatusCode)"
    Assert-Condition ($httpJson.result.serverInfo.name -eq "igi1conv") "HTTP MCP response was not initialize"

    $httpToolsRequest = [ordered]@{ jsonrpc = "2.0"; id = 4; method = "tools/list" }
    $httpToolsResponse = Invoke-WebRequest -Uri "http://127.0.0.1:$HttpPort/mcp" -Method Post -ContentType "application/json" -Headers @{ Origin = "http://127.0.0.1:$HttpPort"; Accept = "application/json, text/event-stream"; "MCP-Protocol-Version" = "2025-11-25" } -Body (Json-Line $httpToolsRequest) -UseBasicParsing
    $httpToolsJson = $httpToolsResponse.Content | ConvertFrom-Json
    Assert-Condition ($httpToolsResponse.StatusCode -eq 200) "HTTP tools/list status was $($httpToolsResponse.StatusCode)"
    $httpToolNames = @($httpToolsJson.result.tools | ForEach-Object { $_.name })
    Assert-Condition ($httpToolNames -contains "igi_game_command") "HTTP tools/list omitted igi_game_command"
    Assert-Condition ($httpToolNames -contains "igi_game_object_edit") "HTTP tools/list omitted igi_game_object_edit"

    $httpCallResponse = Invoke-WebRequest -Uri "http://127.0.0.1:$HttpPort/mcp" -Method Post -ContentType "application/json" -Headers @{ Origin = "http://127.0.0.1:$HttpPort"; Accept = "application/json, text/event-stream"; "MCP-Protocol-Version" = "2025-11-25" } -Body (Json-Line $call) -UseBasicParsing
    $httpCallJson = $httpCallResponse.Content | ConvertFrom-Json
    Assert-Condition ($httpCallResponse.StatusCode -eq 200) "HTTP tools/call status was $($httpCallResponse.StatusCode)"
    Assert-Condition ($httpCallJson.result.structuredContent.exit_code -eq 0) "HTTP game operation failed"
    Assert-Condition ($httpCallJson.result.structuredContent.stdout.Contains("SmokeAlpha")) "HTTP game operation did not reach QSC listing"

    $httpNotificationResponse = Invoke-WebRequest -Uri "http://127.0.0.1:$HttpPort/mcp" -Method Post -ContentType "application/json" -Headers @{ Origin = "http://127.0.0.1:$HttpPort"; Accept = "application/json, text/event-stream"; "MCP-Protocol-Version" = "2025-11-25" } -Body (Json-Line $initialized) -UseBasicParsing
    Assert-Condition ($httpNotificationResponse.StatusCode -eq 202) "HTTP notification status was $($httpNotificationResponse.StatusCode)"

    try {
        Invoke-WebRequest -Uri "http://127.0.0.1:$HttpPort/mcp" -Method Get -Headers @{ Origin = "http://127.0.0.1:$HttpPort"; Accept = "application/json, text/event-stream"; "MCP-Protocol-Version" = "2025-11-25" } -UseBasicParsing | Out-Null
        throw "HTTP GET request was accepted"
    } catch {
        if ($_.Exception.Response.StatusCode.value__ -ne 405) { throw }
    }

    try {
        Invoke-WebRequest -Uri "http://127.0.0.1:$HttpPort/mcp" -Method Post -ContentType "application/json" -Headers @{ Origin = "http://127.0.0.1:$HttpPort"; Accept = "application/json"; "MCP-Protocol-Version" = "2025-11-25" } -Body $httpBody -UseBasicParsing | Out-Null
        throw "HTTP request with incomplete Accept header was accepted"
    } catch {
        if ($_.Exception.Response.StatusCode.value__ -ne 400) { throw }
    }
    try {
        Invoke-WebRequest -Uri "http://127.0.0.1:$HttpPort/mcp" -Method Post -ContentType "application/json" -Headers @{ Origin = "http://127.0.0.1:$HttpPort"; Accept = "application/json, text/event-stream"; "MCP-Protocol-Version" = "2099-01-01" } -Body $httpBody -UseBasicParsing | Out-Null
        throw "HTTP request with unsupported protocol version was accepted"
    } catch {
        if ($_.Exception.Response.StatusCode.value__ -ne 400) { throw }
    }
    $invalidOriginAccepted = $false
    try {
        Invoke-WebRequest -Uri "http://127.0.0.1:$HttpPort/mcp" -Method Post -ContentType "application/json" -Headers @{ Origin = "http://evil.example"; Accept = "application/json, text/event-stream"; "MCP-Protocol-Version" = "2025-11-25" } -Body $httpBody -UseBasicParsing | Out-Null
        $invalidOriginAccepted = $true
    } catch {
        if ($_.Exception.Response.StatusCode.value__ -ne 403) { throw }
    }
    Assert-Condition (-not $invalidOriginAccepted) "invalid Origin was accepted"
    Write-Output "http: PASS"
}
finally {
    if ($http -and -not $http.HasExited) { $http.Kill(); $http.WaitForExit() }
}

# Remote binds must not start without authentication.
$remote = Start-Child @("mcp", "--transport", "http", "--host", "0.0.0.0", "--port", ([string]($HttpPort + 1)))
$remoteError = $remote.StandardError.ReadToEnd()
$remote.WaitForExit()
Assert-Condition ($remote.ExitCode -eq 1) "unauthenticated remote HTTP bind was accepted"
Assert-Condition ($remoteError.Contains("requires --auth-token")) "remote bind rejection did not explain the authentication requirement"
Write-Output "remote-auth-guard: PASS"
