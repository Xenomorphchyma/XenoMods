param(
    [string]$MainRoot = '',
    [string]$TestRoot = '',
    [string]$WorkspaceRoot = $env:SRHD_WORKSPACE_ROOT
)

$ErrorActionPreference = 'Stop'

function Find-WorkspaceRoot([string]$StartPath) {
    if(-not [string]::IsNullOrWhiteSpace($WorkspaceRoot)) {
        return [IO.Path]::GetFullPath($WorkspaceRoot)
    }
    $cursor = [IO.DirectoryInfo](Get-Item -LiteralPath $StartPath)
    while($null -ne $cursor) {
        if(Test-Path -LiteralPath (Join-Path $cursor.FullName 'Tools\SRHDModKit\srhd.py') -PathType Leaf) {
            return $cursor.FullName
        }
        $cursor = $cursor.Parent
    }
    throw 'SRHD workspace root was not found; pass -WorkspaceRoot or set SRHD_WORKSPACE_ROOT.'
}

$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$resolvedWorkspaceRoot = Find-WorkspaceRoot $scriptRoot
if([string]::IsNullOrWhiteSpace($MainRoot)) {
    $MainRoot = Split-Path -Parent $scriptRoot
}
if([string]::IsNullOrWhiteSpace($TestRoot)) {
    $siblingTestRoot = Join-Path (Split-Path -Parent $MainRoot) 'XenoDomRangersTest'
    $workspaceTestRoot = Join-Path $resolvedWorkspaceRoot 'Projects\ModWorkspaces\XenoDomRangersTest'
    $TestRoot = if(Test-Path -LiteralPath $siblingTestRoot -PathType Container) {
        $siblingTestRoot
    } else {
        $workspaceTestRoot
    }
}

function Read-CodeLines([string]$Path) {
    return [IO.File]::ReadAllLines($Path, [Text.UTF8Encoding]::new($false))
}

function Remove-BracedCode([string[]]$Lines, [int]$StartIndex) {
    $index = $StartIndex
    $depth = 0
    $started = $false
    while($index -lt $Lines.Count) {
        $openCount = ([regex]::Matches($Lines[$index], '\{')).Count
        $closeCount = ([regex]::Matches($Lines[$index], '\}')).Count
        if($openCount -gt 0) { $started = $true }
        $depth += $openCount - $closeCount
        if($started -and $depth -le 0) { return $index }
        $index++
    }
    throw "Unterminated test-only block starting at source line $($StartIndex + 1)"
}

function Remove-TestOnlyCode([string[]]$Lines) {
    $result = [Collections.Generic.List[string]]::new()
    for($index = 0; $index -lt $Lines.Count; $index++) {
        $line = $Lines[$index]

        if($line -match '^\s*function\s+XDR_Test') {
            $index = Remove-BracedCode $Lines $index
            continue
        }

        if($line -match '^\s*if\s*\(\s*xdr_test_mode(?:\s*&&[^)]*)?\s*\)') {
            $afterCondition = $line.Substring($line.LastIndexOf(')') + 1).Trim()
            if($afterCondition -ne '' -and $afterCondition -ne '{') { continue }
            $index = Remove-BracedCode $Lines $index
            continue
        }

        if($line -match '^\s*if\s*\(\s*!xdr_test_mode\s*\)\s*exit;') {
            $result.Add(($line -replace 'if\s*\(\s*!xdr_test_mode\s*\)\s*', ''))
            continue
        }

        $line = $line -replace '\s*&&\s*!xdr_test_mode', ''
        $line = $line -replace '!xdr_test_mode\s*&&\s*', ''
        if($line -match '\bxdr_test_' -or $line -match '\bxdr_diag_') { continue }
        if($line -match '\bxdr_test_mode\b') {
            throw "Production source still contains xdr_test_mode at source line $($index + 1): $line"
        }
        $result.Add($line)
    }
    return [string[]]$result
}

function New-BaseObject([string]$Type, [string]$Name, [int]$Id, [int]$X, [int]$Y) {
    return [ordered]@{
        Type = $Type
        Name = $Name
        'Pos.x' = $X
        'Pos.y' = $Y
        Parent = -1
        '#' = $Id
    }
}

function New-Variable([string]$Name, [string]$VarType, [string]$Init, [int]$Id, [int]$Y) {
    $v = New-BaseObject 'TVar' $Name $Id 20 $Y
    $v['Var.Type'] = $VarType
    $v.Init = $Init
    $v.Global = $false
    return $v
}

function New-Operation([string]$Name, [string]$CodeType, [string[]]$Code, [int]$Id, [int]$X, [int]$Y) {
    $op = New-BaseObject 'Top' $Name $Id $X $Y
    $op['Code.Type'] = $CodeType
    $op.Code = @($Code)
    return $op
}

function New-Dialog([string]$Name, [int]$Id, [int]$X, [int]$Y) {
    return (New-BaseObject 'TDialog' $Name $Id $X $Y)
}

function New-DialogMessage([int]$Id, [int]$X, [int]$Y, [int]$Number) {
    $msg = New-BaseObject 'TDialogMsg' '' $Id $X $Y
    $msg.Msg = ''
    $msg['DMsg.Num'] = [string]$Number
    return $msg
}

function New-Link([int]$Begin, [int]$End) {
    return [ordered]@{ Type = 'TGraphLink'; Begin = $Begin; End = $End; Nom = 0; Arrow = $true }
}

function New-XdrRson([string]$Root, [string]$ScriptName, [bool]$TestMode) {
    $sourceRoot = Join-Path $MainRoot 'SOURCE'
    $globalFunctions = Read-CodeLines (Join-Path $sourceRoot 'Mod_XenoDomRangers.global.txt')
    # Mod_XenoDomRangers.global.txt is the authoritative complete operation #6
    # source and already contains the turn handler after the function block.
    # Appending the legacy turn fragment duplicates declarations in one RScript
    # scope and can hang old runtimes even though the JSON remains valid.
    $turnCode = if($TestMode) { @($globalFunctions) } else { @(Remove-TestOnlyCode $globalFunctions) }
    if(-not $TestMode) {
        $productionCode = $turnCode -join "`n"
        $forbiddenProductionSymbols = @(
            'xdr_test_', 'xdr_test_mode', 'XDR_Test', 'xdr_diag_',
            'xdr_relation_', 'xdr_player_warning_ship_id', 'xdr_process_cursor',
            'XDRMaskGuard'
        )
        foreach($forbiddenProductionSymbol in $forbiddenProductionSymbols) {
            if($productionCode -match [regex]::Escape($forbiddenProductionSymbol)) {
                throw "Production source contains forbidden test/dead symbol: $forbiddenProductionSymbol"
            }
        }
    }

    $groups = [Collections.Generic.List[object]]::new()
    $operations = [Collections.Generic.List[object]]::new()
    $planets = [Collections.Generic.List[object]]::new()
    $ships = [Collections.Generic.List[object]]::new()
    $stars = [Collections.Generic.List[object]]::new()
    $states = [Collections.Generic.List[object]]::new()
    $statements = [Collections.Generic.List[object]]::new()
    $variables = [Collections.Generic.List[object]]::new()
    $dialogs = [Collections.Generic.List[object]]::new()
    $dialogMessages = [Collections.Generic.List[object]]::new()
    $links = [Collections.Generic.List[object]]::new()

    $player = New-BaseObject 'TStarShip' '' 0 760 380
    $player.Count = 1; $player.Owner = 62; $player['Ship.Type'] = 126; $player.Player = $true
    $player.SpeedMin = 0; $player.SpeedMax = 10000; $player.Weapon = 0; $player.CargoHook = 0; $player.EmptySpace = 0
    $player.StatusTraderMin = 0; $player.StatusTraderMax = 100; $player.StatusWarriorMin = 0; $player.StatusWarriorMax = 100
    $player.StatusPirateMin = 0; $player.StatusPirateMax = 100; $player.StrengthMin = '0'; $player.StrengthMax = '0'; $player.Ruins = ''
    $ships.Add($player)

    $group = New-BaseObject 'TGroup' 'XenoDomSpecialists' 1 760 470
    $group.Owner = 62; $group['Group.Type'] = 126; $group.CntShipMin = 0; $group.CntShipMax = 0
    $group.SpeedMin = 100; $group.SpeedMax = 10000; $group.Weapon = 0; $group.CargoHook = 0; $group.EmptySpace = 0
    $group.AddPlayer = $false; $group.StatusTraderMin = 0; $group.StatusTraderMax = 100
    $group.StatusWarriorMin = 0; $group.StatusWarriorMax = 100; $group.StatusPirateMin = 0; $group.StatusPirateMax = 100
    $group.DistSearch = 10000; $group.Dialog = -1; $group.StrengthMin = '0'; $group.StrengthMax = '0'; $group.Ruins = ''
    $groups.Add($group)

    $state = New-BaseObject 'TState' 'XenoDomState' 2 760 500
    $state.Move = 0; $state.MoveObj = -1; $state['Attack.Count'] = 0; $state.TakeItem = -1; $state.TakeAllItem = $false
    $state.OnTalk = ''
    # The event header lives in the canonical file. Keeping one complete source
    # prevents a rebuild from silently losing subscriptions or restoring an old
    # unsafe action-object handler.
    $state.OnActCode = Get-Content -Raw -Encoding UTF8 -LiteralPath (Join-Path $PSScriptRoot 'XenoDomState.onact.txt')
    $requiredNpcEvents = @('t_OnStep','t_OnChameleonConfusion','t_OnWeaponShot','t_OnMissileShot','t_OnGettingWeaponHit','t_OnGettingMissileHit')
    foreach($requiredNpcEvent in $requiredNpcEvents) {
        if($state.OnActCode -notmatch [regex]::Escape($requiredNpcEvent)) {
            throw "XenoDomState is missing required event $requiredNpcEvent"
        }
    }
    if($state.OnActCode -match 'ScriptItemActObject[12]\(\)\s*;\s*(?:\r?\n)?\s*if\([^\r\n]*(?:WeaponTarget|MissileLive|ShipInCurScript)') {
        throw 'XenoDomState must not dereference an untyped weapon or missile action object'
    }
    $state.EType = 1; $state.EUnique = ''; $state.EMsg = ''
    $states.Add($state)

    $planet = New-BaseObject 'TPlanet' 'PlanetAnchor' 3 760 440
    $planet.Race = 62; $planet.Owner = 62; $planet.Economy = 14; $planet.Goverment = 62
    $planet.RangeMin = 0; $planet.RangeMax = 100; $planet.Dialog = -1
    $planets.Add($planet)

    $star = New-BaseObject 'TStar' 'StarAnchor' 4 760 410
    $star.Constellation = 0; $star.Priority = 0; $star.NoKling = $false; $star.NoComeKling = $false
    $stars.Add($star)

    $operations.Add((New-Operation 'XDRGlobal' 'Global' @('xdr_ui_ready = 0;','xdr_ui_ready_turn = 0;','GRun();') 5 860 350))
    $operations.Add((New-Operation 'XDRTurn' 'Turn' $turnCode 6 860 400))

    # RScript expects a Turn entry operation to be reached through a statement.
    # This also prevents the first unsafe tick before runtime object resolution.
    $generationBarrier = New-BaseObject 'Tif' 'GenerationBarrier' 7 830 400
    $generationBarrier['Code.Type'] = 'Turn'
    if($TestMode) {
        $generationBarrier.Code = [string[]]@('CurTurn() > 0')
    }
    else {
        $generationBarrier.Code = [string[]]@('CurTurn() > 0 && xdr_ui_ready')
    }
    $statements.Add($generationBarrier)

    $links.Add((New-Link 0 4)); $links.Add((New-Link 3 4)); $links.Add((New-Link 1 2)); $links.Add((New-Link 1 3)); $links.Add((New-Link 7 6))

    $runtimeGroup = New-BaseObject 'TGroup' 'RuntimePlayer' 8 940 470
    $runtimeGroup.Owner = 62; $runtimeGroup['Group.Type'] = 126; $runtimeGroup.CntShipMin = 1; $runtimeGroup.CntShipMax = 1
    $runtimeGroup.SpeedMin = 100; $runtimeGroup.SpeedMax = 10000; $runtimeGroup.Weapon = 0; $runtimeGroup.CargoHook = 0; $runtimeGroup.EmptySpace = 0
    $runtimeGroup.AddPlayer = $true; $runtimeGroup.StatusTraderMin = 0; $runtimeGroup.StatusTraderMax = 100
    $runtimeGroup.StatusWarriorMin = 0; $runtimeGroup.StatusWarriorMax = 100; $runtimeGroup.StatusPirateMin = 0; $runtimeGroup.StatusPirateMax = 100
    $runtimeGroup.DistSearch = 10000; $runtimeGroup.Dialog = -1; $runtimeGroup.StrengthMin = '0'; $runtimeGroup.StrengthMax = '0'; $runtimeGroup.Ruins = ''
    $groups.Add($runtimeGroup)

    $playerState = New-BaseObject 'TState' 'XenoDomPlayerState' 9 960 500
    $playerState.Move = 0; $playerState.MoveObj = -1; $playerState['Attack.Count'] = 0; $playerState.TakeItem = -1; $playerState.TakeAllItem = $false
    $playerState.OnTalk = ''
    # Player state has one canonical, UI-only source and never inspects weapon
    # or missile action objects.
    $playerState.OnActCode = "[t_OnEnteringForm|]`n" + (Get-Content -Raw -Encoding UTF8 -LiteralPath (Join-Path $PSScriptRoot 'XenoDomPlayerState.body.txt'))
    if($playerState.OnActCode -match 't_OnWeaponShot|t_OnMissileShot|ScriptItemActObject')
    {
        throw 'XenoDomPlayerState must not inspect weapon or missile action objects'
    }
    $playerState.EType = 1; $playerState.EUnique = ''; $playerState.EMsg = ''
    $states.Add($playerState)

    # Player and specialist logic are separate states. This prevents an NPC-only
    # cleanup branch from ever mutating the player through a shared CurShip.
    $links.Add((New-Link 8 9)); $links.Add((New-Link 8 3))

    $varDefs = @(
        @('xdr_schema_version','Int','0'), @('xdr_initialized','Int','0'), @('xdr_ui_ready','Int','0'), @('xdr_ui_ready_turn','Int','0'), @('xdr_next_recruit_turn','Int','0'),
        @('xdr_disabled','Int','0'),
        @('xdr_ids','None',''), @('xdr_states','None',''), @('xdr_home_base_ids','None',''), @('xdr_target_star_ids','None',''),
        @('xdr_series','None',''), @('xdr_due_turns','None',''), @('xdr_phase_turns','None',''), @('xdr_pending_item_ids','None',''),
        @('xdr_pending_since','None',''), @('xdr_no_loot_counts','None',''), @('xdr_threat_counts','None',''), @('xdr_old_owners','None',''),
        @('xdr_old_standings','None',''), @('xdr_old_no_targets','None',''), @('xdr_old_factions','None',''), @('xdr_old_names','None',''),
        @('xdr_ghost_flags','None',''), @('xdr_visual_flags','None',''),
        @('xdr_charge_expire_turns','None',''), @('xdr_last_hull_damage','None',''), @('xdr_return_place_ids','None',''), @('xdr_return_kinds','None',''),
        @('xdr_release_flags','None',''), @('xdr_route_slots','None',''), @('xdr_route_orders','None',''), @('xdr_route_star_ids','None',''),
        @('xdr_scavenged_fuel_slots','None',''), @('xdr_scavenged_fuel_item_ids','None',''), @('xdr_dialog_markers','None','')
    )
    if($TestMode) {
        $varDefs += @(
            @('xdr_test_mode','Int','1'), @('xdr_test_spawn_stalker','Int','0'), @('xdr_test_force_recruit','Int','0'),
            @('xdr_test_no_bases','Int','0'), @('xdr_test_force_abort','Int','0'), @('xdr_test_force_mask_failure','Int','0'),
            @('xdr_test_release_all','Int','0'), @('xdr_test_ranger_cursor','Int','0'), @('xdr_test_last_event','Str',''),
            @('xdr_test_quick_stage','Int','0'), @('xdr_test_quick_source_star_id','Int','0'), @('xdr_test_quick_target_star_id','Int','0'),
            @('xdr_test_quick_ranger_id','Int','0'), @('xdr_test_quick_loot_id','Int','0'), @('xdr_test_quick_slot','Int','0'),
            @('xdr_test_quick_fuel_mode','Int','0'), @('xdr_diag_direct_pickups','Int','0')
        )
    }
    $nextId = 10
    $varY = 540
    foreach($def in $varDefs) {
        $variables.Add((New-Variable $def[0] $def[1] $def[2] $nextId $varY))
        $nextId++; $varY += 20
    }
    $dialogKeyPrefix = "Script.$ScriptName"

    # Replace the vanilla ranger conversation only while the script's own
    # per-ship mask marker is active. Native camouflage never changes faction.
    $maskedDialogBeginCode = @(
        "function XDR_DialogSlotByShip(dword dialog_checked_ship)",
        "{",
        "    result = 0;",
        "    if(!dialog_checked_ship || ArrayDim(xdr_ids) != 4) exit;",
        "    int dialog_checked_id = Id(dialog_checked_ship);",
        "    if(xdr_ids[1] == dialog_checked_id) { result = 1; exit; }",
        "    if(xdr_ids[2] == dialog_checked_id) { result = 2; exit; }",
        "    if(xdr_ids[3] == dialog_checked_id) { result = 3; exit; }",
        "}",
        "if(!TalkByAI())",
        "{",
        "    dword xdr_talk_ship = GetTalkShip();",
        "    if(xdr_talk_ship)",
        "    {",
        "        if(ShipInCurScript(xdr_talk_ship))",
        "        {",
        "            if(ShipTypeN(xdr_talk_ship) == t_Ranger)",
        "            {",
                "                int xdr_talk_slot = XDR_DialogSlotByShip(xdr_talk_ship);",
                "                if(xdr_talk_slot > 0 && (xdr_dialog_markers[xdr_talk_slot] == 2 || xdr_dialog_markers[xdr_talk_slot] == 4))",
        "                {",
        "                    SkipGreeting();",
        "                    AddDialogOverride('XDRAttackedTalkDialog', 200001, 0);",
        "                }",
        "                else if(xdr_talk_slot > 0 && xdr_ghost_flags[xdr_talk_slot])",
        "                {",
        "                    SkipGreeting();",
        "                    AddDialogOverride('XDRMaskedTalkDialog', 200000, 0);",
        "                }",
        "                else",
        "                {",
        "                    SkipGreeting();",
        "                    AddDialogOverride('XDRMissionTalkDialog', 199999, 0);",
        "                }",
        "            }",
        "        }",
        "    }",
        "}"
    )
    $operations.Add((New-Operation 'XDRMaskedDialogBegin' 'DialogBegin' $maskedDialogBeginCode $nextId 940 350))
    $nextId++

    $maskedDialogId = $nextId; $nextId++
    $maskedMessageId = $nextId; $nextId++
    $maskedRootId = $nextId; $nextId++
    $maskedActionId = $nextId; $nextId++
    # DChange uses the ordinal number across all TDialogMsg objects in this
    # script, not a per-dialog index. Keep one cursor for both builds.
    $dialogMessageCursor = 0
    $maskedMessageNumber = $dialogMessageCursor; $dialogMessageCursor++
    $dialogs.Add((New-Dialog 'XDRMaskedTalkDialog' $maskedDialogId 940 440))
    $dialogMessages.Add((New-DialogMessage $maskedMessageId 940 490 $maskedMessageNumber))
    $operations.Add((New-Operation 'XDRMaskedTalkRoot' 'Turn' @("DChange($maskedMessageNumber);",'exit;') $maskedRootId 940 540))
    $operations.Add((New-Operation 'XDRMaskedTalkAction' 'Turn' @(
        "DText(CT('$dialogKeyPrefix.MaskedText'));",
        "DAnswer(CT('$dialogKeyPrefix.MaskedClose'));",
        'exit;'
    ) $maskedActionId 940 590))
    $links.Add((New-Link $maskedDialogId $maskedRootId))
    $links.Add((New-Link $maskedMessageId $maskedActionId))

    if($TestMode) {
        $dialogBeginCode = @(
            "if(!TalkByAI())",
            "{",
            "    dword xdr_test_dialog_place = GetShipRuins(Player());",
            "    if(xdr_test_dialog_place)",
            "    {",
            "        if(ShipType(xdr_test_dialog_place) == 'PlayerBridge')",
            "        {",
            "            AddDialogInject('XDRTestQuickDialog', '', CT('$dialogKeyPrefix.TestMenuQuickFuel'), 10003, 0, 0);",
            "            AddDialogInject('XDRTestQuickSolarDialog', '', CT('$dialogKeyPrefix.TestMenuQuickSolar'), 10002, 0, 0);",
            "            AddDialogInject('XDRTestQuickCleanupDialog', '', CT('$dialogKeyPrefix.TestMenuCleanup'), 10001, 0, 0);",
            "            AddDialogInject('XDRTestSpawnDialog', '', CT('$dialogKeyPrefix.TestMenuSpawn'), 10000, 0, 0);",
            "            AddDialogInject('XDRTestStatusDialog', '', CT('$dialogKeyPrefix.TestMenuStatus'), 9999, 0, 0);",
            "            AddDialogInject('XDRTestRecruitDialog', '', CT('$dialogKeyPrefix.TestMenuRecruit'), 9998, 0, 0);",
            "            AddDialogInject('XDRTestNoBasesDialog', '', CT('$dialogKeyPrefix.TestMenuNoBases'), 9997, 0, 0);",
            "            AddDialogInject('XDRTestAbortDialog', '', CT('$dialogKeyPrefix.TestMenuAbort'), 9996, 0, 0);",
            "            AddDialogInject('XDRTestMaskDialog', '', CT('$dialogKeyPrefix.TestMenuMask'), 9995, 0, 0);",
            "            AddDialogInject('XDRTestReleaseDialog', '', CT('$dialogKeyPrefix.TestMenuRelease'), 9994, 0, 0);",
            "        }",
            "    }",
            "}"
        )
        $operations.Add((New-Operation 'XDRTestDialogBegin' 'DialogBegin' $dialogBeginCode $nextId 1120 350))
        $nextId++

        $actions = @(
            @('Quick', @(
                "if(xdr_test_quick_stage > 0)",
                "{",
                "    DText('Быстрый тест уже выполняется. Этап=' + xdr_test_quick_stage + '. Закройте мост и пропускайте ходы. Подробности доступны в диагностике.');",
                "}",
                "else",
                "{",
                "    xdr_test_quick_stage = 1;",
                "    xdr_test_quick_fuel_mode = 0;",
                "    xdr_test_quick_source_star_id = 0;",
                "    xdr_test_quick_target_star_id = 0;",
                "    xdr_test_quick_ranger_id = 0;",
                "    xdr_test_quick_loot_id = 0;",
                "    xdr_test_quick_slot = 0;",
                "    xdr_test_last_event = 'запрошен быстрый тест вылазки';",
                "    DText('Закройте мост и пропускайте дни до завершения варпа. Будут созданы цистерна с топливом и два доминаторских обломка. Сталкер соберёт их, перельёт топливо в установленный бак, найдёт обратный маршрут и после возвращения будет передан ванильному ИИ. Камуфляж игрока не включается автоматически.');",
                "}",
                "DAnswer(CT('$dialogKeyPrefix.TestClose'));",
                "exit;"
            )),
            @('QuickSolar', @(
                "if(xdr_test_quick_stage > 0)",
                "{",
                "    DText('Быстрый тест уже выполняется. Этап=' + xdr_test_quick_stage + '.');",
                "}",
                "else",
                "{",
                "    xdr_test_quick_stage = 1;",
                "    xdr_test_quick_fuel_mode = 1;",
                "    xdr_test_quick_source_star_id = 0;",
                "    xdr_test_quick_target_star_id = 0;",
                "    xdr_test_quick_ranger_id = 0;",
                "    xdr_test_quick_loot_id = 0;",
                "    xdr_test_quick_slot = 0;",
                "    xdr_test_last_event = 'запрошен быстрый тест солнечной зарядки';",
                "    DText('Закройте мост и пропускайте дни до завершения варпа. Будут созданы два доминаторских обломка без топлива, а бак сталкера после входа станет пустым. После сбора он должен подойти к солнцу, зарядиться ванильной механикой, уйти по проверенному маршруту и вернуться к обычному ИИ.');",
                "}",
                "DAnswer(CT('$dialogKeyPrefix.TestClose'));",
                "exit;"
            )),
            @('QuickCleanup', @(
                "xdr_test_quick_stage = -1;",
                "xdr_test_last_event = 'запрошено снятие тестовой маскировки';",
                "DText('Закройте мост и пропустите один день. Тестовая маскировка игрока будет снята. Уже созданный сталкер продолжит свою миссию; для его освобождения используйте отдельную безопасную команду.');",
                "DAnswer(CT('$dialogKeyPrefix.TestClose'));",
                "exit;"
            )),
            @('Spawn', @(
                "if(xdr_test_spawn_stalker)",
                "{",
                "    DText('Запрос на спавн уже ожидает выполнения. Закройте мост и сделайте один ход.');",
                "}",
                "else",
                "{",
                "    xdr_test_spawn_stalker = 1;",
                "    xdr_test_last_event = 'запрошен спавн сталкера из панели корабля';",
                "    DText('Закройте мост и пропустите один день. Рейнджер-сталкер появится рядом с кораблём и уже на следующем ходу начнёт маршрут. Если пиратских баз нет, он будет ждать в неактивном состоянии.');",
                "}",
                "DAnswer(CT('$dialogKeyPrefix.TestClose'));",
                "exit;"
            )),
            @('Status', @(
                "if(!xdr_initialized)",
                "{",
                "    DText('Хранилище XenoDomRangersTest ещё не инициализировано. Закройте панель корабля и пропустите один день.');",
                "    DAnswer(CT('$dialogKeyPrefix.TestClose'));",
                "    exit;",
                "}",
                "int test_active = (xdr_ids[1] > 1) + (xdr_ids[2] > 1) + (xdr_ids[3] > 1);",
                "str test_slots = '<br>слот 1: id=' + xdr_ids[1] + ', состояние=' + xdr_states[1] + ', pending=' + xdr_pending_item_ids[1] + ', без лута=' + xdr_no_loot_counts[1] + ', база=' + xdr_home_base_ids[1] + ', цель=' + xdr_target_star_ids[1] + ', заряд=' + xdr_charge_expire_turns[1] + ', серия=' + xdr_series[1] + ', облик=' + xdr_visual_flags[1] + ', маска=' + xdr_ghost_flags[1];",
                "test_slots = test_slots + '<br>слот 2: id=' + xdr_ids[2] + ', состояние=' + xdr_states[2] + ', pending=' + xdr_pending_item_ids[2] + ', без лута=' + xdr_no_loot_counts[2] + ', база=' + xdr_home_base_ids[2] + ', цель=' + xdr_target_star_ids[2] + ', заряд=' + xdr_charge_expire_turns[2] + ', серия=' + xdr_series[2] + ', облик=' + xdr_visual_flags[2] + ', маска=' + xdr_ghost_flags[2];",
                "test_slots = test_slots + '<br>слот 3: id=' + xdr_ids[3] + ', состояние=' + xdr_states[3] + ', pending=' + xdr_pending_item_ids[3] + ', без лута=' + xdr_no_loot_counts[3] + ', база=' + xdr_home_base_ids[3] + ', цель=' + xdr_target_star_ids[3] + ', заряд=' + xdr_charge_expire_turns[3] + ', серия=' + xdr_series[3] + ', облик=' + xdr_visual_flags[3] + ', маска=' + xdr_ghost_flags[3];",
                "str test_quick_ship_info = '';",
                "dword test_quick_ship = 0;",
                "if(xdr_test_quick_ranger_id > 1) test_quick_ship = IdToShip(xdr_test_quick_ranger_id);",
                "if(test_quick_ship)",
                "{",
                "    dword test_quick_hook = ShipEqInSlot(test_quick_ship, t_CargoHook);",
                "    dword test_quick_tank = ShipEqInSlot(test_quick_ship, t_FuelTanks);",
                "    int test_quick_hook_size = 0;",
                "    int test_quick_tank_size = 0;",
                "    if(test_quick_hook) test_quick_hook_size = GetEquipmentStats(test_quick_hook, 0);",
                "    if(test_quick_tank) test_quick_tank_size = GetEquipmentStats(test_quick_tank, 0);",
                "    int test_quick_has_order_object = ShipOrderObj(test_quick_ship) != 0;",
                "    test_quick_ship_info = '<br>корабль быстрого теста: приказ=' + ShipOrder(test_quick_ship) + ', осталось=' + ShipTurnBeforeEndOrder(test_quick_ship) + ', объект приказа=' + test_quick_has_order_object + ', свободно=' + ShipFreeSpace(test_quick_ship) + ', захват=' + test_quick_hook_size + ', топливо=' + ShipFuel(test_quick_ship) + ' из ' + test_quick_tank_size;",
                "}",
                "int test_base_count = 0;",
                "if(!xdr_test_no_bases)",
                "{",
                "    for(int test_base_star_index = 0; test_base_star_index < GalaxyStars(); test_base_star_index = test_base_star_index + 1)",
                "    {",
                "        dword test_base_star = GalaxyStar(test_base_star_index);",
                "        if(!test_base_star) continue;",
                "        dword test_base = StarRuins(test_base_star, 'PB');",
                "        if(test_base) test_base_count = test_base_count + 1;",
                "    }",
                "}",
                "unknown test_native_ready_function = ImportedFunction('XenoDomRangersNative', 'XenoDomRangers_IsReady');",
                "int test_native_ready = test_native_ready_function();",
                "DText('XenoDomRangersTest<br>нативная маскировка=' + test_native_ready + ', активно=' + test_active + ', отключено=' + xdr_disabled + ', баз=' + test_base_count + ', быстрый тест=' + xdr_test_quick_stage + ', режим топлива=' + xdr_test_quick_fuel_mode + ', система старта=' + xdr_test_quick_source_star_id + ', цель=' + xdr_test_quick_target_star_id + ', сталкер=' + xdr_test_quick_ranger_id + ', лут=' + xdr_test_quick_loot_id + test_slots + test_quick_ship_info + '<br>прямых подборов=' + xdr_diag_direct_pickups + '<br>последнее событие: ' + xdr_test_last_event);",
                "DAnswer(CT('$dialogKeyPrefix.TestClose'));",
                "exit;"
            )),
            @('Recruit', @("xdr_test_force_recruit = 1;", "xdr_next_recruit_turn = CurTurn();", "xdr_test_last_event = 'запрошен принудительный набор';", "DText('Набор будет выполнен на следующем ходу.');", "DAnswer(CT('$dialogKeyPrefix.TestClose'));", "exit;")),
            @('NoBases', @("xdr_test_no_bases = 1 - xdr_test_no_bases;", "xdr_test_last_event = 'симуляция отсутствия баз=' + xdr_test_no_bases;", "DText('Симуляция отсутствия пиратских баз: ' + xdr_test_no_bases);", "DAnswer(CT('$dialogKeyPrefix.TestClose'));", "exit;")),
            @('Abort', @("xdr_test_force_abort = 1;", "xdr_test_last_event = 'принудительный отход подготовлен';", "DText('Следующая активная попытка сбора перейдёт в ветку осторожного отхода.');", "DAnswer(CT('$dialogKeyPrefix.TestClose'));", "exit;")),
            @('Mask', @("xdr_test_force_mask_failure = 1 - xdr_test_force_mask_failure;", "xdr_test_last_event = 'симуляция отказа маскировки=' + xdr_test_force_mask_failure;", "DText('Симуляция отказа маскировки: ' + xdr_test_force_mask_failure);", "DAnswer(CT('$dialogKeyPrefix.TestClose'));", "exit;")),
            @('Release', @("xdr_test_release_all = 1;", "xdr_test_last_event = 'запрошено безопасное освобождение';", "DText('Активные специалисты сначала вернутся; безопасные и неактивные будут освобождены сразу.');", "DAnswer(CT('$dialogKeyPrefix.TestClose'));", "exit;"))
        )

        $dialogX = 1120
        $dialogNumber = $dialogMessageCursor
        foreach($entry in $actions) {
            $suffix = $entry[0]
            $actionCode = [string[]]$entry[1]
            $dialogId = $nextId; $nextId++
            $messageId = $nextId; $nextId++
            $rootId = $nextId; $nextId++
            $actionId = $nextId; $nextId++
            $dialogs.Add((New-Dialog "XDRTest${suffix}Dialog" $dialogId $dialogX 440))
            $dialogMessages.Add((New-DialogMessage $messageId $dialogX 490 $dialogNumber))
            $operations.Add((New-Operation "XDRTest${suffix}Root" 'Turn' @("DChange($dialogNumber);",'exit;') $rootId $dialogX 540))
            $operations.Add((New-Operation "XDRTest${suffix}Action" 'Turn' $actionCode $actionId $dialogX 590))
            $links.Add((New-Link $dialogId $rootId))
            $links.Add((New-Link $messageId $actionId))
            $dialogX += 180
            $dialogNumber++
        }
        $dialogMessageCursor = $dialogNumber
    }

    # Every active, non-camouflaged specialist uses this conversation in both
    # the production and test builds. Object IDs intentionally describe only
    # the current new-game schema; no compatibility placeholders are retained.
    $missionDialogId = $nextId; $nextId++
    $missionMessageId = $nextId; $nextId++
    $missionRootId = $nextId; $nextId++
    $missionActionId = $nextId; $nextId++
    $missionMessageNumber = $dialogMessageCursor; $dialogMessageCursor++
    $dialogs.Add((New-Dialog 'XDRMissionTalkDialog' $missionDialogId 1040 640))
    $dialogMessages.Add((New-DialogMessage $missionMessageId 1040 690 $missionMessageNumber))
    $operations.Add((New-Operation 'XDRMissionTalkRoot' 'Turn' @("DChange($missionMessageNumber);",'exit;') $missionRootId 1040 740))
    $operations.Add((New-Operation 'XDRMissionTalkAction' 'Turn' @(
        "DText(CT('$dialogKeyPrefix.MissionText'));",
        "DAnswer(CT('$dialogKeyPrefix.MissionClose'));",
        'exit;'
    ) $missionActionId 1040 790))
    $links.Add((New-Link $missionDialogId $missionRootId))
    $links.Add((New-Link $missionMessageId $missionActionId))

    # Warning shown when the player contacts a camouflaged specialist after
    # targeting it. The day controller only records marker 2; it never opens a
    # modal dialog from TGalaxy.NextDay.
    $attackedDialogId = $nextId; $nextId++
    $attackedMessageId = $nextId; $nextId++
    $attackedRootId = $nextId; $nextId++
    $attackedMessageActionId = $nextId; $nextId++
    $attackedSorryDialogId = $nextId; $nextId++
    $attackedHostileDialogId = $nextId; $nextId++
    $attackedSorryActionId = $nextId; $nextId++
    $attackedHostileActionId = $nextId; $nextId++
    $attackedMessageNumber = $dialogMessageCursor; $dialogMessageCursor++
    $dialogs.Add((New-Dialog 'XDRAttackedTalkDialog' $attackedDialogId 1240 640))
    $dialogMessages.Add((New-DialogMessage $attackedMessageId 1240 690 $attackedMessageNumber))
    $operations.Add((New-Operation 'XDRAttackedTalkRoot' 'Turn' @("DChange($attackedMessageNumber);",'exit;') $attackedRootId 1240 740))
    $operations.Add((New-Operation 'XDRAttackedTalkMessage' 'Turn' @(
        "DText(CT('$dialogKeyPrefix.AttackedText'));",
        "InjectAnswer('XDRAttackedSorryDialog', CT('$dialogKeyPrefix.AttackedSorry'), 0);",
        "InjectAnswer('XDRAttackedHostileDialog', CT('$dialogKeyPrefix.AttackedHostile'), 1);",
        'exit;'
    ) $attackedMessageActionId 1240 790))
    $dialogs.Add((New-Dialog 'XDRAttackedSorryDialog' $attackedSorryDialogId 1240 840))
    $dialogs.Add((New-Dialog 'XDRAttackedHostileDialog' $attackedHostileDialogId 1420 840))
    $operations.Add((New-Operation 'XDRAttackedTalkSorry' 'Turn' @(
        'dword xdr_attacked_ship = GetTalkShip();',
        'dword xdr_attacked_player = Player();',
        'if(!xdr_attacked_ship || !xdr_attacked_player) exit;',
        'if(ShipInHyperSpace(xdr_attacked_ship, 1)) exit;',
        'if(ShipIsTakeoff(xdr_attacked_ship)) exit;',
        'if(!ShipInNormalSpace(xdr_attacked_ship)) exit;',
        'if(ShipInHyperSpace(xdr_attacked_player, 1)) exit;',
        'if(ShipIsTakeoff(xdr_attacked_player)) exit;',
        'if(!ShipInNormalSpace(xdr_attacked_player)) exit;',
        'int xdr_attacked_slot = 0;',
        'if(ShipInCurScript(xdr_attacked_ship) && ArrayDim(xdr_ids) == 4 && ArrayDim(xdr_dialog_markers) == 4)',
        '{',
        '    int xdr_attacked_id = Id(xdr_attacked_ship);',
        '    if(xdr_ids[1] == xdr_attacked_id) xdr_attacked_slot = 1;',
        '    else if(xdr_ids[2] == xdr_attacked_id) xdr_attacked_slot = 2;',
        '    else if(xdr_ids[3] == xdr_attacked_id) xdr_attacked_slot = 3;',
        '    if(xdr_attacked_slot > 0) xdr_dialog_markers[xdr_attacked_slot] = 3;',
        '}',
        'if(ShipGetBad(xdr_attacked_player) == xdr_attacked_ship) ShipSetBad(xdr_attacked_player, 0);',
        'if(ShipOrderObj(xdr_attacked_player) == xdr_attacked_ship) OrderNone(xdr_attacked_player);',
        'if(ShipGetBad(xdr_attacked_ship) == xdr_attacked_player) ShipSetBad(xdr_attacked_ship, 0);',
        'if(ShipOrderObj(xdr_attacked_ship) == xdr_attacked_player) OrderNone(xdr_attacked_ship);',
        'for(int xdr_attacked_player_weapon_index = 1; xdr_attacked_player_weapon_index <= 5; xdr_attacked_player_weapon_index = xdr_attacked_player_weapon_index + 1)',
        '{',
        '    dword xdr_attacked_player_weapon = ShipWeapon(xdr_attacked_player, xdr_attacked_player_weapon_index);',
        '    if(!xdr_attacked_player_weapon) continue;',
        '    if(WeaponTarget(xdr_attacked_player_weapon) == xdr_attacked_ship) WeaponTarget(xdr_attacked_player_weapon, 0);',
        '}',
        'for(int xdr_attacked_ship_weapon_index = 1; xdr_attacked_ship_weapon_index <= 5; xdr_attacked_ship_weapon_index = xdr_attacked_ship_weapon_index + 1)',
        '{',
        '    dword xdr_attacked_ship_weapon = ShipWeapon(xdr_attacked_ship, xdr_attacked_ship_weapon_index);',
        '    if(!xdr_attacked_ship_weapon) continue;',
        '    if(WeaponTarget(xdr_attacked_ship_weapon) == xdr_attacked_player) WeaponTarget(xdr_attacked_ship_weapon, 0);',
        '}',
        'dword xdr_attacked_star = ShipStar(xdr_attacked_ship);',
        'if(xdr_attacked_star)',
        '{',
        '    int xdr_attacked_missile_count = StarMissiles(xdr_attacked_star);',
        '    for(int xdr_attacked_missile_index = xdr_attacked_missile_count - 1; xdr_attacked_missile_index >= 0; xdr_attacked_missile_index = xdr_attacked_missile_index - 1)',
        '    {',
        '        dword xdr_attacked_missile = StarMissiles(xdr_attacked_star, xdr_attacked_missile_index);',
        '        if(!xdr_attacked_missile) continue;',
        '        dword xdr_attacked_missile_owner = MissileOwner(xdr_attacked_missile);',
        '        dword xdr_attacked_missile_target = MissileTarget(xdr_attacked_missile);',
        '        if(xdr_attacked_missile_owner == xdr_attacked_player && xdr_attacked_missile_target == xdr_attacked_ship) MissileLive(xdr_attacked_missile, 10000);',
        '        if(xdr_attacked_missile_owner == xdr_attacked_ship && xdr_attacked_missile_target == xdr_attacked_player) MissileLive(xdr_attacked_missile, 10000);',
        '    }',
        '}',
        '// Do not call TruceBetweenShips here: it synchronously reruns both AIs',
        '// and can recreate the target while a hit-event dialog is unwinding.',
        '// Clear the transient combat channels once more without changing the',
        '// permanent vanilla relationship between player and ranger.',
        'if(ShipGetBad(xdr_attacked_player) == xdr_attacked_ship) ShipSetBad(xdr_attacked_player, 0);',
        'if(ShipOrderObj(xdr_attacked_player) == xdr_attacked_ship) OrderNone(xdr_attacked_player);',
        'if(ShipGetBad(xdr_attacked_ship) == xdr_attacked_player) ShipSetBad(xdr_attacked_ship, 0);',
        'if(ShipOrderObj(xdr_attacked_ship) == xdr_attacked_player) OrderNone(xdr_attacked_ship);',
        'for(int xdr_attacked_after_weapon_index = 1; xdr_attacked_after_weapon_index <= 5; xdr_attacked_after_weapon_index = xdr_attacked_after_weapon_index + 1)',
        '{',
        '    dword xdr_attacked_after_player_weapon = ShipWeapon(xdr_attacked_player, xdr_attacked_after_weapon_index);',
        '    if(xdr_attacked_after_player_weapon && WeaponTarget(xdr_attacked_after_player_weapon) == xdr_attacked_ship) WeaponTarget(xdr_attacked_after_player_weapon, 0);',
        '    dword xdr_attacked_after_ship_weapon = ShipWeapon(xdr_attacked_ship, xdr_attacked_after_weapon_index);',
        '    if(xdr_attacked_after_ship_weapon && WeaponTarget(xdr_attacked_after_ship_weapon) == xdr_attacked_player) WeaponTarget(xdr_attacked_after_ship_weapon, 0);',
        '}',
        'if(xdr_attacked_star)',
        '{',
        '    for(int xdr_attacked_after_missile_index = StarMissiles(xdr_attacked_star) - 1; xdr_attacked_after_missile_index >= 0; xdr_attacked_after_missile_index = xdr_attacked_after_missile_index - 1)',
        '    {',
        '        dword xdr_attacked_after_missile = StarMissiles(xdr_attacked_star, xdr_attacked_after_missile_index);',
        '        if(!xdr_attacked_after_missile) continue;',
        '        dword xdr_attacked_after_owner = MissileOwner(xdr_attacked_after_missile);',
        '        dword xdr_attacked_after_target = MissileTarget(xdr_attacked_after_missile);',
        '        if((xdr_attacked_after_owner == xdr_attacked_player && xdr_attacked_after_target == xdr_attacked_ship) || (xdr_attacked_after_owner == xdr_attacked_ship && xdr_attacked_after_target == xdr_attacked_player))',
        '        {',
        '            MissileLive(xdr_attacked_after_missile, 10000);',
        '        }',
        '    }',
        '}',
        "DText(CT('$dialogKeyPrefix.SorryText'));",
        "DAnswer(CT('$dialogKeyPrefix.DialogClose'));",
        'exit;'
    ) $attackedSorryActionId 1240 890))
    $operations.Add((New-Operation 'XDRAttackedTalkHostile' 'Turn' @(
        'dword xdr_hostile_ship = GetTalkShip();',
        'dword xdr_hostile_player = Player();',
        'if(xdr_hostile_ship && xdr_hostile_player)',
        '{',
        '    if(ShipInCurScript(xdr_hostile_ship) && ArrayDim(xdr_ids) == 4 && ArrayDim(xdr_dialog_markers) == 4)',
        '    {',
        '        int xdr_hostile_id = Id(xdr_hostile_ship);',
        '        if(xdr_ids[1] == xdr_hostile_id) xdr_dialog_markers[1] = 1;',
        '        else if(xdr_ids[2] == xdr_hostile_id) xdr_dialog_markers[2] = 1;',
        '        else if(xdr_ids[3] == xdr_hostile_id) xdr_dialog_markers[3] = 1;',
        '    }',
        '    if(ShipGetBad(xdr_hostile_ship) != 0) ShipSetBad(xdr_hostile_ship, 0);',
        '    if(ShipOrderObj(xdr_hostile_ship) == xdr_hostile_player) OrderNone(xdr_hostile_ship);',
        '}',
        "DText(CT('$dialogKeyPrefix.HostileText'));",
        "DAnswer(CT('$dialogKeyPrefix.DialogClose'));",
        'exit;'
    ) $attackedHostileActionId 1420 890))
    $links.Add((New-Link $attackedDialogId $attackedRootId))
    $links.Add((New-Link 2 $attackedDialogId))
    $links.Add((New-Link $attackedMessageId $attackedMessageActionId))
    $links.Add((New-Link $attackedSorryDialogId $attackedSorryActionId))
    $links.Add((New-Link $attackedHostileDialogId $attackedHostileActionId))

    $visual = [ordered]@{
        Groups = @($groups)
        Operations = @($operations)
        Planets = @($planets)
        Ships = @($ships)
        Stars = @($stars)
        States = @($states)
        Statements = @($statements)
        Variables = @($variables)
    }
    if($dialogs.Count -gt 0) { $visual.Dialogs = @($dialogs); $visual.DialogMessages = @($dialogMessages) }

    $rson = [ordered]@{
        FileID = 573785173
        FileVersion = 8
        'ViewPos.x' = -70
        'ViewPos.y' = -41
        ScriptName = $ScriptName
        ScriptFileOut = "$ScriptName.scr"
        ScriptTextOut = "$ScriptName.txt"
        LangDatIn = ''
        LangDatOut = ''
        MainDatIn = ''
        MainDatOut = ''
        CacheDatIn = ''
        CacheDatOut = ''
        ExportLangTxt = $false
        ExportLangDat = $false
        'Visual.Objects' = @($visual)
        'Visual.Links' = @($links)
        'BlockPar.EC.Total.Strings' = 0
        'BlockPar.EC' = @()
    }

    $sourceDir = Join-Path $Root 'SOURCE'
    [IO.Directory]::CreateDirectory($sourceDir) | Out-Null
    $output = Join-Path $sourceDir "$ScriptName.rson"
    $json = $rson | ConvertTo-Json -Depth 100
    [IO.File]::WriteAllText($output, $json + [Environment]::NewLine, [Text.UTF8Encoding]::new($false))
    return $output
}

$mainOutput = New-XdrRson $MainRoot 'Mod_XenoDomRangers' $false
$testOutput = New-XdrRson $TestRoot 'Mod_XenoDomRangersTest' $true
Write-Output $mainOutput
Write-Output $testOutput
