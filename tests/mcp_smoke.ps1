param(
    [string]$Executable = "",
    [int]$HttpPort = 18767,
    [string]$QtBin = $env:IGI1CONV_QT_BIN,
    [string]$ArtifactRoot = (Join-Path $PSScriptRoot "..\tests_temp")
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

function Http-Headers([string]$Origin, [string]$SessionId = "", [string]$Accept = "application/json, text/event-stream") {
    $headers = @{
        Origin = $Origin
        Accept = $Accept
        "MCP-Protocol-Version" = "2025-11-25"
    }
    if (-not [string]::IsNullOrWhiteSpace($SessionId)) { $headers["Mcp-Session-Id"] = $SessionId }
    return $headers
}

function Send-RawHttp([int]$Port, [string]$Request) {
    $client = [System.Net.Sockets.TcpClient]::new()
    try {
        $client.Connect("127.0.0.1", $Port)
        $stream = $client.GetStream()
        $stream.ReadTimeout = 5000
        $bytes = [System.Text.Encoding]::ASCII.GetBytes($Request)
        $stream.Write($bytes, 0, $bytes.Length)
        $stream.Flush()
        $buffer = New-Object byte[] 8192
        $output = [System.IO.MemoryStream]::new()
        try {
            while (($read = $stream.Read($buffer, 0, $buffer.Length)) -gt 0) {
                $output.Write($buffer, 0, $read)
            }
            return [System.Text.Encoding]::ASCII.GetString($output.ToArray())
        } finally {
            $output.Dispose()
        }
    } finally {
        $client.Dispose()
    }
}

$bundledFixture = Join-Path $PSScriptRoot "fixtures\mcp_game_objects.qsc"
$fixture = if (Test-Path -LiteralPath $bundledFixture) { $bundledFixture } else { Join-Path $root "tests\fixtures\mcp_game_objects.qsc" }
Assert-Condition (Test-Path -LiteralPath $fixture) "MCP fixture is missing: $fixture"
New-Item -ItemType Directory -Path $ArtifactRoot -Force | Out-Null
$artifactRoot = Convert-Path -LiteralPath $ArtifactRoot
$objectOutput = Join-Path $artifactRoot ("igi1conv_mcp_smoke_" + [guid]::NewGuid().ToString("N") + ".qsc")
$weaponOutput = Join-Path $artifactRoot ("igi1conv_mcp_weapon_smoke_" + [guid]::NewGuid().ToString("N") + ".qsc")

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
    $weaponCall = [ordered]@{
        jsonrpc = "2.0"; id = 4; method = "tools/call"
        params = [ordered]@{
            name = "igi_game_object_edit"
            arguments = [ordered]@{
                input_file = $fixture; output_file = $weaponOutput
                selector = [ordered]@{ task_id = 702 }
                updates = @(
                    [ordered]@{ direct_index = 7; literal = '"rocket_launcher"' }
                    [ordered]@{ direct_index = 8; literal = '12' }
                )
            }
        }
    }
    $stdio.StandardInput.WriteLine((Json-Line $initialize))
    $stdio.StandardInput.WriteLine((Json-Line $initialized))
    $stdio.StandardInput.WriteLine((Json-Line $call))
    $stdio.StandardInput.WriteLine((Json-Line $objectCall))
    $stdio.StandardInput.WriteLine((Json-Line $weaponCall))
    $stdio.StandardInput.Close()
    $stderrTask = $stdio.StandardError.ReadToEndAsync()
    $stdout = $stdio.StandardOutput.ReadToEnd()
    $stderr = $stderrTask.GetAwaiter().GetResult()
    $stdio.WaitForExit()
    Assert-Condition ($stdio.ExitCode -eq 0) "stdio server failed: $stderr"
    $responses = @($stdout -split "`r?`n" | Where-Object { $_.Length -gt 0 } | ForEach-Object { $_ | ConvertFrom-Json })
    Assert-Condition ($responses.Count -eq 4) "stdio response count was $($responses.Count)"
    Assert-Condition ($responses[0].result.protocolVersion -eq "2025-11-25") "stdio initialize negotiation failed"
    Assert-Condition ($responses[1].result.structuredContent.exit_code -eq 0) "real game operation failed: $($responses[1] | ConvertTo-Json -Depth 20)"
    Assert-Condition ($responses[1].result.structuredContent.stdout.Contains("SmokeAlpha")) "real QSC listing did not reach the game operation"
    Assert-Condition ($responses[2].result.structuredContent.exit_code -eq 0) "real game object edit failed: $($responses[2] | ConvertTo-Json -Depth 20)"
    Assert-Condition (Test-Path -LiteralPath $objectOutput) "real game object edit did not create its explicit output"
    $editedText = Get-Content -LiteralPath $objectOutput -Raw
    Assert-Condition ($editedText.Contains('701, "HumanSoldier", "SmokeAlpha", 100, 200, 300, 1.25, "updated_model", 3')) "real game object edit did not apply position/rotation/model/team"
    Assert-Condition ($editedText.Contains('702, "Weapon", "SmokeRifle"')) "real game object edit changed unrelated game data"
    Assert-Condition ($responses[3].result.structuredContent.exit_code -eq 0) "real generic game object edit failed: $($responses[3] | ConvertTo-Json -Depth 20)"
    Assert-Condition (Test-Path -LiteralPath $weaponOutput) "real generic game object edit did not create its explicit output"
    $weaponEditedText = Get-Content -LiteralPath $weaponOutput -Raw
    Assert-Condition ($weaponEditedText.Contains('701, "HumanSoldier", "SmokeAlpha", 10, 20, 30, 0, "soldier_model", 1, 2, 3')) "generic game object edit changed unrelated HumanSoldier data"
    Assert-Condition ($weaponEditedText.Contains('702, "Weapon", "SmokeRifle", 1, 2, 3, 0, "rocket_launcher", 12')) "real generic game object edit did not apply indexed Weapon updates"
    Write-Output "stdio: PASS"
}
finally {
    if ($stdio -and -not $stdio.HasExited) { $stdio.Kill(); $stdio.WaitForExit() }
    if (Test-Path -LiteralPath $objectOutput) { Remove-Item -LiteralPath $objectOutput -Force }
    if (Test-Path -LiteralPath $weaponOutput) { Remove-Item -LiteralPath $weaponOutput -Force }
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
    $preflightOrigin = "http://127.0.0.1:$HttpPort"
    $httpBody = Json-Line $initialize
    $httpResponse = Invoke-WebRequest -Uri "http://127.0.0.1:$HttpPort/mcp" -Method Post -ContentType "application/json" -Headers (Http-Headers $preflightOrigin) -Body $httpBody -UseBasicParsing
    $httpJson = $httpResponse.Content | ConvertFrom-Json
    Assert-Condition ($httpResponse.StatusCode -eq 200) "HTTP status was $($httpResponse.StatusCode)"
    Assert-Condition ($httpJson.result.serverInfo.name -eq "igi1conv") "HTTP MCP response was not initialize"
    $sessionId = [string]$httpResponse.Headers["Mcp-Session-Id"]
    Assert-Condition (-not [string]::IsNullOrWhiteSpace($sessionId)) "HTTP initialize did not return Mcp-Session-Id"

    $httpToolsRequest = [ordered]@{ jsonrpc = "2.0"; id = 4; method = "tools/list" }
    $secondInitResponse = Invoke-WebRequest -Uri "http://127.0.0.1:$HttpPort/mcp" -Method Post -ContentType "application/json" -Headers (Http-Headers $preflightOrigin) -Body $httpBody -UseBasicParsing
    $secondSessionId = [string]$secondInitResponse.Headers["Mcp-Session-Id"]
    Assert-Condition (-not [string]::IsNullOrWhiteSpace($secondSessionId)) "second HTTP initialize did not return Mcp-Session-Id"
    Assert-Condition ($secondSessionId -ne $sessionId) "HTTP sessions were not independently identified"

    $unknownSessionHeaders = Http-Headers $preflightOrigin "igi1conv-unknown-session"
    $unknownSessionResponse = Invoke-WebRequest -Uri "http://127.0.0.1:$HttpPort/mcp" -Method Post -ContentType "application/json" -Headers $unknownSessionHeaders -Body (Json-Line $httpToolsRequest) -UseBasicParsing -SkipHttpErrorCheck
    Assert-Condition ($unknownSessionResponse.StatusCode -eq 404) "unknown HTTP session was accepted"

    $preflightResponse = Invoke-WebRequest -Uri "http://127.0.0.1:$HttpPort/mcp" -Method Options -Headers @{
        Origin = $preflightOrigin
        "Access-Control-Request-Method" = "POST"
        "Access-Control-Request-Headers" = "content-type, accept, mcp-protocol-version, mcp-session-id"
    } -UseBasicParsing
    Assert-Condition ($preflightResponse.StatusCode -eq 204) "HTTP CORS preflight status was $($preflightResponse.StatusCode)"
    $allowOrigin = [string]$preflightResponse.Headers["Access-Control-Allow-Origin"]
    $allowMethods = [string]$preflightResponse.Headers["Access-Control-Allow-Methods"]
    $allowHeaders = [string]$preflightResponse.Headers["Access-Control-Allow-Headers"]
    Assert-Condition ($allowOrigin -eq $preflightOrigin) "HTTP CORS preflight omitted the allowed Origin"
    Assert-Condition ($allowMethods.Contains("POST")) "HTTP CORS preflight omitted POST"
    Assert-Condition ($allowHeaders.Contains("MCP-Protocol-Version")) "HTTP CORS preflight omitted MCP headers"

    $invalidRequestResponse = Invoke-WebRequest -Uri "http://127.0.0.1:$HttpPort/mcp" -Method Post -ContentType "application/json" -Headers (Http-Headers $preflightOrigin $sessionId) -Body "{}" -UseBasicParsing
    $invalidRequestJson = $invalidRequestResponse.Content | ConvertFrom-Json
    Assert-Condition ($invalidRequestResponse.StatusCode -eq 200) "HTTP malformed JSON-RPC status was $($invalidRequestResponse.StatusCode)"
    Assert-Condition ($invalidRequestJson.error.code -eq -32600) "HTTP malformed id-less JSON-RPC request was not rejected"
    Assert-Condition ($invalidRequestJson.id -eq $null) "HTTP malformed JSON-RPC error did not use a null id"

    $rejectedPreflightResponse = Invoke-WebRequest -Uri "http://127.0.0.1:$HttpPort/mcp" -Method Options -Headers @{
        Origin = "http://evil.example"
        "Access-Control-Request-Method" = "POST"
    } -UseBasicParsing -SkipHttpErrorCheck
    Assert-Condition ($rejectedPreflightResponse.StatusCode -eq 403) "invalid CORS preflight was accepted"
    $rejectedAllowOrigin = [string]$rejectedPreflightResponse.Headers["Access-Control-Allow-Origin"]
    Assert-Condition ([string]::IsNullOrEmpty($rejectedAllowOrigin)) "rejected CORS preflight returned Allow-Origin"

    $httpToolsResponse = Invoke-WebRequest -Uri "http://127.0.0.1:$HttpPort/mcp" -Method Post -ContentType "application/json" -Headers (Http-Headers $preflightOrigin $sessionId) -Body (Json-Line $httpToolsRequest) -UseBasicParsing
    $httpToolsJson = $httpToolsResponse.Content | ConvertFrom-Json
    Assert-Condition ($httpToolsResponse.StatusCode -eq 200) "HTTP tools/list status was $($httpToolsResponse.StatusCode)"
    $httpToolNames = @($httpToolsJson.result.tools | ForEach-Object { $_.name })
    Assert-Condition ($httpToolNames -contains "igi_game_command") "HTTP tools/list omitted igi_game_command"
    Assert-Condition ($httpToolNames -contains "igi_game_object_edit") "HTTP tools/list omitted igi_game_object_edit"

    $httpCallResponse = Invoke-WebRequest -Uri "http://127.0.0.1:$HttpPort/mcp" -Method Post -ContentType "application/json" -Headers (Http-Headers $preflightOrigin $sessionId) -Body (Json-Line $call) -UseBasicParsing
    $httpCallJson = $httpCallResponse.Content | ConvertFrom-Json
    Assert-Condition ($httpCallResponse.StatusCode -eq 200) "HTTP tools/call status was $($httpCallResponse.StatusCode)"
    Assert-Condition ($httpCallJson.result.structuredContent.exit_code -eq 0) "HTTP game operation failed"
    Assert-Condition ($httpCallJson.result.structuredContent.stdout.Contains("SmokeAlpha")) "HTTP game operation did not reach QSC listing"

    $httpNotificationResponse = Invoke-WebRequest -Uri "http://127.0.0.1:$HttpPort/mcp" -Method Post -ContentType "application/json" -Headers (Http-Headers $preflightOrigin $sessionId) -Body (Json-Line $initialized) -UseBasicParsing
    Assert-Condition ($httpNotificationResponse.StatusCode -eq 202) "HTTP notification status was $($httpNotificationResponse.StatusCode)"

    try {
        Invoke-WebRequest -Uri "http://127.0.0.1:$HttpPort/mcp" -Method Get -Headers (Http-Headers $preflightOrigin $sessionId) -UseBasicParsing | Out-Null
        throw "HTTP GET request was accepted"
    } catch {
        if ($_.Exception.Response.StatusCode.value__ -ne 405) { throw }
    }

    try {
        Invoke-WebRequest -Uri "http://127.0.0.1:$HttpPort/mcp" -Method Post -ContentType "application/json" -Headers (Http-Headers $preflightOrigin $sessionId "application/json") -Body $httpBody -UseBasicParsing | Out-Null
        throw "HTTP request with incomplete Accept header was accepted"
    } catch {
        if ($_.Exception.Response.StatusCode.value__ -ne 400) { throw }
    }
    try {
        $unsupportedHeaders = Http-Headers $preflightOrigin $sessionId
        $unsupportedHeaders["MCP-Protocol-Version"] = "2099-01-01"
        Invoke-WebRequest -Uri "http://127.0.0.1:$HttpPort/mcp" -Method Post -ContentType "application/json" -Headers $unsupportedHeaders -Body $httpBody -UseBasicParsing | Out-Null
        throw "HTTP request with unsupported protocol version was accepted"
    } catch {
        if ($_.Exception.Response.StatusCode.value__ -ne 400) { throw }
    }
    $invalidOriginAccepted = $false
    try {
        Invoke-WebRequest -Uri "http://127.0.0.1:$HttpPort/mcp" -Method Post -ContentType "application/json" -Headers (Http-Headers "http://evil.example" $sessionId) -Body $httpBody -UseBasicParsing | Out-Null
        $invalidOriginAccepted = $true
    } catch {
        if ($_.Exception.Response.StatusCode.value__ -ne 403) { throw }
    }
    Assert-Condition (-not $invalidOriginAccepted) "invalid Origin was accepted"
    $oversizedRequest = "POST /mcp HTTP/1.1`r`n" +
        "Host: 127.0.0.1:$HttpPort`r`n" +
        "Accept: application/json, text/event-stream`r`n" +
        "Content-Type: application/json`r`n" +
        "MCP-Protocol-Version: 2025-11-25`r`n" +
        "Content-Length: 8388609`r`n`r`n"
    $oversizedRaw = Send-RawHttp $HttpPort $oversizedRequest
    Assert-Condition ($oversizedRaw.StartsWith("HTTP/1.1 413 ")) "oversized HTTP body was not rejected with 413"
    Assert-Condition ($oversizedRaw.Contains("HTTP request body is too large")) "oversized HTTP error did not identify the size limit"
    Write-Output "http: PASS"
}
finally {
    if ($http -and -not $http.HasExited) { $http.Kill(); $http.WaitForExit() }
}

# A configured token protects loopback HTTP requests as well.
$authPort = $HttpPort + 2
$authHttp = Start-Child @("mcp", "--transport", "http", "--host", "127.0.0.1", "--port", ([string]$authPort), "--auth-token", "smoke-secret")
try {
    $authReady = $false
    for ($attempt = 0; $attempt -lt 30; ++$attempt) {
        if ($authHttp.HasExited) { break }
        $listeners = [System.Net.NetworkInformation.IPGlobalProperties]::GetIPGlobalProperties().GetActiveTcpListeners()
        if ($listeners | Where-Object { $_.Port -eq $authPort }) {
            $authReady = $true
            break
        }
        Start-Sleep -Milliseconds 100
    }
    Assert-Condition $authReady "token-protected HTTP server did not start"
    $authOrigin = "http://127.0.0.1:$authPort"
    $authNoToken = Invoke-WebRequest -Uri "http://127.0.0.1:$authPort/mcp" -Method Post -ContentType "application/json" -Headers (Http-Headers $authOrigin) -Body (Json-Line $initialize) -UseBasicParsing -SkipHttpErrorCheck
    Assert-Condition ($authNoToken.StatusCode -eq 401) "missing HTTP token was accepted"
    $authBadHeaders = Http-Headers $authOrigin
    $authBadHeaders["Authorization"] = "Bearer wrong-token"
    $authBadToken = Invoke-WebRequest -Uri "http://127.0.0.1:$authPort/mcp" -Method Post -ContentType "application/json" -Headers $authBadHeaders -Body (Json-Line $initialize) -UseBasicParsing -SkipHttpErrorCheck
    Assert-Condition ($authBadToken.StatusCode -eq 401) "invalid HTTP token was accepted"
    $authGoodHeaders = Http-Headers $authOrigin
    $authGoodHeaders["Authorization"] = "Bearer smoke-secret"
    $authGood = Invoke-WebRequest -Uri "http://127.0.0.1:$authPort/mcp" -Method Post -ContentType "application/json" -Headers $authGoodHeaders -Body (Json-Line $initialize) -UseBasicParsing
    Assert-Condition ($authGood.StatusCode -eq 200) "valid HTTP token was rejected"
    Write-Output "http-auth: PASS"
}
finally {
    if ($authHttp -and -not $authHttp.HasExited) { $authHttp.Kill(); $authHttp.WaitForExit() }
}

# Plaintext remote binds are refused even when a bearer token is supplied.
$remote = Start-Child @("mcp", "--transport", "http", "--host", "0.0.0.0", "--port", ([string]($HttpPort + 1)))
$remoteErrorTask = $remote.StandardError.ReadToEndAsync()
$remoteOutput = $remote.StandardOutput.ReadToEnd()
$remoteError = $remoteErrorTask.GetAwaiter().GetResult()
$remote.WaitForExit()
Assert-Condition ($remote.ExitCode -eq 1) "unauthenticated remote HTTP bind was accepted"
Assert-Condition ($remoteError.Contains("requires HTTPS termination")) "remote bind rejection did not explain the HTTPS requirement"
Write-Output "remote-auth-guard: PASS"
