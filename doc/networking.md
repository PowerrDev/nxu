# Networking and Terminal

NXU's first networking milestone separates Ethernet hardware from protocol processing. VirtIO-net moves frames through one RX/TX queue pair; `kern/net` owns interfaces, bounded packet storage, receive queues, routing, and echo transactions. `kern/netinet` implements IPv4/ICMP and `kern/netinet6` implements IPv6/ICMPv6 and Neighbor Discovery. Protocol work runs in one kernel worker, outside the interrupt handler.

## Supported devices and protocols

| Component | Current support |
| --- | --- |
| VirtIO-net | ARM64 modern MMIO; i386 legacy and modern PCI; software checksums; MTU 1500 |
| Loopback | `lo0`, IPv4 and IPv6; local Ethernet-interface addresses also stay inside the kernel |
| Ethernet / ARP | Ethernet II dispatch; bounded IPv4 neighbor cache; requests and replies |
| IPv4 / ICMP | Header and checksum validation; connected/default routes; echo request/reply; incoming error matching |
| IPv6 / ICMPv6 | Base-header validation; bounded extension-header parsing; echo request/reply; incoming error matching |
| Neighbor Discovery | Neighbor Solicitation/Advertisement; bounded cache; Duplicate Address Detection |
| Diagnostics | Read-only `ifconfig`; numeric-address `ping`; interface RX/TX/drop counters |

There is no TCP, UDP, DNS, DHCP, SLAAC, Router Advertisement configuration, forwarding, fragmentation reassembly, or Path MTU Discovery state yet. Unsupported fragmented traffic is dropped. The interface configuration is a QEMU development configuration, not a general network configurator. External internet echo depends on the host's QEMU backend and network policy; the regression uses QEMU's local gateway.

`en0` uses `10.0.2.15/24` with gateway `10.0.2.2`, and the QEMU IPv6 development prefix with address `fec0::15` and gateway `fec0::2`. It also derives a link-local IPv6 address from the MAC. Secondary adapters receive link-local IPv6 addresses but no default route. IPv6 remains tentative while DAD runs. `make run-console` and the i386 desktop recipe attach a VirtIO NIC by default; use `NET=0` to omit it.

## Terminal and shell

The ARM64 desktop includes `/Applications/Terminal.app`, with `nzh` as its shell. Terminal supplies line editing, command history, executable completion, scrollback, and Ctrl-C. The shell supports quoted arguments, escapes, built-ins (`help`, `commands`, `echo`, `pwd`, `cd`, `ls`, `cat`, `rm`, `clear`, `exit`), and foreground executables. Its command search includes `/Volumes/mainOS/System/Library/CoreServices` and `/Volumes/mainOS/bin`. It does not implement zsh scripting, pipelines, redirection syntax, or job control.

Examples:

```text
ifconfig
ping -c 4 10.0.2.2
ping -6 -c 4 fec0::2
ping -c 1 127.0.0.1
ping -6 -c 1 ::1
ipctest
sockettest
terminaltest
```

`ping` accepts `-4`/`-6`, `-c` (1–1000 probes), `-W` (1–5000 ms per probe), and `-I` (interface index from `ifconfig`). Addresses must be numeric. `ipctest` launches the existing receiver/sender pair and `sockettest` launches its server/client pair. Other installed test binaries are available by executable name. `terminaltest` checks fork/exec, duplicated standard descriptors, stream backpressure, stderr, and EOF.

Terminal redirects the child's standard descriptors through a local stream socket using `dup2`. Its UI loop polls output while waiting for desktop events. These local sockets are separate from the IP stack. The graphical Terminal and executable shell require ARM64's existing fork/exec and signal support; the i386 networking kernel is supported independently.

## Validation

```sh
python3 tools/check_network.py
python3 tools/check_network_vm.py
python3 tools/check_network_vm.py --arch i386
python3 tools/check_network_session.py
python3 tools/check_network_session.py --terminal
```

The host harness runs the actual protocols with AddressSanitizer and UndefinedBehaviorSanitizer, checking bounded buffer ownership, checksums, inbound echo handling, ARP/NDP, DAD, local addresses, forged echo payloads, timeouts, parsers, and malformed frames. VM tests exercise real VirtIO DMA and gateway replies on all three transports. The shell session uses a private disk and runs the real executables through redirected stdin/stdout. The optional graphical session also launches WindowServer, DesktopService, and Terminal and sends keyboard input. VM logs, packet captures, and screenshots are retained under `/private/tmp`.

The VM runner builds an isolated `network-standalone` ARM64 configuration. The session runner uses that kernel, so run the ARM64 VM check first. Kernel-only boot validation is also registered as `network` in `make check` and `make test TEST=network`.

Protocol and transport references: [VirtIO 1.2](https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html), [IPv6 RFC 8200](https://www.rfc-editor.org/rfc/rfc8200), [Neighbor Discovery RFC 4861](https://www.rfc-editor.org/rfc/rfc4861), and [ICMP RFC 792](https://www.rfc-editor.org/rfc/rfc792).

`rm [-f] [-d] [--] path ...` removes one or more files. Relative paths use the shell's current directory. `-f` ignores missing files, `-d` permits removing empty directories, and `--` ends option parsing. Recursive removal is not implemented.
