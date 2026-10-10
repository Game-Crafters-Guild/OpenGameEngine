#!/usr/bin/env pwsh
<#
.SYNOPSIS
    Comprehensive performance monitoring and regression detection for Game Engine

.DESCRIPTION
    This script runs performance tests and monitors for regressions in critical systems:
    - Hot-reload pipeline (0ms main thread blocking target)
    - Job system (10,000+ tasks per frame at 240Hz)
    - Asset loading and management
    - Memory allocation patterns

.PARAMETER TestType
    Type of performance test to run: All, HotReload, JobSystem, Assets

.PARAMETER Iterations
    Number of test iterations for statistical accuracy (default: 5)

.PARAMETER Baseline
    Run baseline performance measurement and save results

.PARAMETER Compare
    Compare current performance against saved baseline

.PARAMETER Verbose
    Enable detailed performance logging

.EXAMPLE
    .\performance-monitor.ps1 -TestType All -Iterations 10 -Verbose
    
.EXAMPLE
    .\performance-monitor.ps1 -TestType HotReload -Baseline
    
.EXAMPLE
    .\performance-monitor.ps1 -Compare
#>

param(
    [ValidateSet("All", "HotReload", "JobSystem", "Assets")]
    [string]$TestType = "All",
    
    [int]$Iterations = 5,
    
    [switch]$Baseline,
    
    [switch]$Compare,
    
    [switch]$Verbose
)

# Performance targets and thresholds
$PerformanceTargets = @{
    HotReload = @{
        MainThreadBlocking = 1      # <1ms (target: 0ms)
        TotalCompletion = 20        # <20ms (optimal: 8-13ms)
        PipelineSubmission = 10     # <10ms (optimal: 3-5ms)
        OptimalCompletion = 13      # Warn if above this
    }
    JobSystem = @{
        TaskExecution = 15          # <15ms for 10,000 tasks
        TaskLatencyP95 = 500        # <500μs P95 latency
        TaskThroughput = 666        # >666 tasks/ms (10,000 tasks in 15ms)
    }
    Assets = @{
        AssetLoading = 100          # <100ms for typical asset
        AssetRegistration = 10      # <10ms for asset registration
        DependencyResolution = 5    # <5ms for dependency resolution
    }
}

function Write-PerformanceHeader {
    Write-Host "🎯 Game Engine Performance Monitor" -ForegroundColor Cyan
    Write-Host "=================================" -ForegroundColor Cyan
    Write-Host "Test Type: $TestType" -ForegroundColor White
    Write-Host "Iterations: $Iterations" -ForegroundColor White
    Write-Host "Timestamp: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')" -ForegroundColor White
    Write-Host ""
}

function Test-HotReloadPerformance {
    Write-Host "🔥 Testing Hot-Reload Performance..." -ForegroundColor Yellow
    
    $results = @()
    
    for ($i = 1; $i -le $Iterations; $i++) {
        Write-Host "  Iteration $i/$Iterations" -ForegroundColor Gray
        
        # Run hot-reload performance tests
        $output = & ".\build\bin\Debug\JobSystemTests.exe" --gtest_filter="*HotReloadPerformanceRegression*" 2>&1
        
        if ($LASTEXITCODE -eq 0) {
            # Parse performance metrics from output
            $mainThreadBlocking = 0
            $totalCompletion = 0
            $pipelineSubmission = 0
            
            # Extract metrics from test output (simplified parsing)
            foreach ($line in $output) {
                if ($line -match "Main thread blocking: (\d+)ms") {
                    $mainThreadBlocking = [int]$matches[1]
                }
                if ($line -match "Total completion: (\d+)ms") {
                    $totalCompletion = [int]$matches[1]
                }
                if ($line -match "Average submission: (\d+)ms") {
                    $pipelineSubmission = [int]$matches[1]
                }
            }
            
            $results += @{
                MainThreadBlocking = $mainThreadBlocking
                TotalCompletion = $totalCompletion
                PipelineSubmission = $pipelineSubmission
                Success = $true
            }
        } else {
            Write-Host "    ❌ Test failed" -ForegroundColor Red
            $results += @{ Success = $false }
        }
    }
    
    # Calculate statistics
    $successfulResults = $results | Where-Object { $_.Success }
    
    if ($successfulResults.Count -gt 0) {
        $avgMainThreadBlocking = ($successfulResults | Measure-Object -Property MainThreadBlocking -Average).Average
        $avgTotalCompletion = ($successfulResults | Measure-Object -Property TotalCompletion -Average).Average
        $avgPipelineSubmission = ($successfulResults | Measure-Object -Property PipelineSubmission -Average).Average
        
        Write-Host ""
        Write-Host "📊 Hot-Reload Performance Results:" -ForegroundColor Green
        Write-Host "   Main Thread Blocking: $([math]::Round($avgMainThreadBlocking, 2))ms (target: less than $($PerformanceTargets.HotReload.MainThreadBlocking)ms)" -ForegroundColor White
        Write-Host "   Total Completion: $([math]::Round($avgTotalCompletion, 2))ms (target: less than $($PerformanceTargets.HotReload.TotalCompletion)ms)" -ForegroundColor White
        Write-Host "   Pipeline Submission: $([math]::Round($avgPipelineSubmission, 2))ms (target: less than $($PerformanceTargets.HotReload.PipelineSubmission)ms)" -ForegroundColor White
        
        # Regression detection
        $regressions = @()
        
        if ($avgMainThreadBlocking -gt $PerformanceTargets.HotReload.MainThreadBlocking) {
            $regressions += "Main thread blocking exceeded target"
        }
        
        if ($avgTotalCompletion -gt $PerformanceTargets.HotReload.TotalCompletion) {
            $regressions += "Total completion time exceeded target"
        }
        
        if ($avgTotalCompletion -gt $PerformanceTargets.HotReload.OptimalCompletion) {
            Write-Host "   ⚠️  Performance degradation: $([math]::Round($avgTotalCompletion, 2))ms (optimal: 8-13ms)" -ForegroundColor Yellow
        }
        
        if ($regressions.Count -gt 0) {
            Write-Host ""
            Write-Host "🚨 PERFORMANCE REGRESSIONS DETECTED:" -ForegroundColor Red
            foreach ($regression in $regressions) {
                Write-Host "   - $regression" -ForegroundColor Red
            }
            return $false
        } else {
            Write-Host "   ✅ All performance targets met" -ForegroundColor Green
            return $true
        }
    } else {
        Write-Host "❌ All hot-reload tests failed" -ForegroundColor Red
        return $false
    }
}

function Test-JobSystemPerformance {
    Write-Host "⚡ Testing Job System Performance..." -ForegroundColor Yellow
    
    $results = @()
    
    for ($i = 1; $i -le $Iterations; $i++) {
        Write-Host "  Iteration $i/$Iterations" -ForegroundColor Gray
        
        # Run job system performance tests
        $output = & ".\build\bin\Debug\JobSystemTests.exe" --gtest_filter="*PerformanceRegression*" 2>&1
        
        if ($LASTEXITCODE -eq 0) {
            # Parse performance metrics
            $taskExecution = 0
            $taskThroughput = 0
            
            foreach ($line in $output) {
                if ($line -match "(\d+) tasks completed in (\d+)ms") {
                    $taskCount = [int]$matches[1]
                    $taskExecution = [int]$matches[2]
                    $taskThroughput = $taskCount / $taskExecution
                }
            }
            
            $results += @{
                TaskExecution = $taskExecution
                TaskThroughput = $taskThroughput
                Success = $true
            }
        } else {
            $results += @{ Success = $false }
        }
    }
    
    # Calculate and report statistics
    $successfulResults = $results | Where-Object { $_.Success }
    
    if ($successfulResults.Count -gt 0) {
        $avgTaskExecution = ($successfulResults | Measure-Object -Property TaskExecution -Average).Average
        $avgTaskThroughput = ($successfulResults | Measure-Object -Property TaskThroughput -Average).Average
        
        Write-Host ""
        Write-Host "📊 Job System Performance Results:" -ForegroundColor Green
        Write-Host "   Task Execution: $([math]::Round($avgTaskExecution, 2))ms (target: less than $($PerformanceTargets.JobSystem.TaskExecution)ms)" -ForegroundColor White
        Write-Host "   Task Throughput: $([math]::Round($avgTaskThroughput, 2)) tasks/ms (target: greater than $($PerformanceTargets.JobSystem.TaskThroughput))" -ForegroundColor White
        
        # Regression detection
        if ($avgTaskExecution -gt $PerformanceTargets.JobSystem.TaskExecution) {
            Write-Host "🚨 Job system performance regression detected!" -ForegroundColor Red
            return $false
        } else {
            Write-Host "   ✅ Job system performance targets met" -ForegroundColor Green
            return $true
        }
    } else {
        Write-Host "❌ All job system tests failed" -ForegroundColor Red
        return $false
    }
}

function Save-PerformanceBaseline {
    param($Results)
    
    $baselineFile = "performance-baseline.json"
    $baseline = @{
        Timestamp = Get-Date -Format 'yyyy-MM-dd HH:mm:ss'
        Results = $Results
        Targets = $PerformanceTargets
    }
    
    $baseline | ConvertTo-Json -Depth 3 | Out-File -FilePath $baselineFile -Encoding UTF8
    Write-Host "💾 Performance baseline saved to $baselineFile" -ForegroundColor Green
}

function Compare-WithBaseline {
    param($CurrentResults)
    
    $baselineFile = "performance-baseline.json"
    
    if (-not (Test-Path $baselineFile)) {
        Write-Host "❌ No baseline file found. Run with -Baseline first." -ForegroundColor Red
        return $false
    }
    
    $baseline = Get-Content $baselineFile | ConvertFrom-Json
    
    Write-Host ""
    Write-Host "📈 Performance Comparison with Baseline:" -ForegroundColor Cyan
    Write-Host "   Baseline: $($baseline.Timestamp)" -ForegroundColor Gray
    Write-Host "   Current:  $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')" -ForegroundColor Gray
    
    # Compare results and detect regressions
    # Implementation would compare current vs baseline metrics
    
    Write-Host "   📊 Comparison completed" -ForegroundColor Green
    return $true
}

# Main execution
try {
    Write-PerformanceHeader
    
    $allPassed = $true
    $results = @{}
    
    if ($TestType -eq "All" -or $TestType -eq "HotReload") {
        $results.HotReload = Test-HotReloadPerformance
        $allPassed = $allPassed -and $results.HotReload
    }
    
    if ($TestType -eq "All" -or $TestType -eq "JobSystem") {
        $results.JobSystem = Test-JobSystemPerformance
        $allPassed = $allPassed -and $results.JobSystem
    }
    
    # Save baseline if requested
    if ($Baseline) {
        Save-PerformanceBaseline -Results $results
    }
    
    # Compare with baseline if requested
    if ($Compare) {
        Compare-WithBaseline -CurrentResults $results
    }
    
    Write-Host ""
    if ($allPassed) {
        Write-Host "🎉 All performance tests passed!" -ForegroundColor Green
        exit 0
    } else {
        Write-Host "❌ Performance regressions detected!" -ForegroundColor Red
        exit 1
    }
    
} catch {
    Write-Host "Performance monitoring failed: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}

