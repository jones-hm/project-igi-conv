#requires -Version 7.0

[CmdletBinding()]
param(
    [string]$GamePath = 'D:\IGI1',
    [string]$Executable = (Join-Path $PSScriptRoot '..\bin\Release\igi1conv.exe'),
    [string]$ArtifactRoot = (Join-Path $PSScriptRoot '..\tests_temp'),
    [switch]$KeepArtifacts
)

$ErrorActionPreference = 'Stop'

function Assert-Condition([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function Require-File([string]$Path, [string]$Label) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Missing live-test ${Label}: $Path"
    }
    return (Resolve-Path -LiteralPath $Path).Path
}

function Require-Directory([string]$Path, [string]$Label) {
    if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
        throw "Missing live-test ${Label}: $Path"
    }
    return (Resolve-Path -LiteralPath $Path).Path
}

function Invoke-ProcessCase {
    param(
        [Parameter(Mandatory)]$Case,
        [Parameter(Mandatory)][string]$Root,
        [Parameter(Mandatory)][string]$WorkingDirectory,
        [Parameter(Mandatory)][string]$Program
    )

    $started = [DateTime]::UtcNow
    $psi = [Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = $Program
    $psi.WorkingDirectory = $WorkingDirectory
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    foreach ($argument in @($Case.Args)) {
        [void]$psi.ArgumentList.Add([string]$argument)
    }

    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $psi
    $stdout = ''
    $stderr = ''
    $exitCode = $null
    $error = $null
    try {
        if (-not $process.Start()) { throw "Process did not start" }
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit([int]$Case.TimeoutSeconds * 1000)) {
            $process.Kill($true)
            $process.WaitForExit()
            throw "timed out after $($Case.TimeoutSeconds)s"
        }
        $stdout = $stdoutTask.Result
        $stderr = $stderrTask.Result
        $exitCode = $process.ExitCode
    } catch {
        $error = $_.Exception.Message
        if ($process.HasExited) {
            $exitCode = $process.ExitCode
            $stdout = $stdoutTask.Result
            $stderr = $stderrTask.Result
        }
    } finally {
        $process.Dispose()
    }

    $expected = @($Case.ExpectedExit)
    $exitOk = ($null -ne $exitCode -and $expected -contains [int]$exitCode)
    $outputsOk = $true
    $missingOutputs = @()
    foreach ($output in @($Case.Outputs)) {
        if (-not (Test-Path -LiteralPath $output)) {
            $outputsOk = $false
            $missingOutputs += $output
            continue
        }
        if ((Get-Item -LiteralPath $output).PSIsContainer) {
            if (-not (Get-ChildItem -LiteralPath $output -File -Recurse -ErrorAction SilentlyContinue)) {
                $outputsOk = $false
                $missingOutputs += "$output (empty directory)"
            }
        } elseif ((Get-Item -LiteralPath $output).Length -eq 0) {
            $outputsOk = $false
            $missingOutputs += "$output (empty file)"
        }
    }

    [pscustomobject]@{
        Name = $Case.Name
        Registry = $Case.Registry
        Category = $Case.Category
        ExpectedExit = ($expected -join ',')
        ExitCode = $exitCode
        ExitMatched = $exitOk
        OutputsPresent = $outputsOk
        MissingOutputs = ($missingOutputs -join '; ')
        DurationMs = [int](([DateTime]::UtcNow - $started).TotalMilliseconds)
        Error = $error
        Stdout = $stdout.Substring(0, [Math]::Min(2048, $stdout.Length))
        Stderr = $stderr.Substring(0, [Math]::Min(2048, $stderr.Length))
        Pass = ($null -eq $error -and $exitOk -and $outputsOk)
    }
}

function Add-Case {
    param(
        [Parameter(Mandatory)][AllowEmptyCollection()][System.Collections.Generic.List[object]]$List,
        [Parameter(Mandatory)][string]$Name,
        [Parameter(Mandatory)][AllowEmptyString()][string]$Registry,
        [Parameter(Mandatory)][string]$Category,
        [Parameter(Mandatory)][string[]]$Args,
        [string[]]$Outputs = @(),
        [int[]]$ExpectedExit = @(0),
        [int]$TimeoutSeconds = 120
    )
    $List.Add([pscustomobject]@{
        Name = $Name
        Registry = $Registry
        Category = $Category
        Args = $Args
        Outputs = $Outputs
        ExpectedExit = $ExpectedExit
        TimeoutSeconds = $TimeoutSeconds
    })
}

if (-not (Test-Path -LiteralPath $Executable -PathType Leaf)) {
    throw "Converter executable not found: $Executable"
}
$GamePath = Require-Directory $GamePath 'game directory'
$Executable = (Resolve-Path -LiteralPath $Executable).Path

$stamp = [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss')
New-Item -ItemType Directory -Path $ArtifactRoot -Force | Out-Null
$artifactBase = Convert-Path -LiteralPath $ArtifactRoot
$artifactRoot = Join-Path $artifactBase "igi1conv-live-$stamp-$([guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Path $artifactRoot | Out-Null
$results = [System.Collections.Generic.List[object]]::new()
$mcpResults = [System.Collections.Generic.List[object]]::new()

try {
    $commonDat = Require-File (Join-Path $GamePath 'COMMON\COMMON.DAT') 'DAT sample'
    $commonMtp = Require-File (Join-Path $GamePath 'COMMON\COMMON.MTP') 'MTP sample'
    $font = Require-File (Join-Path $GamePath 'COMPUTER\COMPUTER_unpacked\font1.fnt') 'FNT sample'
    $graph = Require-File (Join-Path $GamePath 'editor\backup\level1\graphs\graph1.dat') 'graph sample'
    $iff = Require-File (Join-Path $GamePath 'COMMON\ANIMS\000.IFF') 'IFF sample'
    $model = Require-File (Join-Path $GamePath 'content\models\level1\210_01_1.mef') 'MEF sample'
    $qsc = Require-File (Join-Path $GamePath 'MISSIONS\location0\level1\objects.qsc') 'QSC sample'
    $lightQsc = Require-File (Join-Path $GamePath 'editor\backup\level1\objects.qsc') 'lightmap QSC sample'
    $qvm = Require-File (Join-Path $GamePath 'config.qvm') 'QVM sample'
    $tex = Require-File (Join-Path $GamePath 'COMMON\SPRITES\binoculars1.tex') 'TEX sample'
    $spr = Require-File (Join-Path $GamePath 'COMMON\SPRITES\alarmlight.spr') 'SPR sample'
    $pic = Require-File (Join-Path $GamePath 'menusystem\loadingscreen_unpacked\loading_us.pic') 'PIC sample'
    $lmp = Require-File (Join-Path $GamePath 'editor\backup\level1\terrain\terrain.lmp') 'LMP sample'
    $ctr = Require-File (Join-Path $GamePath 'editor\backup\level1\terrain\terrain.ctr') 'CTR sample'
    $olm = Require-File (Join-Path $GamePath 'editor\backup\level1\lightmaps\lightmaps_unpacked\obj00000_00000.olm') 'OLM sample'
    $wav = Require-File (Join-Path $GamePath 'MISSIONS\location0\level1\sounds\m1_ambience.wav') 'IGI WAV sample'
    $wavDir = Require-Directory (Join-Path $GamePath 'MISSIONS\location0\level1\sounds') 'IGI WAV directory'
    $res = Require-File (Join-Path $GamePath 'COMMON\SPRITES\SPRITES.RES') 'RES sample'
    $texDir = Require-Directory (Join-Path $GamePath 'COMMON\TEXTURES') 'texture directory'

    $stage = Join-Path $artifactRoot 'outputs'
    New-Item -ItemType Directory -Path $stage | Out-Null
    $datJson = Join-Path $stage 'common.json'
    $datMtp = Join-Path $stage 'common-from-dat.mtp'
    $fontPng = Join-Path $stage 'font.png'
    $graphJson = Join-Path $stage 'graph.json'
    $graphMd = Join-Path $stage 'graph.md'
    $graphTable = Join-Path $stage 'graph-table.md'
    $iffBef = Join-Path $stage 'iff-bef'
    $iffDecoded = Join-Path $stage 'iff-decoded'
    $iffCreated = Join-Path $stage 'created.iff'
    $iffQsc = Join-Path $stage 'anims.qsc'
    $iffRebuilt = Join-Path $stage 'rebuilt.iff'
    $iffGif = Join-Path $stage 'animation.gif'
    $lightModel = Require-File (Join-Path $GamePath 'editor\backup\level1\models\level1_unpacked\435_01_1.mef') 'lightmap MEF sample'
    $lightRecalc = @('--model', '435_01_1', '--qsc', $lightQsc, '--task-id', '1104', '--mef', $lightModel,
                     '--sun-dir', '0.2,-0.8,-0.5', '--sun-color', '0.6,0.6,0.6', '--ambient', '0.3,0.3,0.3')
    $mefObj = Join-Path $stage 'model.obj'
    $mefDump = Join-Path $stage 'model.dump.txt'
    $mefText = Join-Path $stage 'model.mef.txt'
    $mefCompiled = Join-Path $stage 'model-compiled.mef'
    $mefBundle = Join-Path $stage 'mef-bundle'
    $mefRigid = Join-Path $stage 'model-rigid.mef'
    $mtpDump = Join-Path $stage 'common-mtp.json'
    $mtpDat = Join-Path $stage 'common-from-mtp.dat'
    $mtpRepair = Join-Path $stage 'repair.mtp'
    $mtpSync = Join-Path $stage 'sync.mtp'
    Copy-Item -LiteralPath $commonMtp -Destination $mtpRepair
    Copy-Item -LiteralPath $commonMtp -Destination $mtpSync
    $olmPng = Join-Path $stage 'lightmap.png'
    $olmTga = Join-Path $stage 'lightmap.tga'
    $olmRoundtrip = Join-Path $stage 'lightmap-roundtrip.olm'
    $qscCompiled = Join-Path $stage 'objects.qvm'
    $qscEdited = Join-Path $stage 'objects-edited.qsc'
    $qvmQsc = Join-Path $stage 'config.qsc'
    $qvmDisasm = Join-Path $stage 'config.disasm.txt'
    $resExtract = Join-Path $stage 'res-extract'
    $resUnpack = Join-Path $stage 'res-unpack'
    $resPacked = Join-Path $stage 'packed.res'
    $resRepacked = Join-Path $stage 'repacked.res'
    $resAppended = Join-Path $stage 'appended.res'
    $resResourceQsc = Join-Path $stage 'resource.qsc'
    $terrainPgm = Join-Path $stage 'terrain.pgm'
    $terrainJson = Join-Path $stage 'terrain.json'
    $texDecoded = Join-Path $stage 'tex-decoded'
    $texPng = Join-Path $stage 'texture.png'
    $texTga = Join-Path $stage 'texture.tga'
    $texSpr = Join-Path $stage 'texture.spr'
    $wavOut = Join-Path $stage 'sound.wav'
    $wavDirOut = Join-Path $stage 'wav-converted'

    $cases = [System.Collections.Generic.List[object]]::new()
    Add-Case $cases 'dat.info' 'dat.info' 'dat' @('dat','info',$commonDat)
    Add-Case $cases 'dat.export' 'dat.export' 'dat' @('dat','export',$commonDat,'-o',$datJson) @($datJson)
    Add-Case $cases 'dat.to-mtp' 'dat.to-mtp' 'dat' @('dat','to-mtp',$commonDat,'-o',$datMtp) @($datMtp)
    Add-Case $cases 'fnt.info' 'fnt.info' 'fnt' @('fnt','info',$font)
    Add-Case $cases 'fnt.export' 'fnt.export' 'fnt' @('fnt','export',$font,'-o',$fontPng) @($fontPng)
    Add-Case $cases 'graph.info' 'graph.info' 'graph' @('graph','info',$graph)
    Add-Case $cases 'graph.dump' 'graph.dump' 'graph' @('graph','dump',$graph)
    Add-Case $cases 'graph.export' 'graph.export' 'graph' @('graph','export',$graph,'-o',$graphJson) @($graphJson)
    Add-Case $cases 'graph.md' 'graph.md' 'graph' @('graph','md',$graph,'-o',$graphMd) @($graphMd)
    Add-Case $cases 'graph.table' 'graph.table' 'graph' @('graph','table',$graph,'-o',$graphTable) @($graphTable)
    Add-Case $cases 'iff.info' 'iff.info' 'iff' @('iff','info',$iff)
    Add-Case $cases 'iff.test' 'iff.test' 'iff' @('iff','test',$iff)
    Add-Case $cases 'iff.convert' 'iff.convert' 'iff' @('iff','convert',$iff,$iffBef) @($iffBef)
    Add-Case $cases 'iff.create' 'iff.create' 'iff' @('iff','create',$iffBef,$iffCreated) @($iffCreated)
    Add-Case $cases 'iff.decompile' 'iff.decompile' 'iff' @('iff','decompile',$iff,$iffDecoded) @($iffDecoded)
    Add-Case $cases 'iff.emit-qsc' 'iff.emit-qsc' 'iff' @('iff','emit-qsc',$iffBef,$iffQsc) @($iffQsc)
    Add-Case $cases 'iff.rebuild' 'iff.rebuild' 'iff' @('iff','rebuild',$iff,$iffRebuilt) @($iffRebuilt)
    Add-Case $cases 'iff.export-gif' '' 'iff-extra' @('iff','export-gif',$iff,$iffGif) @($iffGif) @('0','3') 300
    Add-Case $cases 'lightmap.list' 'lightmap.list' 'lightmap' @('lightmap','list','--model','435_01_1','--qsc',$lightQsc)
    Add-Case $cases 'lightmap.resolve' 'lightmap.resolve' 'lightmap' @('lightmap','resolve','--model','435_01_1','--qsc',$lightQsc,'--task-id','1104')
    # lightmap.recalc edits resolved .olm files in place and is intentionally
    # CLI-only; MCP must not expose a mutator without a distinct output mode.
    Add-Case $cases 'lightmap.recalc' '' 'lightmap' (@('lightmap','recalc') + $lightRecalc) @() @(3)
    Add-Case $cases 'mef.info' 'mef.info' 'mef' @('mef','info',$model)
    Add-Case $cases 'mef.dump' 'mef.dump' 'mef' @('mef','dump',$model,'-o',$mefDump) @($mefDump)
    Add-Case $cases 'mef.export' 'mef.export' 'mef' @('mef','export',$model,'-o',$mefObj) @($mefObj)
    Add-Case $cases 'mef.to-text' 'mef.to-text' 'mef' @('mef','to-text',$model,'-o',$mefText) @($mefText)
    Add-Case $cases 'mef.compile' 'mef.compile' 'mef' @('mef','compile',$mefText,'-o',$mefCompiled) @($mefCompiled) @('0','3') 180
    Add-Case $cases 'mef.build-rigid' 'mef.build-rigid' 'mef' @('mef','build-rigid',$model,'-o',$mefRigid) @($mefRigid) @('0','3') 180
    Add-Case $cases 'mef.bundle' 'mef.bundle' 'mef' @('mef','bundle',$model,'-o',$mefBundle,'--dat',$commonDat,'--texdir',$texDir,'--no-obj') @($mefBundle) @('0','3') 180
    Add-Case $cases 'mtp.info' 'mtp.info' 'mtp' @('mtp','info',$commonMtp)
    Add-Case $cases 'mtp.dump' 'mtp.dump' 'mtp' @('mtp','dump',$commonMtp,'-o',$mtpDump) @($mtpDump)
    # These repair/sync commands are also in-place CLI operations and are not
    # part of the MCP registry.
    Add-Case $cases 'mtp.repair' '' 'mtp' @('mtp','repair',$mtpRepair) @($mtpRepair)
    Add-Case $cases 'mtp.sync' '' 'mtp' @('mtp','sync',$mtpSync,$commonDat) @($mtpSync)
    Add-Case $cases 'mtp.to-dat' 'mtp.to-dat' 'mtp' @('mtp','to-dat',$commonMtp,'-o',$mtpDat) @($mtpDat)
    Add-Case $cases 'olm.info' 'olm.info' 'olm' @('olm','info',$olm)
    Add-Case $cases 'olm.to-png' 'olm.to-png' 'olm' @('olm','to-png',$olm,'-o',$olmPng) @($olmPng)
    Add-Case $cases 'olm.to-tga' 'olm.to-tga' 'olm' @('olm','to-tga',$olm,'-o',$olmTga) @($olmTga)
    Add-Case $cases 'olm.from-png' 'olm.from-png' 'olm' @('olm','from-png',$olmPng,'-o',$olmRoundtrip,'--template',$olm) @($olmRoundtrip)
    Add-Case $cases 'qsc.validate' 'qsc.validate' 'qsc' @('qsc','validate',$qsc)
    Add-Case $cases 'qsc.compile' 'qsc.compile' 'qsc' @('qsc','compile',$qsc,'-o',$qscCompiled) @($qscCompiled)
    Add-Case $cases 'qsc.list-objects' 'qsc.list-objects' 'qsc' @('qsc','list-objects',$qsc,'--json')
    Add-Case $cases 'qsc.edit-object' 'qsc.edit-object' 'qsc' @('qsc','edit-object',$lightQsc,'-o',$qscEdited,'--id','1104','--set','3=24658471') @($qscEdited)
    Add-Case $cases 'qvm.info' 'qvm.info' 'qvm' @('qvm','info',$qvm)
    Add-Case $cases 'qvm.disasm' 'qvm.disasm' 'qvm' @('qvm','disasm',$qvm,'-o',$qvmDisasm) @($qvmDisasm)
    Add-Case $cases 'qvm.decompile' 'qvm.decompile' 'qvm' @('qvm','decompile',$qvm,'-o',$qvmQsc) @($qvmQsc)
    Add-Case $cases 'res.list' 'res.list' 'res' @('res','list',$res)
    Add-Case $cases 'res.extract' 'res.extract' 'res' @('res','extract',$res,'-o',$resExtract) @($resExtract) @('0','3') 180
    Add-Case $cases 'res.unpack' 'res.unpack' 'res' @('res','unpack',$res,$resUnpack) @($resUnpack) 0 180
    Add-Case $cases 'res.pack' 'res.pack' 'res' @('res','pack',$resUnpack,$resPacked) @($resPacked) @('0','3') 180
    Add-Case $cases 'res.compile' 'res.compile' 'res' @('res','compile',$resResourceQsc) @() @('0','2','3') 180
    Add-Case $cases 'res.repack' 'res.repack' 'res' @('res','repack',$res,$resUnpack,'-o',$resRepacked) @($resRepacked) @('0','3') 180
    Add-Case $cases 'res.append' 'res.append' 'res' @('res','append',$res,$font,'-o',$resAppended) @($resAppended) @('0','3') 180
    Add-Case $cases 'terrain.info-lmp' 'terrain.info' 'terrain' @('terrain','info',$lmp)
    Add-Case $cases 'terrain.info-ctr' 'terrain.info' 'terrain' @('terrain','info',$ctr)
    Add-Case $cases 'terrain.export-lmp' 'terrain.export-lmp' 'terrain' @('terrain','export-lmp',$lmp,'-o',$terrainPgm) @((Join-Path $stage 'terrain_0.pgm'))
    Add-Case $cases 'terrain.export-ctr' 'terrain.export-ctr' 'terrain' @('terrain','export-ctr',$ctr,'-o',$terrainJson) @($terrainJson)
    Add-Case $cases 'tex.info-tex' 'tex.info' 'tex' @('tex','info',$tex)
    Add-Case $cases 'tex.info-spr' 'tex.info' 'tex' @('tex','info',$spr)
    Add-Case $cases 'tex.info-pic' 'tex.info' 'tex' @('tex','info',$pic)
    Add-Case $cases 'tex.decode' 'tex.decode' 'tex' @('tex','decode',$tex,'-o',$texDecoded) @($texDecoded)
    Add-Case $cases 'tex.to-png' 'tex.to-png' 'tex' @('tex','to-png',$tex,'-o',$texPng) @($texPng)
    Add-Case $cases 'tex.to-tga' 'tex.to-tga' 'tex' @('tex','to-tga',$tex,'-o',$texTga) @($texTga)
    Add-Case $cases 'tex.to-spr' 'tex.to-spr' 'tex' @('tex','to-spr',$texPng,'-o',$texSpr) @($texSpr)
    Add-Case $cases 'wav.info' 'wav.info' 'wav' @('wav','info',$wav)
    Add-Case $cases 'wav.convert' 'wav.convert' 'wav' @('wav','convert',$wav,'-o',$wavOut) @($wavOut)
    Add-Case $cases 'wav.convert-dir' 'wav.convert-dir' 'wav' @('wav','convert-dir',$wavDir,'-o',$wavDirOut,'--no-recursive') @($wavDirOut) @('0','3') 180

    $protectedFiles = @($commonDat,$commonMtp,$font,$graph,$iff,$model,$qsc,$lightQsc,$qvm,$tex,$spr,$pic,$lmp,$ctr,$olm,$wav,$res)
    $beforeHashes = @{}
    foreach ($file in $protectedFiles) { $beforeHashes[$file] = (Get-FileHash -Algorithm SHA256 -LiteralPath $file).Hash }

    foreach ($case in $cases) {
        $result = Invoke-ProcessCase $case $artifactRoot $GamePath $Executable
        $results.Add($result)
        $mark = if ($result.Pass) { 'PASS' } else { 'FAIL' }
        Write-Output ("{0,-24} {1,-4} exit={2} expected={3} {4}ms" -f $case.Name,$mark,$result.ExitCode,$result.ExpectedExit,$result.DurationMs)
    }

    $afterHashes = @{}
    foreach ($file in $protectedFiles) { $afterHashes[$file] = (Get-FileHash -Algorithm SHA256 -LiteralPath $file).Hash }
    $mutated = @($protectedFiles | Where-Object { $beforeHashes[$_] -ne $afterHashes[$_] })
    if ($mutated.Count -gt 0) { throw "Live matrix detected mutation of game input: $($mutated -join ', ')" }

    # Replay every registry entry through a real MCP stdio process. The CLI
    # cases use the exact same operation allowlist and safe staged outputs.
    $mcpCases = @($cases | Where-Object { -not [string]::IsNullOrWhiteSpace($_.Registry) })
    $mcpProtectedFiles = @($protectedFiles + $mtpRepair + $mtpSync | Sort-Object -Unique)
    $mcpBeforeHashes = @{}
    foreach ($file in $mcpProtectedFiles) {
        $mcpBeforeHashes[$file] = (Get-FileHash -Algorithm SHA256 -LiteralPath $file).Hash
    }
    $mcpPsi = [Diagnostics.ProcessStartInfo]::new()
    $mcpPsi.FileName = $Executable
    $mcpPsi.WorkingDirectory = $GamePath
    $mcpPsi.UseShellExecute = $false
    $mcpPsi.CreateNoWindow = $true
    $mcpPsi.RedirectStandardInput = $true
    $mcpPsi.RedirectStandardOutput = $true
    $mcpPsi.RedirectStandardError = $true
    [void]$mcpPsi.ArgumentList.Add('mcp')
    [void]$mcpPsi.ArgumentList.Add('--transport')
    [void]$mcpPsi.ArgumentList.Add('stdio')
    $mcpProcess = [Diagnostics.Process]::new()
    $mcpProcess.StartInfo = $mcpPsi
    try {
        if (-not $mcpProcess.Start()) { throw 'MCP stdio process did not start' }
        $mcpOutTask = $mcpProcess.StandardOutput.ReadToEndAsync()
        $mcpErrTask = $mcpProcess.StandardError.ReadToEndAsync()
        $requestId = 1
        $initialize = [ordered]@{ jsonrpc='2.0'; id=$requestId; method='initialize'; params=[ordered]@{
                protocolVersion='2025-11-25'; capabilities=[ordered]@{}; clientInfo=[ordered]@{name='live-matrix';version='1'} } }
        $mcpProcess.StandardInput.WriteLine(($initialize | ConvertTo-Json -Compress -Depth 20)); $requestId++
        $mcpProcess.StandardInput.WriteLine(([ordered]@{jsonrpc='2.0';method='notifications/initialized'} | ConvertTo-Json -Compress));
        $mcpProcess.StandardInput.WriteLine(([ordered]@{jsonrpc='2.0';id=$requestId;method='tools/list'} | ConvertTo-Json -Compress)); $requestId++
        $mcpProcess.StandardInput.WriteLine(([ordered]@{jsonrpc='2.0';id=$requestId;method='resources/list'} | ConvertTo-Json -Compress)); $requestId++
        $mcpProcess.StandardInput.WriteLine(([ordered]@{jsonrpc='2.0';id=$requestId;method='resources/read';params=[ordered]@{uri='igi1conv://game-capabilities'}} | ConvertTo-Json -Compress -Depth 20)); $requestId++

        foreach ($case in $mcpCases) {
            $mcpArgs = @($case.Args | Select-Object -Skip 2)
            $params = [ordered]@{name='igi_game_command';arguments=[ordered]@{command=$case.Registry;args=$mcpArgs;working_directory=$GamePath}}
            $request = [ordered]@{jsonrpc='2.0';id=$requestId;method='tools/call';params=$params}
            $mcpProcess.StandardInput.WriteLine(($request | ConvertTo-Json -Compress -Depth 20)); $requestId++
        }
        $objectRequest = [ordered]@{jsonrpc='2.0';id=$requestId;method='tools/call';params=[ordered]@{
                name='igi_game_object_edit';arguments=[ordered]@{input_file=$lightQsc;output_file=(Join-Path $stage 'mcp-typed-edit.qsc');selector=[ordered]@{task_id=1104};updates=@([ordered]@{direct_index=3;literal='24658471'})}}}
        $mcpProcess.StandardInput.WriteLine(($objectRequest | ConvertTo-Json -Compress -Depth 20))
        $mcpProcess.StandardInput.Close()
        if (-not $mcpProcess.WaitForExit(300000)) { $mcpProcess.Kill($true); throw 'MCP stdio matrix timed out' }
        $mcpStdout = $mcpOutTask.Result
        $mcpStderr = $mcpErrTask.Result
        $mcpLines = @($mcpStdout -split "`r?`n" | Where-Object { $_.Trim().Length -gt 0 })
        foreach ($line in $mcpLines) {
            try { $mcpResults.Add(($line | ConvertFrom-Json -Depth 30)) } catch { throw "MCP emitted invalid JSON: $line" }
        }
        if ($mcpProcess.ExitCode -ne 0) { throw "MCP stdio exit code $($mcpProcess.ExitCode): $mcpStderr" }
    } finally {
        if (-not $mcpProcess.HasExited) { $mcpProcess.Kill($true); $mcpProcess.WaitForExit() }
        $mcpProcess.Dispose()
    }

    $toolList = $mcpResults | Where-Object { $_.id -eq 2 } | Select-Object -First 1
    $toolNames = @($toolList.result.tools | ForEach-Object name)
    if ($toolNames.Count -ne 2 -or $toolNames -notcontains 'igi_game_command' -or $toolNames -notcontains 'igi_game_object_edit') {
        throw "MCP tools/list did not expose exactly the two game-facing tools"
    }
    $commandTool = $toolList.result.tools | Where-Object { $_.name -eq 'igi_game_command' } | Select-Object -First 1
    $registeredOperations = @($commandTool.inputSchema.properties.command.enum |
        ForEach-Object { [string]$_ } | Sort-Object -Unique)
    $matrixOperations = @($mcpCases.Registry | Sort-Object -Unique)
    $missingMatrixOperations = @($registeredOperations | Where-Object { $_ -notin $matrixOperations })
    $unexpectedMatrixOperations = @($matrixOperations | Where-Object { $_ -notin $registeredOperations })
    if ($missingMatrixOperations.Count -gt 0 -or $unexpectedMatrixOperations.Count -gt 0) {
        throw "Live matrix registry mismatch; missing=$($missingMatrixOperations -join ',') unexpected=$($unexpectedMatrixOperations -join ',')"
    }
    $capability = $mcpResults | Where-Object { $_.id -eq 4 } | Select-Object -First 1
    if (-not [string]$capability.result.contents[0].text -or [string]$capability.result.contents[0].text -notmatch 'game-facing') {
        throw 'MCP game capabilities resource was missing or not game-facing'
    }
    $mcpCalls = @($mcpResults | Where-Object { $_.id -ge 5 })
    Assert-Condition ($mcpCalls.Count -eq ($mcpCases.Count + 1)) "MCP response count was $($mcpCalls.Count), expected $($mcpCases.Count + 1)"
    $mcpCommandCalls = @($mcpCalls | Select-Object -First $mcpCases.Count)
    $mcpCaseStatuses = [System.Collections.Generic.List[object]]::new()
    foreach ($index in 0..($mcpCases.Count - 1)) {
        $call = $mcpCommandCalls[$index]
        $exit = if ($call.result.structuredContent) { $call.result.structuredContent.exit_code } else { $null }
        $mcpOk = ($null -ne $exit -and @($mcpCases[$index].ExpectedExit) -contains [int]$exit)
        $mcpCaseStatuses.Add([pscustomobject]@{ Case = $mcpCases[$index]; Pass = $mcpOk; Exit = $exit })
    }
    $typed = $mcpResults | Where-Object { $_.id -eq ($requestId) } | Select-Object -First 1
    if (-not $typed.result.structuredContent -or $typed.result.isError) { throw 'MCP typed object edit did not succeed' }
    if (-not (Test-Path -LiteralPath (Join-Path $stage 'mcp-typed-edit.qsc'))) { throw 'MCP typed edit output missing' }

    $mcpAfterHashes = @{}
    foreach ($file in $mcpProtectedFiles) {
        $mcpAfterHashes[$file] = (Get-FileHash -Algorithm SHA256 -LiteralPath $file).Hash
    }
    $mcpMutated = @($mcpProtectedFiles | Where-Object { $mcpBeforeHashes[$_] -ne $mcpAfterHashes[$_] })
    if ($mcpMutated.Count -gt 0) { throw "MCP matrix detected mutation of game input: $($mcpMutated -join ', ')" }

    $cliCaseCount = @($results | Where-Object { $_.Registry }).Count
    $uniqueCliGroups = @($results | Where-Object { $_.Registry } | Group-Object Registry)
    $cliApplicable = $uniqueCliGroups.Count
    $cliPassed = @($uniqueCliGroups | Where-Object { @($_.Group | Where-Object { -not $_.Pass }).Count -eq 0 }).Count
    $mcpCaseCount = $mcpCases.Count
    $mcpApplicable = @($mcpCases.Registry | Sort-Object -Unique).Count
    $mcpPassedCases = @($mcpCaseStatuses | Where-Object Pass).Count
    $mcpUniqueGroups = @($mcpCaseStatuses | Group-Object { $_.Case.Registry })
    $mcpPassed = @($mcpUniqueGroups | Where-Object { @($_.Group | Where-Object { -not $_.Pass }).Count -eq 0 }).Count
    $cliRate = if ($cliApplicable) { [math]::Round(100.0 * $cliPassed / $cliApplicable, 2) } else { 0 }
    $mcpRate = if ($mcpApplicable) { [math]::Round(100.0 * $mcpPassed / $mcpApplicable, 2) } else { 0 }
    $report = [ordered]@{
        game_path = $GamePath
        executable = $Executable
        generated_utc = [DateTime]::UtcNow.ToString('o')
        protected_inputs_unchanged = ($mutated.Count -eq 0 -and $mcpMutated.Count -eq 0)
        cli = [ordered]@{ cases=$cliCaseCount; applicable=$cliApplicable; passed=$cliPassed; coverage_percent=$cliRate; results=$results }
        mcp_stdio = [ordered]@{ cases=$mcpCaseCount; passed_cases=$mcpPassedCases; applicable=$mcpApplicable; passed=$mcpPassed; coverage_percent=$mcpRate; tool_names=$toolNames }
        mcp_registry = [ordered]@{ registered=$registeredOperations; covered=$matrixOperations; missing=$missingMatrixOperations; unexpected=$unexpectedMatrixOperations }
        mcp_results = $mcpCaseStatuses
        artifact_root = $artifactRoot
    }
    $reportPath = Join-Path $artifactRoot 'live-matrix.json'
    $report | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath $reportPath -Encoding UTF8
    Write-Output "LIVE_CLI_CASES=$cliCaseCount"
    Write-Output "LIVE_CLI_COVERAGE=$cliPassed/$cliApplicable unique-operations ($cliRate%)"
    Write-Output "LIVE_MCP_CASES=$mcpCaseCount"
    Write-Output "LIVE_MCP_CASE_PASS=$mcpPassedCases/$mcpCaseCount"
    Write-Output "LIVE_MCP_COVERAGE=$mcpPassed/$mcpApplicable unique-operations ($mcpRate%)"
    Write-Output "LIVE_MCP_REGISTRY=$($registeredOperations.Count) registered/$($matrixOperations.Count) covered"
    Write-Output "LIVE_INPUT_INTEGRITY=$($mutated.Count -eq 0)"
    Write-Output "LIVE_REPORT=$reportPath"
    if ($cliRate -lt 100 -or $mcpRate -lt 100 -or $mcpPassedCases -ne $mcpCaseCount) { throw 'Live operation coverage is below the required 100% gate' }
} finally {
    if (-not $KeepArtifacts -and (Test-Path -LiteralPath $artifactRoot)) {
        Remove-Item -LiteralPath $artifactRoot -Recurse -Force
    }
}
