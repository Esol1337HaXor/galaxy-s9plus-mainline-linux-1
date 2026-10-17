# debug-tools

Register, PMIC and clock tools from the bring-up of this port, and test-run helpers.

All of them read or write hardware directly through `/dev/mem`; use them only
if you know what the register does.

| Tool | |
|---|---|
| `s9p-acpm` | speaks the ACPM mailbox (the only path to the S2MPS18 PMIC): `read`, `write`, `update`, `tsp on/off`, `dvfs` |
| `s9p-cpuclk` | sets a cluster PLL and its rail by hand |
| `s9p-mem` | reads 32-bit registers |
| `s9p-wlan` | loads the PCIe root complex and `brcmfmac` by hand (for `WLAN=m` kernels) |
| `s9p-cpu-validate` | loads the clusters with `stress-ng --verify` (per cluster, one M3 core for the 2704 MHz point, everything with memory, or random frequency jumps) and logs clocks, temperatures and PLL timeouts to `/var/log/s9p-cpu-validate/`; non-zero exit on any wrong result |
| `s9p-testrun` + `s9p-testrun.service` | writes a status snapshot every 15 s; with `s9p_testrun=N` on the kernel command line it reboots into TWRP after N s so logs can be read without touching the phone |
| `s9p-governor` + `s9p-governor.service` | the userspace DVFS governor from before the in-kernel drivers; do not run it together with them |
| `90-s9p-exception-trace.conf` | logs user-space crashes (pc/lr) in dmesg |

The two services are not enabled by the installer; enable one with
`sudo systemctl enable --now <unit>` if you need it.
