# How to use

This module, once loaded, creates a number of sysfs files that can be used to access the voltage offsets of Intel CPUs even on systems with Secure Boot enabled. The following offsets are available under `/sys/module/kundervolt/offsets`:

* `/sys/module/kundervolt/offsets/cpu`: The core's voltage offset.
* `/sys/module/kundervolt/offsets/gpu`: The voltage offset of the Intel integrated GPU (if available).
* `/sys/module/kundervolt/offsets/cache`: The cache's voltage offset.
* `/sys/module/kundervolt/offsets/system_agent`: Sometimes also called "uncore". It includes components that are not directly inside the cores themselves.
* `/sys/module/kundervolt/offsets/analog_io`: The voltage offset of the analog IO ports.

The files contain information about the voltage offset of the specified component in the form of a single floating point number representing millivolts. This is normally `0.00` (indicating that the component is at its normal operating voltage). You can change the value to a negative one to undervolt the component, for example writing `-50` to `/sys/module/kundervolt/offsets/gpu` will apply a `-50` millivolts undervolt to the Intel integrated GPU. A valid offset is in the range of -999 mV to 999 mV.

## Important Tips

* The accepted values must include only numeric characters, possibly decimal, and starting with a negative sign for negative offsets. Unexpected characters (such as whitespace) will cause an invalid input error. I.e. if you are writing to the file with `echo` make sure you use the `-n` option to avoid the newline at the end. E.g. `echo -n '-50' | sudo tee /sys/module/kundervolt/offsets/cpu /sys/module/kundervolt/offsets/cache`
* The cpu and cache voltage offsets are separate interfaces, however they cannot have different voltages. The CPU will use the offset with the highest value between the two and appy it to both. As such it is recommended to always give them the same offset value, otherwise one of them will be effectively useless.
* Overvolting has been explicitly restricted as I am not aware of any real use for it and doing it by mistake could damage hardware. However, the module does technically support overvolting if the restrictions are removed.

# Credits

Thanks to [https://github.com/mihic/linux-intel-undervolt](https://github.com/mihic/linux-intel-undervolt) for the work of listing the publicly known information about Intel's undocumented voltage control.

# License

kundervolt: a kernel module to undervolt Intel-based Linux system with Secure Boot enabled.
Copyright © 2025  Alessandro Balducci

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; version 2
of the License.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, see [https://www.gnu.org/licenses/](https://www.gnu.org/licenses/).

A copy of the license can be found in [COPYING](COPYING).

## FloatToAscii

The library "FloatToAscii" (ftoa) is included in its dedicated directory ftoa/ and is published with its own license, described in [ftoa/README.md](ftoa/README.md).
Minimal modification has been applied to the library to allow for successful compilation inside the kernel module.
