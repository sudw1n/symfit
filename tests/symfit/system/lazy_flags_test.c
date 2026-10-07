/* Deterministic boundary tests linked with the actual system emulator. */
#include "qemu/osdep.h"
#include "cpu.h"
#include "qemu/module.h"
#include "migration/vmstate.h"
#include "sysemu/tcg.h"
#include "exec/exec-all.h"
#include "io/channel-buffer.h"
#include "migration/qemu-file.h"
#include "migration/qemu-file-channel.h"
#include "qapi/error.h"
#include "sysemu/cpus.h"

#define FLAGS_CC (CC_O | CC_S | CC_Z | CC_A | CC_P | CC_C)

typedef struct LazyTuple {
    target_ulong dst, src, src2, dst_label, src_label, src2_label;
    uint32_t op;
    int32_t df;
} LazyTuple;

static LazyTuple tuple(CPUX86State *e)
{
    LazyTuple t = { e->cc_dst, e->cc_src, e->cc_src2,
                    e->shadow_cc_dst, e->shadow_cc_src, e->shadow_cc_src2,
                    e->cc_op, e->df };
    return t;
}

static void assert_tuple(CPUX86State *e, LazyTuple t)
{
    g_assert_cmphex(e->cc_dst, ==, t.dst);
    g_assert_cmphex(e->cc_src, ==, t.src);
    g_assert_cmphex(e->cc_src2, ==, t.src2);
    g_assert_cmphex(e->shadow_cc_dst, ==, t.dst_label);
    g_assert_cmphex(e->shadow_cc_src, ==, t.src_label);
    g_assert_cmphex(e->shadow_cc_src2, ==, t.src2_label);
    g_assert_cmpint(e->cc_op, ==, t.op);
    g_assert_cmpint(e->df, ==, t.df);
}

static void seed(X86CPU *cpu, uint32_t op)
{
    CPUX86State *e = &cpu->env;
    e->eflags = IF_MASK | 2;
    e->cc_dst = 0;
    e->cc_src = op == CC_OP_EFLAGS ? CC_Z | CC_C : 1;
    e->cc_src2 = 0x1234;
    e->cc_op = op;
    e->df = -1;
    /* Opaque sentinel labels: these tests never ask the runtime to resolve them. */
    e->shadow_cc_dst = 11;
    e->shadow_cc_src = 22;
    e->shadow_cc_src2 = 33;
    cpu->lazy_flags_reusable = false;
}

static void assert_concrete(CPUX86State *e, target_ulong flags)
{
    g_assert_cmpint(e->cc_op, ==, CC_OP_EFLAGS);
    g_assert_cmphex(e->cc_src, ==, flags & FLAGS_CC);
    g_assert_cmpint(e->df, ==, (flags & DF_MASK) ? -1 : 1);
    g_assert_cmphex(e->shadow_cc_dst, ==, 0);
    g_assert_cmphex(e->shadow_cc_src, ==, 0);
    g_assert_cmphex(e->shadow_cc_src2, ==, 0);
    g_assert_cmphex(e->eflags, ==, flags & ~(FLAGS_CC | DF_MASK));
}

int __wrap_main(int argc, char **argv, char **envp);
int __wrap_main(int argc, char **argv, char **envp)
{
    X86CPU *cpu;
    CPUX86State *e;
    const uint32_t masks[] = { 0, IF_MASK | IOPL_MASK, UINT32_MAX };
    const uint32_t ops[] = { CC_OP_ADDL, CC_OP_SUBL, CC_OP_EFLAGS };
    const uint32_t flags = CC_Z | CC_P | DF_MASK | AC_MASK | 2;
    const uint32_t reloads[] = { 0, flags, FLAGS_CC | IF_MASK | 2 };
    unsigned i, j;
    uint8_t buf[4];
    target_ulong architectural;
    QIOChannelBuffer *ioc, *input;
    QEMUFile *file;
    QemuThread thread;

    module_call_init(MODULE_INIT_QOM);
    object_property_add_child(object_get_root(), "machine",
                              object_new("pc-i440fx-4.1-machine"), &error_abort);
    tcg_allowed = true;
    cpu = X86_CPU(object_new("qemu64-x86_64-cpu"));
    e = &cpu->env;

    for (i = 0; i < ARRAY_SIZE(masks); i++) {
        for (j = 0; j < ARRAY_SIZE(reloads); j++) {
            seed(cpu, CC_OP_SUBL);
            e->eflags |= VM_MASK | RF_MASK | IOPL_MASK;
            architectural = e->eflags;
            cpu_load_eflags(e, reloads[j], masks[i]);
            g_assert_cmphex(e->eflags, ==,
                            (architectural & ~masks[i]) | (reloads[j] & masks[i]) | 2);
            g_assert_cmphex(e->cc_src, ==, reloads[j] & FLAGS_CC);
            g_assert_cmpint(e->cc_op, ==, CC_OP_EFLAGS);
            g_assert_cmpint(e->df, ==, (reloads[j] & DF_MASK) ? -1 : 1);
            g_assert_cmphex(e->cc_dst, ==, 0);
            g_assert_cmphex(e->cc_src2, ==, 0x1234);
            g_assert_cmphex(e->shadow_cc_dst | e->shadow_cc_src |
                            e->shadow_cc_src2, ==, 0);
        }
    }
    for (i = 0; i < ARRAY_SIZE(ops); i++) {
        LazyTuple t;
        seed(cpu, ops[i]);
        if (ops[i] == CC_OP_ADDL) {
            e->df = 1;
        }
        t = tuple(e);
        architectural = cpu_compute_eflags(e);
        for (j = 0; j < 8; j++) {
            x86_cpu_exec_exit(CPU(cpu));
            g_assert_true(cpu->lazy_flags_reusable);
            g_assert_cmphex(e->eflags, ==, architectural);
            assert_tuple(e, t);
            x86_cpu_exec_enter(CPU(cpu));
            assert_tuple(e, t);
            g_assert_false(cpu->lazy_flags_reusable);
            g_assert_cmphex(cpu_compute_eflags(e), ==, architectural);
            g_assert_cmphex(e->eflags & (FLAGS_CC | DF_MASK), ==, 0);
        }
    }
    /* First entry consumes architectural flags, despite dirty shadow storage. */
    seed(cpu, CC_OP_ADDL);
    e->eflags = flags;
    x86_cpu_exec_enter(CPU(cpu));
    assert_concrete(e, flags);

    /* A second enter without an intervening exit must not reuse the marker. */
    seed(cpu, CC_OP_ADDL);
    x86_cpu_exec_exit(CPU(cpu));
    x86_cpu_exec_enter(CPU(cpu));
    e->eflags = flags;
    x86_cpu_exec_enter(CPU(cpu));
    assert_concrete(e, flags);

    for (i = 0; i < 2; i++) {
        seed(cpu, CC_OP_ADDL);
        x86_cpu_exec_exit(CPU(cpu));
        architectural = i ? e->eflags : flags;
        stl_p(buf, architectural);
        g_assert_cmpint(x86_cpu_gdb_write_register(CPU(cpu), buf,
                                                  CPU_NB_REGS + 1), ==, 4);
        g_assert_false(cpu->lazy_flags_reusable);
        x86_cpu_exec_enter(CPU(cpu));
        assert_concrete(e, architectural);
    }
    seed(cpu, CC_OP_ADDL);
    x86_cpu_exec_exit(CPU(cpu));
    cpu_reset(CPU(cpu));
    g_assert_false(cpu->lazy_flags_reusable);
    architectural = e->eflags;
    x86_cpu_exec_enter(CPU(cpu));
    assert_concrete(e, architectural);

    /* Exercise the production VMState load hook, before fields are replaced. */
    seed(cpu, CC_OP_ADDL);
    x86_cpu_exec_exit(CPU(cpu));
    g_assert_cmpint(vmstate_x86_cpu.pre_load(cpu), ==, 0);
    g_assert_false(cpu->lazy_flags_reusable);
    e->eflags = flags;
    x86_cpu_exec_enter(CPU(cpu));
    assert_concrete(e, flags);
    /* Round-trip the actual CPU migration description into the same CPU. */
    tlb_init(CPU(cpu));
    qemu_thread_get_self(&thread);
    CPU(cpu)->thread = &thread;
    seed(cpu, CC_OP_EFLAGS);
    x86_cpu_exec_exit(CPU(cpu));
    architectural = e->eflags;
    ioc = qio_channel_buffer_new(4096);
    file = qemu_fopen_channel_output(QIO_CHANNEL(ioc));
    g_assert_cmpint(vmstate_save_state(file, &vmstate_x86_cpu, cpu, NULL), ==, 0);
    qemu_put_byte(file, 0); /* terminator for subsection lookahead */
    qemu_fflush(file);
    input = qio_channel_buffer_new(ioc->usage);
    memcpy(input->data, ioc->data, ioc->usage);
    input->usage = ioc->usage;
    g_assert_cmpint(qemu_file_get_error(file), ==, 0);
    g_assert_cmpint(qemu_fclose(file), ==, 0);
    seed(cpu, CC_OP_SUBL);
    x86_cpu_exec_exit(CPU(cpu));
    file = qemu_fopen_channel_input(QIO_CHANNEL(input));
    g_assert_cmpint(vmstate_load_state(file, &vmstate_x86_cpu, cpu,
                                      vmstate_x86_cpu.version_id), ==, 0);
    g_assert_cmpint(qemu_fclose(file), ==, 0);
    g_assert_false(cpu->lazy_flags_reusable);
    g_assert_cmphex(e->shadow_cc_dst | e->shadow_cc_src | e->shadow_cc_src2, ==, 0);
    x86_cpu_exec_enter(CPU(cpu));
    assert_concrete(e, architectural);
    object_unref(OBJECT(ioc));
    object_unref(OBJECT(input));
    puts("lazy flags: reload masks, repeated re-entry, first entry, reset, "
         "VMState round-trip, debugger replacement (including identical) PASS");
    return 0;
}
