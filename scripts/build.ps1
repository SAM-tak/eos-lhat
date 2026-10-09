# Build with the exact L^ headers used by the host, then embed its signatures.
param(
    [string]$Lovec = "$PSScriptRoot/../../lhat-love/build/love/Release/lovec.exe",
    [string]$Lhat = "$PSScriptRoot/../../lhat",
    [string]$LhatGenerated = "$PSScriptRoot/../../lhat-love/build/love/lhat/include",
    [string]$Sdk = "$PSScriptRoot/../EOSSDK/SDK",
    [string]$Build = "$PSScriptRoot/../build",
    [switch]$Test
)
$ErrorActionPreference = 'Stop'
cmake -S "$PSScriptRoot/.." -B $Build "-DEOS_SDK_ROOT=$Sdk" "-DLHAT_ROOT=$Lhat" "-DLHAT_GENERATED_INCLUDE=$LhatGenerated"
if ($LASTEXITCODE -ne 0) { throw 'Configure failed' }
cmake --build $Build --config Release
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
python "$PSScriptRoot/embed-signatures.py" --lovec $Lovec --build $Build --sdk $Sdk
if ($LASTEXITCODE -ne 0) { throw 'Signature generation failed' }
if ($Test) {
    ctest --test-dir $Build -C Release --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'Native tests failed' }
}
