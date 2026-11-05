# Builds and runs Luau's test suites with asserts on, in upstream's configuration and in Brickadia's (LUA_USE_LONGJMP=1,
# raised light userdata tag limit), with default flags and with --fflags=true, and compares the failures against
# tests/brickadia-expected-failures.txt. Exits nonzero on any unexpected failure, unexpected pass, or crash.
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

# HugeFunctionLoadFailure crashes under longjmp in pristine upstream too, and a crash aborts the remaining tests.
$setups = @(
	@{ Name = 'upstream'; Flags = $commonFlags; Exclude = '' },
	@{ Name = 'longjmp'; Flags = "$commonFlags /DLUA_USE_LONGJMP=1 /DLUA_LUTAG_LIMIT=1024"; Exclude = 'HugeFunctionLoadFailure' }
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
		cmake --build $build --config $config --target Luau.UnitTest Luau.Conformance -- /m /v:minimal *> $log
		if ($LASTEXITCODE -ne 0) { return @{ Setup = $setup.Name; Failure = "build failed, see $log" } }

		$runs = @()
		Push-Location $repo
		foreach ($exe in 'Luau.UnitTest', 'Luau.Conformance')
		{
			foreach ($allFlags in $false, $true)
			{
				$arguments = @()
				if ($allFlags) { $arguments += '--fflags=true' }
				if ($setup.Exclude) { $arguments += "--test-case-exclude=$($setup.Exclude)" }

				$log = Join-Path $build "$exe-$(if ($allFlags) { 'allflags' } else { 'defaultflags' }).log"
				& (Join-Path $build "$config\$exe.exe") @arguments *> $log
				$runs += @{ Exe = $exe; AllFlags = $allFlags; Log = $log; ExitCode = $LASTEXITCODE }
			}
		}
		Pop-Location

		return @{ Setup = $setup.Name; Runs = $runs }
	}
}

$results = $jobs | Wait-Job | Receive-Job
$jobs | Remove-Job

$expected = foreach ($line in Get-Content (Join-Path $repo 'tests/brickadia-expected-failures.txt'))
{
	if ($line -match '^\s*(#|$)') { continue }

	$parts = @($line -split '\s*\|\s*', 4 | ForEach-Object { $_.Trim() })
	if ($parts.Count -ne 4 -or $parts[0] -notin 'all', 'allflags', 'longjmp' -or $parts[1] -notin 'unit', 'conformance')
	{
		throw "Malformed expected-failure line: $line"
	}

	[pscustomobject]@{ Modes = $parts[0]; Exe = $parts[1]; Test = $parts[2]; Reason = $parts[3] }
}

# doctest prints "TEST SUITE:" only for suites with a name, so the suite resets at each separator line.
function Read-Run([string]$log)
{
	$suite = ''
	$case = $null
	$summary = $null
	$failed = [System.Collections.Generic.HashSet[string]]::new()

	foreach ($line in Get-Content $log)
	{
		if ($line -match '^={10,}') { $suite = ''; $case = $null }
		elseif ($line -match '^TEST SUITE:\s+(.*)$') { $suite = $matches[1] }
		elseif ($line -match '^TEST CASE:\s+(.*)$') { $case = "$suite :: $($matches[1])" }
		elseif ($line -match '(ERROR|FATAL ERROR):' -and $case) { [void]$failed.Add($case) }
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
		$label = "[$($result.Setup)] $($run.Exe) $(if ($run.AllFlags) { '--fflags=true' } else { 'default flags' })"
		$parsed = Read-Run $run.Log

		# doctest exits 0 or 1; anything else, or a missing summary, means the binary died part way.
		if ($run.ExitCode -notin 0, 1 -or -not $parsed.Summary)
		{
			Write-Host "$label : CRASHED (exit code $($run.ExitCode)), see $($run.Log)" -ForegroundColor Red
			$problems++
			continue
		}

		$exeKey = if ($run.Exe -eq 'Luau.UnitTest') { 'unit' } else { 'conformance' }
		$applies = @($expected | Where-Object {
			$_.Exe -eq $exeKey -and ($_.Modes -eq 'all' -or ($_.Modes -eq 'allflags' -and $run.AllFlags) -or ($_.Modes -eq 'longjmp' -and $result.Setup -eq 'longjmp'))
		})
		$known = [System.Collections.Generic.HashSet[string]]::new([string[]]@($applies | ForEach-Object { $_.Test }))

		$unexpected = @($parsed.Failed | Where-Object { -not $known.Contains($_) })
		$passing = @($applies | Where-Object { -not $parsed.Failed.Contains($_.Test) })

		$status = if ($unexpected.Count -or $passing.Count) { 'FAIL' } else { 'ok' }
		Write-Host "$label : $status ($($parsed.Summary))"

		foreach ($test in $unexpected) { Write-Host "    unexpected failure: $test" -ForegroundColor Red }
		foreach ($entry in $passing) { Write-Host "    expected failure now passes, update the list: $($entry.Test)" -ForegroundColor Yellow }
		$problems += $unexpected.Count + $passing.Count
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
