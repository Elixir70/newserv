.meta name="Invisible MAG"
.meta description="Make MAGs invisible"
# Original code by Ralf @ GC-Forever

entry_ptr:
reloc0:
  .offsetof start
start:
  .include  WriteCodeBlocks
  # region @ 80115298 (4 bytes)
  .data     0x80115298  # address
  .data     0x00000004  # size
  .data     0x480000D4  # 80115298 => b         +0x000000D4 /* 8011536C */
  # end sentinel
  .data     0x00000000  # address
  .data     0x00000000  # size
