# Security Policy

## Supported Versions

| Version | Supported |
|---------|-----------|
| 1.2.x   | Yes       |
| 1.1.x   | Security fixes only |
| < 1.1.0 | No        |

## Reporting a Vulnerability or Hardware Safety Issue

If you discover a memory safety bug (e.g., DMA buffer overrun, out-of-bounds packet parsing), PIO/GPIO electrical safety issue, or other security concern in `USBSnifferPIO_RP2040`:

1. **Do not** open a public issue with exploit details before a fix is available.
2. Report it privately via **GitHub Security Advisories** (`Security → Advisories → Report a vulnerability`) or contact the maintainer via GitHub.
3. Include the library version, RP2040 board/core version, minimal reproduction sketch, and (if applicable) a `.pcapng` or logic-analyzer capture.

## Responsible Use

`USBSnifferPIO_RP2040` is a passive USB 1.1 protocol analysis library intended strictly for **educational use, embedded debugging, and authorized hardware auditing**. Only tap USB buses that you own or have explicit written authorization to test.
