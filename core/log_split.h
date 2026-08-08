#ifndef HYPE_CORE_LOG_SPLIT_H
#define HYPE_CORE_LOG_SPLIT_H

/*
 * #338: classify one captured log record as hype's own output or as a
 * particular VM's guest serial, so the single combined capture buffer can be
 * fanned out to \hype.log and one \vm-<name>.log per VM at flush time.
 *
 * Deliberately a CLASSIFIER over the existing record format rather than a set
 * of extra capture buffers. The guest serial emitter already tags every record
 * ("fw-1 vm%u ttyS%u| "), so the routing information is present in the bytes
 * and no new format, no new buffer and no new lock are required -- which
 * matters here specifically, because raising several capture buffers at once is
 * the change that once produced a 0-byte \HYPEFULL.LOG on real AMD hardware and
 * never reproduced under QEMU (see hype_debug_print). The combined stream stays
 * byte-for-byte what it was; the split files are derived from it.
 *
 * Pure: no allocation, no I/O, no globals. Fully unit tested.
 */

/* The record is hype's own output, not any guest's serial. */
#define HYPE_LOG_VM_HYPE (-1)

/*
 * Returns the VM index whose serial produced `rec`, or HYPE_LOG_VM_HYPE.
 * `rec` is one record WITHOUT its trailing newline; `len` its length.
 *
 * A guest record is recognised only by the complete emitter prefix
 * "fw-1 vm<N> ttyS<M>| ". The narrower test "starts with fw-1 vm" would also
 * capture hype's own per-VM lines (VMSTAT, IOHIST, ISOLATION), which belong in
 * \hype.log -- they are hype reporting ABOUT a guest, not the guest speaking.
 */
int hype_log_record_vm(const char *rec, unsigned int len);

/*
 * Byte offset at which a per-VM file should start writing this record: past
 * the "fw-1 vm<N> " tag for guest records, 0 for anything else.
 *
 * The port tag ("ttyS0| ") is deliberately KEPT: one VM can have several
 * serial ports, so it still distinguishes records inside the file, whereas the
 * VM identity is carried by the filename.
 */
unsigned int hype_log_record_body_off(const char *rec, unsigned int len);

#endif /* HYPE_CORE_LOG_SPLIT_H */
