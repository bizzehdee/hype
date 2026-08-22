#!/usr/bin/env python3
"""
#602: builds an initial fuzz corpus for each libFuzzer harness in this directory,
encoding the SAME valid descriptor-chain/command shapes core/tests/test_virtio_blk.c,
test_virtio_net_ring.c, test_nvme.c and test_ahci.c build by hand, just serialized
into each harness's own (cfg-writes + guest-RAM bytes) input format instead of C
struct literals -- a byte-for-byte reuse of the .c source is not meaningful here since
the harnesses drive the models through a different call convention (one fuzzer input,
not a sequence of direct C calls), but the wire-format SHAPES (a header + one data
segment + a status descriptor, a Command Header + one PRDT entry, ...) are identical
to what those tests already construct and hype-cfg-spec.md/the VIRTIO spec define.

Re-run after changing a harness's input format: `python3 gen_seeds.py`.
"""
import os
import struct

HERE = os.path.dirname(os.path.abspath(__file__))


def w(path, data):
    full = os.path.join(HERE, path)
    os.makedirs(os.path.dirname(full), exist_ok=True)
    with open(full, "wb") as f:
        f.write(data)
    print("wrote", path, len(data), "bytes")


def le16(v):
    return struct.pack("<H", v)


def le32(v):
    return struct.pack("<I", v)


def le64(v):
    return struct.pack("<Q", v)


# ---- virtio-blk / virtio-net shared cfg offsets (devices/virtio_blk.h) ----
CFG_DEVICE_STATUS = 0x14
CFG_QUEUE_SELECT = 0x16
CFG_QUEUE_SIZE = 0x18
CFG_QUEUE_ENABLE = 0x1C
CFG_QUEUE_DESC_LO = 0x20
CFG_QUEUE_DESC_HI = 0x24
CFG_QUEUE_DRIVER_LO = 0x28
CFG_QUEUE_DRIVER_HI = 0x2C
CFG_QUEUE_DEVICE_LO = 0x30
CFG_QUEUE_DEVICE_HI = 0x34

STATUS_ACK = 0x01
STATUS_DRIVER = 0x02
STATUS_DRIVER_OK = 0x04
STATUS_FEATURES_OK = 0x08


def cfg_write(offset, size_bytes, value):
    return le32(offset) + bytes([size_bytes]) + le32(value)


def blk_setup_writes(ram_base, desc_off, avail_off, used_off, queue_size):
    ws = []
    ws.append(cfg_write(CFG_DEVICE_STATUS, 1, STATUS_ACK))
    ws.append(cfg_write(CFG_DEVICE_STATUS, 1, STATUS_ACK | STATUS_DRIVER))
    ws.append(cfg_write(CFG_DEVICE_STATUS, 1, STATUS_ACK | STATUS_DRIVER | STATUS_FEATURES_OK))
    ws.append(cfg_write(CFG_QUEUE_SELECT, 2, 0))
    ws.append(cfg_write(CFG_QUEUE_SIZE, 2, queue_size))
    ws.append(cfg_write(CFG_QUEUE_DESC_LO, 4, (ram_base + desc_off) & 0xFFFFFFFF))
    ws.append(cfg_write(CFG_QUEUE_DESC_HI, 4, (ram_base + desc_off) >> 32))
    ws.append(cfg_write(CFG_QUEUE_DRIVER_LO, 4, (ram_base + avail_off) & 0xFFFFFFFF))
    ws.append(cfg_write(CFG_QUEUE_DRIVER_HI, 4, (ram_base + avail_off) >> 32))
    ws.append(cfg_write(CFG_QUEUE_DEVICE_LO, 4, (ram_base + used_off) & 0xFFFFFFFF))
    ws.append(cfg_write(CFG_QUEUE_DEVICE_HI, 4, (ram_base + used_off) >> 32))
    ws.append(cfg_write(CFG_QUEUE_ENABLE, 2, 1))
    ws.append(cfg_write(CFG_DEVICE_STATUS, 1,
                         STATUS_ACK | STATUS_DRIVER | STATUS_FEATURES_OK | STATUS_DRIVER_OK))
    return ws


def round_bytes(cfg_writes, bus_master_byte, ram_image):
    body = bytes([len(cfg_writes)]) + b"".join(cfg_writes) + bytes([bus_master_byte])
    return bytes([len(cfg_writes)]) + b"".join(cfg_writes) + bytes([bus_master_byte]) + ram_image


def desc(addr, length, flags, nxt):
    return le64(addr) + le32(length) + le16(flags) + le16(nxt)


VIRTQ_DESC_F_NEXT = 1
VIRTQ_DESC_F_WRITE = 2

RAM_BASE_BLK = 0x10000000
DESC_OFF = 0x0000
AVAIL_OFF = 0x1000
USED_OFF = 0x2000
HDR_OFF = 0x3000
DATA_OFF = 0x3100
STATUS_OFF = 0x3200


def build_virtio_blk_seed(req_type, extra_desc=False):
    ram = bytearray(32 * 1024)
    # descriptor 0: header (16 bytes: type u32, reserved u32, sector u64)
    ram[DESC_OFF:DESC_OFF + 16] = desc(RAM_BASE_BLK + HDR_OFF, 16, VIRTQ_DESC_F_NEXT, 1)
    if req_type == 4:  # FLUSH: header -> status, no data
        ram[DESC_OFF + 16:DESC_OFF + 32] = desc(RAM_BASE_BLK + STATUS_OFF, 1, VIRTQ_DESC_F_WRITE, 0)
    else:
        write_flag = VIRTQ_DESC_F_WRITE if req_type == 0 else 0  # T_IN=0 (read): device writes data
        ram[DESC_OFF + 16:DESC_OFF + 32] = desc(RAM_BASE_BLK + DATA_OFF, 512,
                                                 VIRTQ_DESC_F_NEXT | write_flag, 2)
        ram[DESC_OFF + 32:DESC_OFF + 48] = desc(RAM_BASE_BLK + STATUS_OFF, 1, VIRTQ_DESC_F_WRITE, 0)
    # header: type, reserved, sector=0
    ram[HDR_OFF:HDR_OFF + 4] = le32(req_type)
    ram[HDR_OFF + 4:HDR_OFF + 8] = le32(0)
    ram[HDR_OFF + 8:HDR_OFF + 16] = le64(0)
    # avail ring: flags=0, idx=1, ring[0]=0
    ram[AVAIL_OFF:AVAIL_OFF + 2] = le16(0)
    ram[AVAIL_OFF + 2:AVAIL_OFF + 4] = le16(1)
    ram[AVAIL_OFF + 4:AVAIL_OFF + 6] = le16(0)

    writes = blk_setup_writes(RAM_BASE_BLK, DESC_OFF, AVAIL_OFF, USED_OFF, 8)
    return round_bytes(writes, 1, bytes(ram))


w("corpus/virtio_blk/seed_read.bin", build_virtio_blk_seed(0))
w("corpus/virtio_blk/seed_write.bin", build_virtio_blk_seed(1))
w("corpus/virtio_blk/seed_flush.bin", build_virtio_blk_seed(4))
w("corpus/virtio_blk/seed_get_id.bin", build_virtio_blk_seed(8))

# A cyclic chain (desc0 -> desc1 -> desc0), the #268-class adversarial shape
# test_virtio_blk.c already directs against -- included as a seed rather than left
# purely to mutation, since it's cheap to construct exactly right.
ram_cycle = bytearray(32 * 1024)
ram_cycle[0:16] = desc(RAM_BASE_BLK + HDR_OFF, 16, VIRTQ_DESC_F_NEXT, 1)
ram_cycle[16:32] = desc(RAM_BASE_BLK + DATA_OFF, 16, VIRTQ_DESC_F_NEXT, 0)  # -> back to desc 0
ram_cycle[AVAIL_OFF:AVAIL_OFF + 2] = le16(0)
ram_cycle[AVAIL_OFF + 2:AVAIL_OFF + 4] = le16(1)
ram_cycle[AVAIL_OFF + 4:AVAIL_OFF + 6] = le16(0)
writes = blk_setup_writes(RAM_BASE_BLK, DESC_OFF, AVAIL_OFF, USED_OFF, 8)
w("corpus/virtio_blk/seed_cyclic_chain.bin", round_bytes(writes, 1, bytes(ram_cycle)))

# A descriptor pointing entirely outside the mapped RAM region -- "GPAs outside the
# map" per the ticket.
ram_oob = bytearray(32 * 1024)
ram_oob[0:16] = desc(0xFFFFFFFF00000000, 16, VIRTQ_DESC_F_NEXT, 1)
ram_oob[16:32] = desc(RAM_BASE_BLK + STATUS_OFF, 1, VIRTQ_DESC_F_WRITE, 0)
ram_oob[AVAIL_OFF:AVAIL_OFF + 2] = le16(0)
ram_oob[AVAIL_OFF + 2:AVAIL_OFF + 4] = le16(1)
ram_oob[AVAIL_OFF + 4:AVAIL_OFF + 6] = le16(0)
w("corpus/virtio_blk/seed_gpa_out_of_map.bin", round_bytes(writes, 1, bytes(ram_oob)))


# ---- virtio-net: same cfg-write shape, queue_select 0=RX / 1=TX ----
def net_setup_writes(ram_base, rx_desc, rx_avail, rx_used, tx_desc, tx_avail, tx_used, qsize):
    ws = []
    ws.append(cfg_write(CFG_DEVICE_STATUS, 1, STATUS_ACK))
    ws.append(cfg_write(CFG_DEVICE_STATUS, 1, STATUS_ACK | STATUS_DRIVER))
    ws.append(cfg_write(CFG_DEVICE_STATUS, 1, STATUS_ACK | STATUS_DRIVER | STATUS_FEATURES_OK))
    for qsel, desc_off, avail_off, used_off in (
        (0, rx_desc, rx_avail, rx_used),
        (1, tx_desc, tx_avail, tx_used),
    ):
        ws.append(cfg_write(CFG_QUEUE_SELECT, 2, qsel))
        ws.append(cfg_write(CFG_QUEUE_SIZE, 2, qsize))
        ws.append(cfg_write(CFG_QUEUE_DESC_LO, 4, (ram_base + desc_off) & 0xFFFFFFFF))
        ws.append(cfg_write(CFG_QUEUE_DESC_HI, 4, (ram_base + desc_off) >> 32))
        ws.append(cfg_write(CFG_QUEUE_DRIVER_LO, 4, (ram_base + avail_off) & 0xFFFFFFFF))
        ws.append(cfg_write(CFG_QUEUE_DRIVER_HI, 4, (ram_base + avail_off) >> 32))
        ws.append(cfg_write(CFG_QUEUE_DEVICE_LO, 4, (ram_base + used_off) & 0xFFFFFFFF))
        ws.append(cfg_write(CFG_QUEUE_DEVICE_HI, 4, (ram_base + used_off) >> 32))
        ws.append(cfg_write(CFG_QUEUE_ENABLE, 2, 1))
    ws.append(cfg_write(CFG_DEVICE_STATUS, 1,
                         STATUS_ACK | STATUS_DRIVER | STATUS_FEATURES_OK | STATUS_DRIVER_OK))
    return ws


RAM_BASE_NET = 0x20000000
RX_DESC, RX_AVAIL, RX_USED = 0x0000, 0x1000, 0x1200
TX_DESC, TX_AVAIL, TX_USED = 0x2000, 0x3000, 0x3200
NET_HDR_LEN = 12  # HYPE_VIRTIO_NET_HDR_LEN_MODERN (VERSION_1 negotiated)
RX_BUF_OFF = 0x4000
TX_BUF_OFF = 0x5000

ram_net = bytearray(32 * 1024)
# RX queue: one guest-owned, device-writable buffer big enough for hdr+frame.
ram_net[RX_DESC:RX_DESC + 16] = desc(RAM_BASE_NET + RX_BUF_OFF, 1514 + NET_HDR_LEN,
                                      VIRTQ_DESC_F_WRITE, 0)
ram_net[RX_AVAIL:RX_AVAIL + 2] = le16(0)
ram_net[RX_AVAIL + 2:RX_AVAIL + 4] = le16(1)
ram_net[RX_AVAIL + 4:RX_AVAIL + 6] = le16(0)
# TX queue: header + one Ethernet frame, guest-owned (not device-writable).
ram_net[TX_DESC:TX_DESC + 16] = desc(RAM_BASE_NET + TX_BUF_OFF, NET_HDR_LEN + 64, 0, 0)
ram_net[TX_AVAIL:TX_AVAIL + 2] = le16(0)
ram_net[TX_AVAIL + 2:TX_AVAIL + 4] = le16(1)
ram_net[TX_AVAIL + 4:TX_AVAIL + 6] = le16(0)
# TX buffer: 12-byte virtio-net header (zeroed) + a minimal 64-byte Ethernet frame.
eth = bytes([0x52, 0x54, 0x00, 0x12, 0x34, 0x56] + [0xff] * 6 + [0x08, 0x00]) + bytes(50)
ram_net[TX_BUF_OFF:TX_BUF_OFF + NET_HDR_LEN] = bytes(NET_HDR_LEN)
ram_net[TX_BUF_OFF + NET_HDR_LEN:TX_BUF_OFF + NET_HDR_LEN + len(eth)] = eth

net_writes = net_setup_writes(RAM_BASE_NET, RX_DESC, RX_AVAIL, RX_USED, TX_DESC, TX_AVAIL, TX_USED, 8)
net_round = round_bytes(net_writes, 1, bytes(ram_net))
# Second round: an RX frame for hype_virtio_net_deliver_rx (rx_len u16 + frame bytes).
net_round += le16(64) + bytes(64) + bytes(1514 - 64)
w("corpus/virtio_net/seed_tx_rx.bin", net_round)


# ---- NVMe: register writes to enable the controller + one admin IDENTIFY SQE ----
REG_CC = 0x14
REG_AQA = 0x24
REG_ASQ = 0x28
REG_ACQ = 0x30

NVME_GRAM_BASE = 0x30000000
SQ_OFF = 0x0000  # admin SQ, 64 bytes/entry
CQ_OFF = 0x1000  # admin CQ, 16 bytes/entry
PRP_OFF = 0x2000

CC_EN = 1


def nvme_reg_write(off, val):
    return le32(off) + le32(val)


def build_nvme_seed():
    ram = bytearray(64 * 1024)
    # One admin SQE: IDENTIFY (opcode 0x06), CNS=1 (controller), PRP1 = a 4096-byte buffer.
    sqe = bytearray(64)
    sqe[0] = 0x06  # opcode
    sqe[2:4] = le16(0x1234)  # cid
    sqe[24:32] = le64(NVME_GRAM_BASE + PRP_OFF)  # prp1
    sqe[40:44] = le32(1)  # cdw10: CNS=1
    ram[SQ_OFF:SQ_OFF + 64] = sqe

    regs = [
        nvme_reg_write(REG_ASQ, (NVME_GRAM_BASE + SQ_OFF) & 0xFFFFFFFF),
        nvme_reg_write(REG_ASQ + 4, (NVME_GRAM_BASE + SQ_OFF) >> 32),
        nvme_reg_write(REG_ACQ, (NVME_GRAM_BASE + CQ_OFF) & 0xFFFFFFFF),
        nvme_reg_write(REG_ACQ + 4, (NVME_GRAM_BASE + CQ_OFF) >> 32),
        nvme_reg_write(REG_AQA, (64 << 16) | 64),  # ACQS=64-1, ASQS=64-1 style field (fuzz-approx)
        nvme_reg_write(REG_CC, CC_EN),
    ]
    body = bytes([len(regs)]) + b"".join(regs) + bytes([1]) + bytes(ram)
    # doorbell: sq_tail advance for admin qid 0 so process_sq sees a new entry --
    # modelled purely through guest RAM + registers already written above; the
    # harness itself calls hype_nvme_process_sq() with qid=0 next, taken from one
    # more byte appended here.
    body += bytes([0])  # qid selector byte, %HYPE_NVME_MAX_QUEUES -> 0 (admin)
    return body


w("corpus/nvme/seed_identify.bin", build_nvme_seed())


# ---- AHCI: one Command Header + PRDT entry (ATAPI IDENTIFY-style path is complex;
# seed a plain SATA IDENTIFY DEVICE via the ATA path, which is the simpler shape) ----
AHCI_RAM_BASE = 0x40000000
CLB_OFF = 0x0000       # command list (32 bytes/slot)
CTBL_OFF = 0x1000      # command table for slot 0
FIS_OFF = 0x2000       # H2D register FIS lives at the start of the command table
PRDT_OFF = 0x2080      # PRDT entries start at command-table + 0x80
DATA_OFF_A = 0x3000


def build_ahci_seed(use_atapi):
    ram = bytearray(64 * 1024)
    # Command Header (slot 0, 32 bytes): CFL=5 (20-byte FIS / 4), PRDTL=1, ATAPI/write bits clear,
    # CTBA -> command table.
    hdr = bytearray(32)
    opts = 5  # CFL=5 dwords
    hdr[0] = opts & 0xFF
    hdr[1] = (opts >> 8) & 0xFF
    hdr[2:4] = le16(1)  # PRDTL = 1
    hdr[8:16] = le64(AHCI_RAM_BASE + CTBL_OFF)
    ram[CLB_OFF:CLB_OFF + 32] = hdr

    # Command Table: H2D Register FIS at offset 0 (20 bytes), PRDT entry at +0x80.
    cfis = bytearray(20)
    cfis[0] = 0x27  # FIS_TYPE_REG_H2D
    cfis[1] = 0x80  # C bit set: this is a command
    cfis[2] = 0xEC if not use_atapi else 0xA1  # IDENTIFY DEVICE / IDENTIFY PACKET DEVICE
    ram[CTBL_OFF:CTBL_OFF + 20] = cfis

    prdt = le64(AHCI_RAM_BASE + DATA_OFF_A) + le32(511)  # byte_count-1 -> 512 bytes
    ram[CTBL_OFF + 0x80:CTBL_OFF + 0x80 + 16] = prdt

    # The AHCI harness reads p_clb/p_clbu/p_fb/p_fbu, a PxCMD value, then fills RAM,
    # then a slot number -- build that exact sequence.
    out = bytearray()
    out += le32(AHCI_RAM_BASE + CLB_OFF)  # p_clb
    out += le32(0)                        # p_clbu
    out += le32(AHCI_RAM_BASE + FIS_OFF)  # p_fb
    out += le32(0)                        # p_fbu
    out += le32(0x0011)                   # PxCMD value (ST|FRE bits on)
    out += bytes(ram)
    out += le32(0)                        # slot = 0
    return bytes([1 if use_atapi else 0]) + bytes(out)  # leading byte picks ATAPI vs ATA in harness


w("corpus/ahci/seed_ata_identify.bin", build_ahci_seed(False))
w("corpus/ahci/seed_atapi_identify.bin", build_ahci_seed(True))


# ---- MMIO/PIO register model harness: selector byte, then per-device tuples matching
# fuzz_mmio_regs.c's own per-function byte layout exactly (they differ: ioapic/lapic
# read a u16 offset + u32 value + u8 op/sizesel; pit/ps2_kbd read a u16 port + u8 value
# + u8 op; ps2_mouse reads a bare u8 command). ----
def mmio_seed_wide(which, ops):
    """offset:u16, value:u32, op:u8 -- ioapic (op = read/write bit) and lapic (op = size
    selector index | 0x80 read bit)."""
    out = bytearray([which])
    for offset, value, op in ops:
        out += le16(offset) + le32(value) + bytes([op])
    return bytes(out)


def mmio_seed_narrow(which, ops):
    """port:u16, value:u8, op:u8 -- pit and ps2_kbd."""
    out = bytearray([which])
    for port, value, op in ops:
        out += le16(port) + bytes([value, op])
    return bytes(out)


def mmio_seed_mouse(commands):
    return bytes([4]) + bytes(commands)


w("corpus/mmio/seed_ioapic.bin", mmio_seed_wide(0, [
    (0x00, 0x10, 0),  # IOREGSEL = redirection table entry 0 low dword
    (0x10, 0x000000FF, 0),  # IOWIN write
    (0x00, 0x11, 0),
    (0x10, 0x00000000, 1),
]))
w("corpus/mmio/seed_lapic.bin", mmio_seed_wide(1, [
    (0x0B0, 0, 2),          # EOI write, size index 2 -> 4 bytes
    (0x300, 0x000C0000, 2),  # ICR low, size index 2 -> 4 bytes, write
    (0x310, 0, 2),           # ICR high
]))
w("corpus/mmio/seed_pit.bin", mmio_seed_narrow(2, [
    (0x43, 0x36, 0),  # mode/command register: channel 0, lobyte/hibyte, mode 3
    (0x40, 0xFF, 0),
    (0x40, 0xFF, 0),
    (0x40, 0, 1),
]))
w("corpus/mmio/seed_ps2_kbd.bin", mmio_seed_narrow(3, [
    (0x64, 0xAA, 0),  # self-test command
    (0x60, 0, 1),
    (0x64, 0, 1),
]))
w("corpus/mmio/seed_ps2_mouse.bin", mmio_seed_mouse([0xFF, 0xF4]))  # reset, enable reporting

print("done")
