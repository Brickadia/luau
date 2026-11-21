# Builds and runs Luau's test suites with asserts on, in upstream's configuration and in Brickadia's (LUA_USE_LONGJMP=1,
# raised light userdata tag limit): the unit and conformance tests with default flags and with --fflags=true, and the
# golden suite. Exits nonzero on any failure or crash.
#
# Usage: pwsh tools/brickadia-test.ps1 [-BuildRoot <dir>] [-Config Debug] [-FlagsSince 0.741]

param(
	[string]$BuildRoot = (Join-Path ([IO.Path]::GetTempPath()) 'luau-brickadia-tests'),
	[ValidateSet('RelWithDebInfo', 'Debug', 'Release')][string]$Config = 'RelWithDebInfo',
	[string]$FlagsSince = ''
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent

if ($FlagsSince)
{
	git -C $repo rev-parse --verify --quiet "$FlagsSince^{commit}" | Out-Null
	if ($LASTEXITCODE -ne 0) { throw "-FlagsSince '$FlagsSince' is not a commit" }
}

$commonFlags = "/EHsc /GR /DLUAU_ENABLE_ASSERT=1"

$setups = @(
	@{ Name = 'upstream'; Flags = $commonFlags },
	@{ Name = 'longjmp'; Flags = "$commonFlags /DLUA_USE_LONGJMP=1 /DLUA_LUTAG_LIMIT=1024" }
)

$jobs = foreach ($setup in $setups)
{
	Start-Job -ArgumentList $repo, $BuildRoot, $Config, $setup -ScriptBlock {
		param($repo, $buildRoot, $config, $setup)

		$build = Join-Path $buildRoot $setup.Name
		New-Item -ItemType Directory -Force $build | Out-Null

		$log = Join-Path $build 'configure.log'
		cmake -S $repo -B $build -A x64 "-DCMAKE_CXX_FLAGS=$($setup.Flags)" *> $log
		if ($LASTEXITCODE -ne 0) { return @{ Setup = $setup.Name; Failure = "configure failed, see $log" } }

		$log = Join-Path $build 'build.log'
		cmake --build $build --config $config --target Luau.UnitTest Luau.Conformance Luau.Repl.CLI Luau.Analyze.CLI -- /m /v:minimal *> $log
		if ($LASTEXITCODE -ne 0) { return @{ Setup = $setup.Name; Failure = "build failed, see $log" } }

		$bin = Join-Path $build $config
		$runs = @()
		Push-Location $repo
		foreach ($exe in 'Luau.UnitTest', 'Luau.Conformance')
		{
			foreach ($allFlags in $false, $true)
			{
				$arguments = @()
				if ($allFlags) { $arguments += '--fflags=true' }

				$log = Join-Path $build "$exe-$(if ($allFlags) { 'allflags' } else { 'defaultflags' }).log"
				& (Join-Path $bin "$exe.exe") @arguments *> $log
				$runs += @{ Kind = 'doctest'; Name = "$exe $(if ($allFlags) { '--fflags=true' } else { 'default flags' })"; Log = $log; ExitCode = $LASTEXITCODE }
			}
		}

		$log = Join-Path $build 'golden.log'
		python -m tools.golden --luau (Join-Path $bin 'luau.exe') --luau-analyze (Join-Path $bin 'luau-analyze.exe') *> $log
		$runs += @{ Kind = 'golden'; Name = 'golden suite'; Log = $log; ExitCode = $LASTEXITCODE }
		Pop-Location

		return @{ Setup = $setup.Name; Runs = $runs }
	}
}

$results = $jobs | Wait-Job | Receive-Job
$jobs | Remove-Job

# doctest prints "TEST SUITE:" only for suites with a name, so the suite resets at each separator line.
function Read-DoctestRun([string]$log)
{
	$suite = ''
	$case = $null
	$summary = $null
	$failed = [System.Collections.Generic.List[string]]::new()

	foreach ($line in Get-Content $log)
	{
		if ($line -match '^={10,}') { $suite = ''; $case = $null }
		elseif ($line -match '^TEST SUITE:\s+(.*)$') { $suite = $matches[1] }
		elseif ($line -match '^TEST CASE:\s+(.*)$') { $case = "$suite :: $($matches[1])" }
		elseif ($line -match '(ERROR|FATAL ERROR):' -and $case -and -not $failed.Contains($case)) { $failed.Add($case) }
		elseif ($line -match '\[doctest\] test cases:') { $summary = $line.Trim() }
	}

	return @{ Failed = $failed; Summary = $summary }
}

$problems = 0

foreach ($result in $results)
{
	if ($result.Failure)
	{
		Write-Host "[$($result.Setup)] $($result.Failure)" -ForegroundColor Red
		$problems++
		continue
	}

	foreach ($run in $result.Runs)
	{
		$label = "[$($result.Setup)] $($run.Name)"

		if ($run.Kind -eq 'golden')
		{
			$summary = (Select-String -Path $run.Log -Pattern '^golden: ' | Select-Object -Last 1).Line
			$ok = $run.ExitCode -eq 0 -and $summary
			Write-Host "$label : $(if ($ok) { 'ok' } else { 'FAIL' }) ($(if ($summary) { $summary } else { "exit code $($run.ExitCode)" }))"
			if (-not $ok) { Write-Host "    see $($run.Log)" -ForegroundColor Red; $problems++ }
			continue
		}

		$parsed = Read-DoctestRun $run.Log

		# doctest exits 0 or 1; anything else, or a missing summary, means the binary died part way.
		if ($run.ExitCode -notin 0, 1 -or -not $parsed.Summary)
		{
			Write-Host "$label : CRASHED (exit code $($run.ExitCode)), see $($run.Log)" -ForegroundColor Red
			$problems++
			continue
		}

		$ok = $run.ExitCode -eq 0 -and $parsed.Failed.Count -eq 0
		Write-Host "$label : $(if ($ok) { 'ok' } else { 'FAIL' }) ($($parsed.Summary))"
		foreach ($test in $parsed.Failed) { Write-Host "    failed: $test" -ForegroundColor Red }
		if (-not $ok) { $problems += [Math]::Max(1, $parsed.Failed.Count) }
	}
}

if ($FlagsSince)
{
	$pattern = 'LUAU_(DYNAMIC_)?FASTFLAGVARIABLE\('
	$dirs = 'Analysis', 'Ast', 'Bytecode', 'CodeGen', 'Common', 'Compiler', 'Config', 'Require', 'VM'
	$before = @(git -C $repo grep -h -E $pattern $FlagsSince -- $dirs | ForEach-Object { $_.Trim() } | Sort-Object -Unique)
	$after = @(git -C $repo grep -h -E $pattern HEAD -- $dirs | ForEach-Object { $_.Trim() } | Sort-Object -Unique)

	Write-Host "`nFlags changed since $($FlagsSince). Brickadia enables every non-experimental Luau* flag, so review the added ones:"
	foreach ($change in Compare-Object $before $after)
	{
		Write-Host "    $(if ($change.SideIndicator -eq '=>') { '+' } else { '-' }) $($change.InputObject)"
	}
}

exit ([int]($problems -ne 0))
