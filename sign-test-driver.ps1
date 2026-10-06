$ErrorActionPreference = 'Stop'

& (Join-Path $PSScriptRoot 'build-driver.ps1')

$output = Join-Path $PSScriptRoot 'driver\out'
$image = Join-Path $output 'aim_mxl.sys'
$catalog = Join-Path $output 'aim_mxl.cat'
$publicCertificate = Join-Path $output 'aim_mxl_test.cer'
$version = '10.0.26100.6584'
$catalogTool = Join-Path $env:TEMP "aim-driver-tools\packages\Microsoft.Windows.WDK.ARM64.$version\c\bin\10.0.26100.0\x86\Inf2Cat.exe"
$signTool = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin\10.0.22621.0\arm64\signtool.exe'
if (!(Test-Path $catalogTool) -or !(Test-Path $signTool)) {
    throw 'WDK Inf2Cat and Windows SDK ARM64 SignTool are required'
}

$subject = 'CN=AIM MXL ARM64 Test'
$certificate = Get-ChildItem Cert:\CurrentUser\My | Where-Object {
    $_.Subject -eq $subject -and $_.HasPrivateKey -and $_.NotAfter -gt (Get-Date).AddDays(7)
} | Sort-Object NotAfter -Descending | Select-Object -First 1
if (!$certificate) {
    $certificate = New-SelfSignedCertificate -Type CodeSigningCert -Subject $subject `
        -CertStoreLocation Cert:\CurrentUser\My -HashAlgorithm SHA256 `
        -KeyExportPolicy NonExportable
}

Export-Certificate -Cert $certificate -FilePath $publicCertificate -Force | Out-Null
& $signTool sign /fd SHA256 /s My /sha1 $certificate.Thumbprint $image
if ($LASTEXITCODE -ne 0) { throw 'Test-signing the ARM64 image failed' }

& $catalogTool "/driver:$output" /os:10_GE_ARM64
if ($LASTEXITCODE -ne 0) { throw 'Catalog regeneration after image signing failed' }
& $signTool sign /fd SHA256 /s My /sha1 $certificate.Thumbprint $catalog
if ($LASTEXITCODE -ne 0) { throw 'Test-signing the ARM64 catalog failed' }

foreach ($path in @($image, $catalog)) {
    $signature = Get-AuthenticodeSignature -FilePath $path
    if (!$signature.SignerCertificate -or
        $signature.SignerCertificate.Thumbprint -ne $certificate.Thumbprint) {
        throw "Test signature is missing or has the wrong certificate: $path"
    }
}

"Test-signed package: $output"
"Public test certificate: $publicCertificate"
"Certificate thumbprint: $($certificate.Thumbprint)"
"The package remains untrusted until an administrator configures test signing and certificate trust."