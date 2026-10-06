# Unofficial AIM MXL driver for Windows ARM64

An independently written USB driver for using the original AIM MXL with an unmodified RaceStudio 2 installation on Windows 11 ARM64. It replaces the device's Windows driver; it does not modify RaceStudio 2 or the MXL firmware. This is not an AIM product or a port of AIM's driver.

## Supported device and status

- **Tested:** original AIM MXL, hardware ID `USB\VID_11CC&PID_0101`, with RaceStudio 2.56.84 on Windows 11 ARM64. Check your device's hardware IDs in Device Manager before using this project.
- **Not established:** other AIM models, other USB IDs, and other Windows architectures. Sharing an AIM driver on another platform does not mean this package supports that device.
- **Observed:** live values, configuration and acquisition time, run listing, and download of one saved run. Clearing logger memory and firmware updates have not been tested or implemented here.

**No normal-install package is available yet.** This repository contains source code, not a Microsoft-signed release. A local test build requires Windows test-signing mode, a trusted local test certificate, and Secure Boot off. Those changes reduce system protection. We are investigating production signing so users can install with Secure Boot on. Do not use the current test package on a PC where you need normal security settings.

## Build from source

On Windows 11 ARM64, install Visual Studio 2022 with the ARM64 C++ tools and run the following from the repository root in PowerShell (internet access is needed for the first build):

```powershell
.\build-driver.ps1
```

The script fetches the Microsoft WDK 10.0.26100.6584 and matching SDK NuGet packages if needed. It creates `driver/out/aim_mxl.sys`, `driver/out/aim_mxl.inf`, and `driver/out/aim_mxl.cat`. The catalog is **unsigned**; building alone does not install the driver or make it suitable for a normally secured PC.

## Install on a development test PC only

These steps are for a PC you control that **already has Secure Boot off** and can be returned to its previous driver configuration. Do not turn off Secure Boot just to install this project on your everyday PC. Have your BitLocker/device-encryption recovery key and a way to restore the previous device driver before changing boot settings. Run commands from the repository root.

1. Install the Windows SDK ARM64 SignTool at `C:\Program Files (x86)\Windows Kits\10\bin\10.0.22621.0\arm64\signtool.exe`. In your regular PowerShell session, create a locally test-signed package:

	```powershell
	.\sign-test-driver.ps1
	```

	This script rebuilds the package, signs the SYS and CAT with a non-exportable certificate in your user certificate store, and writes its **public** certificate to `driver/out/aim_mxl_test.cer`. Run it again after any source change or unsigned build. Do not distribute your private key or treat this certificate as production signing.

2. In an elevated PowerShell session, enable test mode and reboot. Test mode persists across reboots until you turn it off:

	```powershell
	bcdedit /set testsigning on
	```

3. After reboot, open an elevated PowerShell in the repository root, trust **your own** exported test certificate, and install the matching package:

	```powershell
	Import-Certificate -FilePath .\driver\out\aim_mxl_test.cer -CertStoreLocation Cert:\LocalMachine\Root
	Import-Certificate -FilePath .\driver\out\aim_mxl_test.cer -CertStoreLocation Cert:\LocalMachine\TrustedPublisher
	pnputil /add-driver .\driver\out\aim_mxl.inf /install
	```

4. With the MXL connected, run `.\probe-mxl.ps1` to check that the interface and USB configuration descriptor are accessible, then confirm the device appears in RaceStudio 2. The INF binds only `USB\VID_11CC&PID_0101` on ARM64.

To roll back, identify this package's published `oemNN.inf` using `pnputil /enum-drivers`, then run `pnputil /delete-driver oemNN.inf /uninstall` from an elevated PowerShell. Do not substitute another package's number. Turn test mode off with `bcdedit /set testsigning off` and reboot. Remove the matching test certificate from the local machine Root and TrustedPublisher stores, then restore your normal Secure Boot setting in firmware if you changed it. Allow for a BitLocker recovery prompt after firmware changes. Removing test mode makes this driver stop loading unless it has a production-trusted signature.

## License

The original code in this repository is licensed under [Apache License 2.0](LICENSE). AIM and RaceStudio are trademarks of their respective owners; their software is not included.