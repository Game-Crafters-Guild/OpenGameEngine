# PowerShell build script for Game Engine
param(
    [string]$Preset = "vs2022-x64-local",
    [string]$Configuration = "DebugFast",
    [switch]$SkipCpp,
    [switch]$SkipCSharp,
    [switch]$Help
)

if ($Help) {
    Write-Host "Game Engine Build Script"
    Write-Host "Usage: .\build.ps1 [-Preset <cmake-preset>] [-Configuration DebugFast|Debug|Release] [-SkipCpp] [-SkipCSharp] [-Help]"
    Write-Host ""
    Write-Host "Parameters:"
    Write-Host "  -Preset         CMake configure preset name (default: vs2022-x64-local)"
    Write-Host "  -Configuration  Build configuration (DebugFast, Debug or Release; default: DebugFast)"
    Write-Host "  -SkipCpp        Skip C++ compilation"
    Write-Host "  -SkipCSharp     Skip managed (.NET) compilation"
    Write-Host "  -Help           Show this help message"
    exit 0
}

Write-Host "=== Game Engine Build Script ===" -ForegroundColor Green
Write-Host "Preset: $Preset" -ForegroundColor Yellow
Write-Host "Configuration: $Configuration" -ForegroundColor Yellow

# Check for required tools
$hasErrors = $false

# Check for CMake
$cmakeAvailable = $false
try {
    $cmakeVersion = cmake --version 2>$null
    if ($LASTEXITCODE -eq 0) {
        $cmakeAvailable = $true
        Write-Host "✓ CMake found" -ForegroundColor Green
    }
} catch {
    # CMake not found
}

if (-not $cmakeAvailable) {
    Write-Host "✗ CMake not found in PATH" -ForegroundColor Red
    Write-Host "  Please install CMake and add it to your PATH" -ForegroundColor Yellow
    Write-Host "  Download from: https://cmake.org/download/" -ForegroundColor Yellow
    $hasErrors = $true
}

# Check for .NET
$dotnetAvailable = $false
try {
    $dotnetVersion = dotnet --version 2>$null
    if ($LASTEXITCODE -eq 0) {
        $dotnetAvailable = $true
        Write-Host "✓ .NET SDK found (version: $dotnetVersion)" -ForegroundColor Green
    }
} catch {
    # .NET not found
}

if (-not $dotnetAvailable) {
    Write-Host "✗ .NET SDK not found" -ForegroundColor Red
    Write-Host "  Please install .NET 10 SDK" -ForegroundColor Yellow
    Write-Host "  Download from: https://dotnet.microsoft.com/download" -ForegroundColor Yellow
    $hasErrors = $true
}

# Check for Visual Studio
$vsAvailable = $false
$vsPaths = @(
    "${env:ProgramFiles}\Microsoft Visual Studio\2022\Enterprise\Common7\Tools\VsDevCmd.bat",
    "${env:ProgramFiles}\Microsoft Visual Studio\2022\Professional\Common7\Tools\VsDevCmd.bat",
    "${env:ProgramFiles}\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat",
    "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2019\Enterprise\Common7\Tools\VsDevCmd.bat",
    "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2019\Professional\Common7\Tools\VsDevCmd.bat",
    "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2019\Community\Common7\Tools\VsDevCmd.bat"
)

foreach ($vsPath in $vsPaths) {
    if (Test-Path $vsPath) {
        $vsAvailable = $true
        Write-Host "✓ Visual Studio found at: $vsPath" -ForegroundColor Green
        break
    }
}

if (-not $vsAvailable) {
    Write-Host "✗ Visual Studio not found" -ForegroundColor Red
    Write-Host "  Please install Visual Studio 2019 or 2022 with C++ support" -ForegroundColor Yellow
    $hasErrors = $true
}

if ($hasErrors) {
    Write-Host ""
    Write-Host "Please install the missing dependencies and try again." -ForegroundColor Red
    Write-Host "For now, you can build C# scripts only with: .\build.ps1 -SkipCpp" -ForegroundColor Yellow
    
    if (-not $SkipCpp) {
        exit 1
    }
}

# Build managed (.NET) projects
if (-not $SkipCSharp -and $dotnetAvailable) {
    Write-Host ""
    Write-Host "=== Building Managed (.NET) Projects ===" -ForegroundColor Green
    
    $managedProjects = @(
        "Managed/HotReload/GameEngine.HotReload.csproj",
        "Managed/CoreBridge/GameEngine.CoreBridge.csproj",
        "Managed/Scripting.ABI/GameEngine.Scripting.ABI.csproj",
        "Managed/Input.ABI/GameEngine.Input.ABI.csproj",
        "Managed/Platform.ABI/GameEngine.Platform.ABI.csproj",
        "Managed/ECS.ABI/GameEngine.ECS.ABI.csproj",
        "Managed/Editor.Managed/GameEngine.Editor.Managed.csproj",
        "Managed/CompileServerHost/GameEngine.CompileServerHost.csproj"
    )

    foreach ($proj in $managedProjects) {
        if (-not (Test-Path $proj)) {
            Write-Host "⚠️  Skipping missing project: $proj" -ForegroundColor Yellow
            continue
        }
        Write-Host "Running: dotnet build $proj -c $Configuration" -ForegroundColor Yellow
        dotnet build $proj -c $Configuration
        if ($LASTEXITCODE -ne 0) {
            Write-Host "✗ Managed build failed: $proj" -ForegroundColor Red
            exit 1
        }
    }

    Write-Host "✓ Managed build successful" -ForegroundColor Green
}

# Build C++ code
if (-not $SkipCpp -and $cmakeAvailable -and $vsAvailable) {
    Write-Host ""
    Write-Host "=== Building C++ Code ===" -ForegroundColor Green
    
    $buildDir = Join-Path "build" $Preset

    # Configure with CMake preset (writes to build/<preset>/)
    Write-Host "Configuring with CMake preset '$Preset'..." -ForegroundColor Yellow
    cmake --preset $Preset
    if ($LASTEXITCODE -ne 0) {
        Write-Host "✗ CMake configuration failed" -ForegroundColor Red
        exit 1
    }

    # Build the project
    Write-Host "Building project (build dir: $buildDir)..." -ForegroundColor Yellow
    cmake --build $buildDir --config $Configuration
    if ($LASTEXITCODE -eq 0) {
        Write-Host "✓ C++ build successful" -ForegroundColor Green
    } else {
        Write-Host "✗ C++ build failed" -ForegroundColor Red
        exit 1
    }
}

Write-Host ""
Write-Host "=== Build Complete ===" -ForegroundColor Green

if ($cmakeAvailable -and $vsAvailable -and -not $SkipCpp) {
    $editorExe = Join-Path (Join-Path "build" $Preset) "bin\$Configuration\Apps\Editor\Editor.exe"
    Write-Host "C++ outputs (per-preset build tree): build\$Preset\bin\$Configuration\" -ForegroundColor Yellow
    Write-Host "  - Editor: $editorExe" -ForegroundColor Cyan
    Write-Host "  - Tests:  build\$Preset\bin\$Configuration\Tests\*.exe" -ForegroundColor Cyan
    Write-Host "  - Demos:  build\$Preset\bin\$Configuration\Demos\*.exe" -ForegroundColor Cyan
}

Write-Host ""
Write-Host "To run the applications:" -ForegroundColor Green
if ($cmakeAvailable -and $vsAvailable -and -not $SkipCpp) {
    Write-Host "  Editor:  .\build\$Preset\bin\$Configuration\Apps\Editor\Editor.exe" -ForegroundColor Cyan
} else {
    Write-Host "  Install CMake and Visual Studio to build and run the applications" -ForegroundColor Yellow
}
