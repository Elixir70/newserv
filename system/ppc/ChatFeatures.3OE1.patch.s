.meta name="Chat"
.meta description="Enable extended\nWord Select and\nstop the Log Window\nfrom scrolling with L+R"
# Original code by Ralf @ GC-Forever

entry_ptr:
reloc0:
  .offsetof start
start:
  .include  WriteCodeBlocks
  # region @ 8000D6A0 (28 bytes)
  .data     0x8000D6A0  # address
  .data     0x0000001C  # size
  .data     0x3C608051  # 8000D6A0 => lis       r3, 0x8051
  .data     0xA0639890  # 8000D6A4 => lhz       r3, [r3 - 0x6770]
  .data     0x70600003  # 8000D6A8 => andi.     r0, r3, 0x0003
  .data     0x28000003  # 8000D6AC => cmplwi    r0, 3
  .data     0x41820008  # 8000D6B0 => beq       +0x00000008 /* 8000D6B8 */
  .data     0xD03C0084  # 8000D6B4 => stfs      [r28 + 0x0084], f1
  .data     0x4825B1C0  # 8000D6B8 => b         +0x0025B1C0 /* 80268878 */
  # region @ 80268874 (4 bytes)
  .data     0x80268874  # address
  .data     0x00000004  # size
  .data     0x4BDA4E2C  # 80268874 => b         -0x0025B1D4 /* 8000D6A0 */
  # region @ 803452A0 (4 bytes)
  .data     0x803452A0  # address
  .data     0x00000004  # size
  .data     0x38600000  # 803452A0 => li        r3, 0x0000
  # end sentinel
  .data     0x00000000  # address
  .data     0x00000000  # size
