# MicroBook (07fd:0004, bcdDevice 0x0032) USB protocol

USB 1.1 full speed, vendor-specific class (255/4/255). "Verified" = observed on hardware.

## Layout (verified)
- Configuration 1, interface 0. macOS leaves the device unconfigured; the driver must set configuration 1.
- Alt setting 0: interrupt EP `0x01` OUT and `0x81` IN (max packet 40).
- Alt setting 1: the same, plus isochronous EP `0x02` OUT (288) and `0x82` IN (576).

## Command channel (verified for read and write quadlet)
Big-endian fields. Byte 0 is a sequence number; one command in flight at a time. Interrupt endpoints take no
completion timeout in DriverKit.

| Direction | Bytes |
|---|---|
| read | `seq 04 00 00 addr32` |
| read reply | `seq 06 status 00 addr32 data32` |
| write | `seq 00 00 00 addr32 data32` |
| write reply | `seq 02 status 00 addr32` |
| device notice | `00 00 00 00 addr32 data32` |

Block read/write opcodes (`05`, `01`) exist in the vendor driver but were not exercised.

About 7 s after attach the device sends a notice for address `0xF0000000` and only then answers commands.

## Registers
| Address | Meaning |
|---|---|
| `0x0000` | identification (read) |
| `0x0004` | serial number (read) |
| `0x0B14` | sample rate: `1` = 44.1 kHz, `0` = 48 kHz (verified) |
| `0x0B18` | clock lock on other models; returns status 7 here |

## Audio (verified)
One isochronous frame per millisecond. All samples are 24-bit **little-endian**, signed, interleaved, no headers.

- OUT: 2 channels (6 bytes per sample frame). 48 kHz: 48 per packet. 44.1 kHz: 45 then nine times 44 (441 per 10 ms).
  Packets must be **packed back to back** in the transfer buffer.
- IN: 4 channels (12 bytes per sample frame). The device decides the packet length (576 B at 48 kHz;
  528/540 B at 44.1 kHz).
