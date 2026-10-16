# Windows software gate. Physical HIL and live provider checks are separate.
$ErrorActionPreference = "Stop"
$PSNativeCommandUseErrorActionPreference = $false

function Invoke-Checked([scriptblock]$Command) {
    & $Command
    if ($LASTEXITCODE -ne 0) { throw "Check failed with exit code $LASTEXITCODE" }
}

$repoRoot = (git rev-parse --show-toplevel).Trim()
Set-Location $repoRoot
$windres = Get-Command windres.exe -ErrorAction SilentlyContinue
if ($windres) { $env:PATH = (Split-Path -Parent $windres.Source) + ';' + $env:PATH }
$testTmp = Join-Path $repoRoot 'target\eval-fast-temp'
New-Item -ItemType Directory -Force -Path $testTmp | Out-Null
$env:TEMP = $testTmp
$env:TMP = $testTmp
$env:RUST_TEST_THREADS = '1'

Write-Host 'Checking Rust formatting, workspace and isolated tests...'
Invoke-Checked { cargo fmt --all -- --check }
Invoke-Checked { cargo check --workspace }
Invoke-Checked { cargo test --workspace }

Write-Host 'Checking frontend...'
Invoke-Checked { pnpm exec prettier --check "app/**/*.{ts,tsx,js,mjs,css,html,json}" "*.{json,yaml}" ".github/**/*.yml" }
Invoke-Checked { pnpm typecheck }
Invoke-Checked { pnpm test }
Invoke-Checked { pnpm build }

Write-Host 'Checking secret paths and contents...'
$files = @(git -c core.quotepath=false ls-files --cached --others --exclude-standard)
if ($LASTEXITCODE -ne 0) { throw 'Cannot list repository files' }
foreach ($file in $files) {
    if ($file -match '(^|/)(\.env($|\.)|secrets?/|evidence/private/)') {
        throw "Sensitive candidate path detected: $file"
    }
}
$pattern = 'sk-[A-Za-z0-9_-]{20,}|Bearer\s+[A-Za-z0-9._-]{20,}|(DASHSCOPE_API_KEY|QWEN_API_KEY)\s*=\s*[^<`\s]+|-----BEGIN (RSA |EC |OPENSSH )?PRIVATE KEY-----'
$scanFiles = @($files | Where-Object { $_ -ne 'scripts/check-secrets.sh' })
& rg -I -q -e $pattern -- $scanFiles
if ($LASTEXITCODE -eq 0) { throw 'Credential-like candidate content detected' }
if ($LASTEXITCODE -ne 1) { throw 'Secret content scan could not finish' }
Write-Host '[PASS] Windows software checks passed; hardware HIL is independent.' -ForegroundColor Green
