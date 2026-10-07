# UHCI for OPENSTEP

UHCI is an OPENSTEP driver for PCI UHCI USB 1.1 controllers.
It supports read-only USB mass-storage disks and data CD-ROMs using SCSI
Bulk-Only Transport, exposed as up to four removable SCSI targets per controller.
Full-speed devices connected directly or through one tier of full-speed hubs
are supported. HID and networking are not supported.

## Requirements

- OPENSTEP 4.2 for Intel processors
- PCI UHCI USB 1.1 controller
- An assigned I/O port range below 64 KiB and DMA memory addressable below 4 GB
- A routed PCI interrupt for the default INTx mode
- USB storage supporting full-speed (12 Mbit/s) operation, SCSI transparent
  subclass 06 and Bulk-Only protocol 50

USB 2.0 and newer storage must support falling back to full speed.
UHCI and EHCI are separate drivers for separate PCI controller functions;
they can be loaded together. This driver does not require EHCI.

The driver uses shared INTx by default. Keep `Share IRQ Levels` set to `Yes`.
Polling is an explicit compatibility mode that keeps interrupt sources disabled
and claims no DriverKit IRQ. INTx failures do not automatically select Polling.
Neither mode requires PCIMSI; MSI and MSI-X are not supported.

| `Interrupt Mode` | Requirement |
| --- | --- |
| `INTx` (default) | Valid PCI interrupt routing; `Share IRQ Levels = Yes` |
| `Polling` | None beyond the controller requirements; nominal 5 ms polling |

## Installation

If using the Installer package, unpack the `.pkg.tar.gz` archive and open
`UHCI.pkg`. It installs `UHCI.config` into `/private/Devices`.

Open `UHCI.config`.
Configure.app should open and confirm the driver was installed.
Click Add, select `PCI UHCI USB 1.1 Storage Controller`, and add the driver.
If the driver is not shown, check `Show All Installed Drivers` and select it.

Each UHCI controller needs its own `InstanceN.table` in `UHCI.config`.
A single `Instance0.table` attaches only one controller, even if `Auto Detect IDs`
matches several. Add one instance per controller in Configure.app, or create the
tables manually from `Default.table`, retaining the other driver settings.

For portable autodetection of two controllers, use:

| Table | `Instance` | `Location` |
| --- | --- | --- |
| `Instance0.table` | `"0"` | `""` |
| `Instance1.table` | `"1"` | `""` |

Keep the same full `Auto Detect IDs` list from `Default.table` in both tables.
With an empty `Location`, PCIBus scans in bus/device/function order and selects
the zero-based `Instance` match: 0 selects the first matching controller and 1
selects the second. This works whether their PCI IDs are identical or different.
Do not restrict the second table to one controller's ID while keeping
`Instance = 1`: that would request the second match of the restricted list.
If only one controller matches, the second instance finds no device; it does not
attach the first controller again. Additional controllers need additional
instance tables with successive `Instance` values and the same detection list.

Include the instance tables on boot media and the installed system.
On boot media the bundle is under `/private/Drivers/i386/UHCI.config`;
on the installed system it is under `/private/Devices/UHCI.config`.

If the controller is not automatically detected, click Expert and set `Location`
to the controller's PCI coordinates using this exact syntax:

```text
Dev:<device> Func:<function> Bus:<bus>
```

For example, PCI bus 0, device 7, function 2 is `Dev:7 Func:2 Bus:0`.
Use the bus, device, and function reported for your UHCI controller. If its PCI
ID is absent from `Auto Detect IDs`, add it in device/vendor order: Intel
`8086:7112`, for example, is `0x71128086`. The driver also verifies the UHCI PCI
class code `0c0300`.

Keep `Interrupt Mode` set to `INTx`, or explicitly set it to `Polling` if needed.
The driver obtains its IRQ from PCI configuration; do not guess an IRQ number.
Click Done, click Save, and Quit.

Verify that `/private/Devices/UHCI.config/Instance0.table` contains the expected
`Instance`, `Location`, `Auto Detect IDs`, `Interrupt Mode` and `Share IRQ Levels`
settings. For additional controllers, check their corresponding tables.
In `/private/Devices/System.config/Instance0.table`, list `UHCI` only once in
`Boot Drivers`; its instance tables select the controllers. Load it after `PCIBus`
and the existing root-storage controller to preserve root disk numbering.
For example, `PCIBus Intel824X0 BusMasterIDE UHCI` retains IDE first.

Restart OPENSTEP to load the driver. To update an existing installation,
back up `UHCI.config`, replace its driver and resources with the new bundle,
and preserve your `InstanceN.table` settings before restarting. The driver
does not support live unloading or replacement.

Storage is read-only and limited to LUN 0. Attachment and removal are supported;
eject mounted storage before unplugging it. OPENSTEP still needs support for
the filesystem on the device. Nested hubs, UAS, audio CD commands, HID,
networking, isochronous transfers and suspend/resume are not supported.

## Building

With OPENSTEP 4.2 DriverKit installed, run `make clean` followed by `make`
in this directory to produce `UHCI.config`.
