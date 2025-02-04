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
  .data     0xA063E970  # 8000D6A4 => lhz       r3, [r3 - 0x1690]
  .data     0x70600003  # 8000D6A8 => andi.     r0, r3, 0x0003
  .data     0x28000003  # 8000D6AC => cmplwi    r0, 3
  .data     0x41820008  # 8000D6B0 => beq       +0x00000008 /* 8000D6B8 */
  .data     0xD03C0084  # 8000D6B4 => stfs      [r28 + 0x0084], f1
  .data     0x4825C2D0  # 8000D6B8 => b         +0x0025C2D0 /* 80269988 */
  # region @ 80269984 (4 bytes)
  .data     0x80269984  # address
  .data     0x00000004  # size
  .data     0x4BDA3D1C  # 80269984 => b         -0x0025C2E4 /* 8000D6A0 */
  # region @ 80346A80 (4 bytes)
  .data     0x80346A80  # address
  .data     0x00000004  # size
  .data     0x38600000  # 80346A80 => li        r3, 0x0000
  # end sentinel
  .data     0x00000000  # address
  .data     0x00000000  # size
