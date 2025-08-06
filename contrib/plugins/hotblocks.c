/*
 * Copyright (C) 2019, Alex Bennée <alex.bennee@linaro.org>
 *
 * License: GNU GPL, version 2 or later.
 *   See the COPYING file in the top-level directory.
 */
#include <inttypes.h>
#include <assert.h>
#include <stdlib.h>
#include <inttypes.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <glib.h>

#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

static bool do_inline;

/* Plugins need to take care of their own locking */
static GMutex lock;
static GHashTable *hotblocks;
//static guint64 limit = 20;
static FILE *fd3_log_fp = NULL;
static GString *fd3_buffer = NULL;
static qemu_plugin_u64 insn_count;
static qemu_plugin_u64 vinsn_count;
typedef struct {
    char *mnemonic;
} vector_insn_info_t;
/*
 * Counting Structure
 *
 * The internals of the TCG are not exposed to plugins so we can only
 * get the starting PC for each block. We cheat this slightly by
 * checking the number of instructions as well to help
 * differentiate.
 */
typedef struct {
    uint64_t start_addr;
    struct qemu_plugin_scoreboard *exec_count;
    int trans_count;
    unsigned long insns;
    int64_t vector_insn_count;
    int64_t vector_lsinsn_count;
} ExecCount;

static void vcpu_common_insn_cb(unsigned int cpu_index, void *udata)
{
    qemu_plugin_u64_add(insn_count, cpu_index, 1);
}

static void exec_vector_insn_cb(unsigned int vcpu_index, void *userdata)
{
    qemu_plugin_u64_add(vinsn_count, vcpu_index, 1);
    const char *mnemonic = (const char *)userdata;

    if (mnemonic) {
        // 拼接日志到缓冲
        g_string_append_printf(fd3_buffer, "%s\n", mnemonic);
    }

    qemu_plugin_u64_add(insn_count, vcpu_index, 1);
}


static gint cmp_exec_count(gconstpointer a, gconstpointer b, gpointer d)
{
    ExecCount *ea = (ExecCount *) a;
    ExecCount *eb = (ExecCount *) b;
    uint64_t count_a =
        qemu_plugin_u64_sum(qemu_plugin_scoreboard_u64(ea->exec_count));
    uint64_t count_b =
        qemu_plugin_u64_sum(qemu_plugin_scoreboard_u64(eb->exec_count));
    return count_a > count_b ? -1 : 1;
}

static guint exec_count_hash(gconstpointer v)
{
    const ExecCount *e = v;
    return e->start_addr ^ e->insns;
}

static gboolean exec_count_equal(gconstpointer v1, gconstpointer v2)
{
    const ExecCount *ea = v1;
    const ExecCount *eb = v2;
    return (ea->start_addr == eb->start_addr) &&
           (ea->insns == eb->insns);
}

static void exec_count_free(gpointer key, gpointer value, gpointer user_data)
{
    ExecCount *cnt = value;
    qemu_plugin_scoreboard_free(cnt->exec_count);
}

static void plugin_exit(qemu_plugin_id_t id, void *p)
{
    g_autoptr(GString) report = g_string_new("collected ");
    GList *counts, *it;
    int i;

    g_string_append_printf(report, "%d entries in the hash table\n",
                           g_hash_table_size(hotblocks));
    for (i = 0; i < qemu_plugin_num_vcpus(); i++) {
            g_string_append_printf(report, "cpu %d insns: %" PRIu64 "\n",
                                   i, qemu_plugin_u64_get(insn_count, i));
            g_string_append_printf(report, "cpu %d vinsns: %" PRIu64 "\n",
                                   i, qemu_plugin_u64_get(vinsn_count, i));
        }
        g_string_append_printf(report, "total insns: %" PRIu64 "\n",
                               qemu_plugin_u64_sum(insn_count));
        g_string_append_printf(report, "total vinsns: %" PRIu64 "\n",
                               qemu_plugin_u64_sum(vinsn_count));
    counts = g_hash_table_get_values(hotblocks);
    it = g_list_sort_with_data(counts, cmp_exec_count, NULL);

    if (it) {
        g_string_append_printf(report, "pc, tcount, vcount, vlscount, icount, ecount\n");

        for (i = 0; i < g_hash_table_size(hotblocks) && it->next; i++, it = it->next) {
            ExecCount *rec = (ExecCount *) it->data;
            g_string_append_printf(
                report, "0x%016"PRIx64", %d, %"PRId64", %"PRId64", %ld, %"PRId64"\n",
                rec->start_addr, rec->trans_count, rec->vector_insn_count,rec->vector_lsinsn_count,
                rec->insns,
                qemu_plugin_u64_sum(
                    qemu_plugin_scoreboard_u64(rec->exec_count)));
        }
        g_list_free(it);
    }

    qemu_plugin_outs(report->str);

    g_hash_table_foreach(hotblocks, exec_count_free, NULL);
    g_hash_table_destroy(hotblocks);

    if (fd3_log_fp && fd3_buffer) {
        fwrite(fd3_buffer->str, 1, fd3_buffer->len, fd3_log_fp);
        fclose(fd3_log_fp);
        g_string_free(fd3_buffer, TRUE);
    }
}

static void plugin_init(void)
{
    hotblocks = g_hash_table_new(exec_count_hash, exec_count_equal);
}

static void vcpu_tb_exec(unsigned int cpu_index, void *udata)
{
    ExecCount *cnt = (ExecCount *)udata;
    qemu_plugin_u64_add(qemu_plugin_scoreboard_u64(cnt->exec_count),
                        cpu_index, 1);
}

/*
 * When do_inline we ask the plugin to increment the counter for us.
 * Otherwise a helper is inserted which calls the vcpu_tb_exec
 * callback.
 */
static void vcpu_tb_trans(qemu_plugin_id_t id, struct qemu_plugin_tb *tb)
{
    ExecCount *cnt;
    uint64_t pc = qemu_plugin_tb_vaddr(tb);
    size_t insns = qemu_plugin_tb_n_insns(tb);

    g_mutex_lock(&lock);
    {
        ExecCount e;
        e.start_addr = pc;
        e.insns = insns;
        cnt = (ExecCount *) g_hash_table_lookup(hotblocks, &e);
    }

    if (cnt) {
        cnt->trans_count++;
    } else {
        cnt = g_new0(ExecCount, 1);
        cnt->start_addr = pc;
        cnt->trans_count = 1;
        cnt->insns = insns;
        cnt->exec_count = qemu_plugin_scoreboard_new(sizeof(uint64_t));
        cnt->vector_insn_count = 0;
        cnt->vector_lsinsn_count = 0;

        g_hash_table_insert(hotblocks, cnt, cnt);
    }

    // 每次 TB 翻译都注册回调
    for (size_t i = 0; i < insns; i++) {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);

        uint32_t raw_insn = 0;
        qemu_plugin_insn_data(insn, &raw_insn, 4);
        uint8_t opcode = raw_insn & 0x7f;
        uint8_t funct3 = (raw_insn >> 12) & 0x7;
        //uint8_t mew = (raw_insn >> 28) & 0x1;
        bool is_vector = false;

        if (opcode == 0x57) {
            // 明确的 vector OP 或 vector AMO
            if (cnt->trans_count == 1) {
                cnt->vector_insn_count++;
            }
            is_vector = true;
        } else if ( (opcode == 0x07 || opcode == 0x27) &&
                   (funct3 == 0x0 || funct3 == 0x5 || funct3 == 0x6 || funct3 == 0x7)) {
            if (cnt->trans_count == 1) {
                cnt->vector_insn_count++;
                cnt->vector_lsinsn_count++;
            }
            is_vector = true;
        }

        if (is_vector) {
            char *insn_disas = qemu_plugin_insn_disas(insn);
            char *mnemonic = g_strdup(insn_disas);
            qemu_plugin_register_vcpu_insn_exec_cb(
                insn, exec_vector_insn_cb, QEMU_PLUGIN_CB_NO_REGS, mnemonic);
        } else {
            qemu_plugin_register_vcpu_insn_exec_cb(
                insn, vcpu_common_insn_cb, QEMU_PLUGIN_CB_NO_REGS, NULL);
        }
    }

    g_mutex_unlock(&lock);

    if (do_inline) {
        qemu_plugin_register_vcpu_tb_exec_inline_per_vcpu(
            tb, QEMU_PLUGIN_INLINE_ADD_U64,
            qemu_plugin_scoreboard_u64(cnt->exec_count), 1);
    } else {
        qemu_plugin_register_vcpu_tb_exec_cb(tb, vcpu_tb_exec,
                                             QEMU_PLUGIN_CB_NO_REGS,
                                             (void *)cnt);
    }
}

QEMU_PLUGIN_EXPORT
int qemu_plugin_install(qemu_plugin_id_t id, const qemu_info_t *info,
                        int argc, char **argv)
{
    for (int i = 0; i < argc; i++) {
        char *opt = argv[i];
        g_auto(GStrv) tokens = g_strsplit(opt, "=", 2);
        if (g_strcmp0(tokens[0], "inline") == 0) {
            if (!qemu_plugin_bool_parse(tokens[0], tokens[1], &do_inline)) {
                fprintf(stderr, "boolean argument parsing failed: %s\n", opt);
                return -1;
            }
        } else {
            fprintf(stderr, "option parsing failed: %s\n", opt);
            return -1;
        }
    }

    fd3_log_fp = fdopen(3, "w");  // 打开文件描述符 3 为 FILE*
    if (!fd3_log_fp) {
        fprintf(stderr, "[plugin] failed to open fd 3\n");
    }

    plugin_init();

    insn_count = qemu_plugin_scoreboard_u64(qemu_plugin_scoreboard_new(sizeof(uint64_t)));
    vinsn_count = qemu_plugin_scoreboard_u64(qemu_plugin_scoreboard_new(sizeof(uint64_t)));
    fd3_buffer = g_string_new(NULL);
    qemu_plugin_register_vcpu_tb_trans_cb(id, vcpu_tb_trans);
    qemu_plugin_register_atexit_cb(id, plugin_exit, NULL);

    return 0;
}