# low-power

Idle power: the memory clock follows the load, and the blocks no driver uses are switched off.

```bash
sudo extras/install.sh low-power
cat /sys/module/s9p_mif/parameters/cur_khz /sys/module/s9p_pd_off/parameters/state
```

Two modules (src/power):

* `s9p-mif` scales the memory interface (LPDDR4X) between 421 and 1794 MHz
  through the APM firmware, as the vendor kernel does. sboot leaves it at
  1794 MHz and nothing in the port changed it. Policy every 50 ms: CPU busy
  time, the GPU clock and the USB-C monitor set the level; up at once, down
  after a second. Saves about 150 mW at an idle desktop, measured at the
  battery; memory-bound work runs at the same speed as with the clock fixed.
* `s9p-pd-off` powers off the PMU domains of the camera ISP, IVA, the SCORE
  DSP, G2D, MFC and the depth-camera blocks with the vendor's sequence, as the
  vendor kernel does once nobody uses them. About 90 mW.

Options in `/etc/modprobe.d/s9p-low-power.conf`. Camera bring-up needs ISPPRE
back on: `echo isppre > /sys/module/s9p_pd_off/parameters/power_on`.
Details: [docs/05-performance.md](../../docs/05-performance.md).
