.meta name="MAG alert"
.meta description="Play a sound when\nyour MAG is hungry"
# Original code by Ralf @ GC-Forever

entry_ptr:
reloc0:
  .offsetof start
start:
  .include  WriteCodeBlocks
  # region @ 8000BF30 (44 bytes)
  .data     0x8000BF30  # address
  .data     0x0000002C  # size
  .data     0x9421FFF0  # 8000BF30 => stwu      [r1 - 0x0010], r1
  .data     0x7C0802A6  # 8000BF34 => mflr      r0
  .data     0x90010014  # 8000BF38 => stw       [r1 + 0x0014], r0
  .data     0x3C600002  # 8000BF3C => lis       r3, 0x0002
  .data     0x60632825  # 8000BF40 => ori       r3, r3, 0x2825
  .data     0x38800000  # 8000BF44 => li        r4, 0x0000
  .data     0x4802794D  # 8000BF48 => bl        +0x0002794C /* 80033894 */
  .data     0x80010014  # 8000BF4C => lwz       r0, [r1 + 0x0014]
  .data     0x7C0803A6  # 8000BF50 => mtlr      r0
  .data     0x38210010  # 8000BF54 => addi      r1, r1, 0x0010
  .data     0x4E800020  # 8000BF58 => blr
  # region @ 80110F30 (4 bytes)
  .data     0x80110F30  # address
  .data     0x00000004  # size
  .data     0x4BEFB000  # 80110F30 => b         -0x00105000 /* 8000BF30 */
  # end sentinel
  .data     0x00000000  # address
  .data     0x00000000  # size
