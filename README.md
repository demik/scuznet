scuznet
=======

Open source hardware emulating a Nuvolink SCSI to Ethernet adapter, for use
with vintage Macintosh computers.

# Status

The firmware is at an early alpha stage. There are many things that are known
to not work correctly, and there are likely a large number of other bugs that
have yet to be identified and/or fixed.

If you encounter problems, please open a new issue.

# Compatibility

This implementation has some limitations by design:

1. There must be only one initiator on the bus.
2. The initiator on the bus must be at ID 7.
3. Read parity is not used. Transmission parity is provided but may be disabled
   to improve performance.

These should not be a significant problem for the target computer platform,
which for the most part has similar limitations.

# Device Configuration

The supplied memory card should be FAT32 formatted (exFAT is not currently
supported). On boot the firmware will look for `scuznet.ini` on the card to
configure itself. See the example file in the repository for more information.

When compiling, verify that -DHW_VXXX in the Makefile is replaced with the
appropriate hardware revision.

The virtual hard drive can be formatted within class Mac OS using a variety of
era-appropriate SCSI tools. Patched versions of *HD SC Setup* are known to
work correctly.

# Error Reporting

If a runtime error occurs, the device will show a series of LED flashes.
Consult the following table to determine what the issue is.

| Long # | Short # | Issue                                                   |
| ------ | ------- | ------------------------------------------------------- |
| 1-4    | FatFs   | Error working with hard drive image.                    |
| 5      | FatFs   | Can't open or read the `scuznet.ini` file.              |
| 6      | 0       | General error in the INI parser.                        |
| 6      | 1-255   | Error in the INI file @ line equal to short flashes.    |
| 7      | 0       | Device experienced a power brown-out.                   |
| 8      | 0       | Unable to open the memory card.                         |

When a hard drive image cannot be accessed, the long flashes will indicate what
configuration item is causing the issue, where 1 is for `[hdd1]`, 2 for
`[hdd2]`, etc.

For the `FatFs` items above, the number of short flashes is the underlying
**FatFs** fault code ID. Common issues include:

| ID  | FatFs Code      | Issue                                              |
| --- | --------------- | -------------------------------------------------- |
| 1   | FR_DISK_ERR     | A low-level problem with the memory card.          |
| 5   | FR_NOT_READY    | The memory card was not ready for the operation.   |
| 5   | FR_NO_FILE      | The specified file does not exist.                 |
| 6   | FR_INVALID_NAME | The specified file name was invalid.               |
| 7   | FR_DENIED       | Insufficient space for 'size=X' command.           |

Refer to `lib/ff/ff.h` for a full listing.

# License

Except where otherwise noted, all files in this repository are available under
the terms of the GNU General Public License, version 3, available in the
LICENSE document. There is NO WARRANTY, not even for MERCHANTABILITY or
FITNESS FOR A PARTICULAR PURPOSE. For details, refer to the license.
