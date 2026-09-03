# Lab 01 · guest 代码（16 位实模式汇编）
# 构建: as -o guest.o guest.S && ld -Ttext=0 --oformat=binary -o guest.bin guest.o

.code16
.globl _start
_start:
    movw $msg, %si
    movw $0x500, %di          # 目标缓冲: GPA 0x500 (已注册 memslot 内)
copy:
    movb (%si), %al
    movb %al, (%di)
    incw %si
    incw %di
    testb %al, %al
    jnz copy

    # 故意读未映射 GPA 0x100000 → EPT violation → KVM_EXIT_MMIO
    movl $0x100000, %ebx
    movb (%ebx), %al

    hlt                        # → KVM_EXIT_HLT

msg:
    .asciz "Hello from guest!"
