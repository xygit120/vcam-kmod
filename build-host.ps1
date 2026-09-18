# SPDX-License-Identifier: GPL-2.0
# Offline build+run of the module sources against the kernel-API shim.
# Usage: powershell -File build-host.ps1 [-Gcc <path to gcc.exe>]
param(
    [string] $Gcc = 'C:\VCAM_DEV\toolchain\cpp\mingw64\bin\gcc.exe'
)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
New-Item -ItemType Directory -Force -Path (Join-Path $here 'build') | Out-Null
& $Gcc -std=gnu11 -DVCAM_HOST_BUILD=1 -Wall -Wextra -Werror `
    -I (Join-Path $here 'shim') -I (Join-Path $here 'include') `
    (Join-Path $here 'host\vcam_host_test.c') `
    -o (Join-Path $here 'build\vcam_host_test.exe')
if ($LASTEXITCODE -ne 0) { throw "compile failed ($LASTEXITCODE)" }
& (Join-Path $here 'build\vcam_host_test.exe')
exit $LASTEXITCODE
