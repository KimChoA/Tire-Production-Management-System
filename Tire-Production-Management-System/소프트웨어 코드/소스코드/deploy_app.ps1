$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Write-Step {
    param([string]$Message)
    Write-Host "`n== $Message ==" -ForegroundColor Cyan
}

function Assert-LastExitCode {
    param([string]$StepName)
    if ($LASTEXITCODE -ne 0) {
        throw "$StepName 실패 (종료 코드: $LASTEXITCODE)"
    }
}

function Copy-WithBackup {
    param(
        [string]$Source,
        [string]$Destination,
        [string]$Backup
    )

    if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) {
        throw "배포할 파일이 없습니다: $Source"
    }

    $destinationDirectory = Split-Path -Parent $Destination
    $backupDirectory = Split-Path -Parent $Backup
    New-Item -ItemType Directory -Force -Path $destinationDirectory | Out-Null
    New-Item -ItemType Directory -Force -Path $backupDirectory | Out-Null

    if (Test-Path -LiteralPath $Destination -PathType Leaf) {
        Copy-Item -LiteralPath $Destination -Destination $Backup -Force
    }

    Copy-Item -LiteralPath $Source -Destination $Destination -Force
}

try {
    $sourceRoot = $PSScriptRoot
    $packageRoot = Split-Path -Parent $sourceRoot
    $deployRoot = Join-Path $packageRoot '실행 앱'
    $serverProject = Join-Path $sourceRoot 'TireProductionServer\TireProductionServer.csproj'
    $winFormsProject = Join-Path $sourceRoot 'WinForms\TireEquipmentHMI.csproj'
    $mfcProject = Join-Path $sourceRoot 'MFC\TireProductionMFC.vcxproj'
    $stagingRoot = Join-Path $sourceRoot '.deploy-staging'
    $serverStaging = Join-Path $stagingRoot 'server'
    $winFormsStaging = Join-Path $stagingRoot 'winforms'

    if (-not (Test-Path -LiteralPath $deployRoot -PathType Container)) {
        throw "실행앱 폴더를 찾지 못했습니다: $deployRoot"
    }

    $running = Get-Process -Name 'TireProductionServer', 'TireEquipmentHMI', 'TireProductionMFC' -ErrorAction SilentlyContinue
    if ($running) {
        $names = ($running | Select-Object -ExpandProperty ProcessName -Unique) -join ', '
        throw "프로그램이 실행 중입니다. 먼저 종료해주세요: $names"
    }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
        throw 'Visual Studio 빌드 도구(vswhere.exe)를 찾지 못했습니다.'
    }

    $msbuild = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
    if (-not $msbuild -or -not (Test-Path -LiteralPath $msbuild -PathType Leaf)) {
        throw 'MSBuild.exe를 찾지 못했습니다.'
    }

    if (Test-Path -LiteralPath $stagingRoot) {
        Remove-Item -LiteralPath $stagingRoot -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path $serverStaging, $winFormsStaging | Out-Null

    Write-Step '중앙서버 Release 단일 파일 게시'
    & dotnet publish $serverProject -c Release -r win-x64 --self-contained true -o $serverStaging '-p:PublishSingleFile=true' '-p:IncludeNativeLibrariesForSelfExtract=true' '-p:DebugType=None' '-p:DebugSymbols=false'
    Assert-LastExitCode '중앙서버 게시'

    Write-Step '로그인/작업자 프로그램 Release 단일 파일 게시'
    & dotnet publish $winFormsProject -c Release -r win-x64 --self-contained true -o $winFormsStaging '-p:PublishSingleFile=true' '-p:IncludeNativeLibrariesForSelfExtract=true' '-p:DebugType=None' '-p:DebugSymbols=false'
    Assert-LastExitCode '로그인/작업자 프로그램 게시'

    Write-Step '관리자 MFC 프로그램 Release x64 빌드'
    & $msbuild $mfcProject /m /t:Build /p:Configuration=Release /p:Platform=x64 /nologo
    Assert-LastExitCode '관리자 MFC 빌드'

    $serverExe = Join-Path $serverStaging 'TireProductionServer.exe'
    $winFormsExe = Join-Path $winFormsStaging 'TireEquipmentHMI.exe'
    $mfcExe = Join-Path $sourceRoot 'MFC\x64\Release\TireProductionMFC.exe'

    # 세 빌드 결과가 모두 준비된 뒤에만 실행앱 파일을 교체합니다.
    foreach ($file in @($serverExe, $winFormsExe, $mfcExe)) {
        if (-not (Test-Path -LiteralPath $file -PathType Leaf)) {
            throw "빌드 결과가 없습니다: $file"
        }
    }

    Write-Step '기존 실행 파일 백업 및 새 실행 파일 배포'
    $backupRoot = Join-Path $packageRoot '이전 버전'
    Copy-WithBackup -Source $serverExe `
        -Destination (Join-Path $deployRoot '01_중앙서버\TireProductionServer.exe') `
        -Backup (Join-Path $backupRoot '01_중앙서버\TireProductionServer.exe')
    Copy-WithBackup -Source $winFormsExe `
        -Destination (Join-Path $deployRoot '02_클라이언트\TireEquipmentHMI.exe') `
        -Backup (Join-Path $backupRoot '02_클라이언트\TireEquipmentHMI.exe')
    Copy-WithBackup -Source $mfcExe `
        -Destination (Join-Path $deployRoot '02_클라이언트\Admin\TireProductionMFC.exe') `
        -Backup (Join-Path $backupRoot '02_클라이언트\Admin\TireProductionMFC.exe')

    Write-Step '배포 완료'
    Write-Host 'DB와 다음 설정 파일은 변경하지 않았습니다.' -ForegroundColor Green
    Write-Host '  01_중앙서버\data\production.db'
    Write-Host '  01_중앙서버\server_config.json'
    Write-Host '  02_클라이언트\client_config.json'
    Write-Host '  02_클라이언트\Admin\client_config.ini'
    Write-Host "이전 EXE 백업: $backupRoot"
}
catch {
    Write-Host "`n배포 실패: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}
finally {
    if ($stagingRoot -and (Test-Path -LiteralPath $stagingRoot)) {
        Remove-Item -LiteralPath $stagingRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}

