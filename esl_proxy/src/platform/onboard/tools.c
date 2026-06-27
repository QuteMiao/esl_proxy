/*
 * Shared onboard utilities (file I/O, fingerprinting, JSON bootstrap helpers).
 */
#define _GNU_SOURCE

#include "tools.h"

#include "platform.h"
#include "onboard_config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

uint64_t fnv1a64(const char *data, size_t len)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    size_t i;

    for (i = 0; i < len; ++i) {
        h ^= (unsigned char)data[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

static int read_elf_build_id(const char *data, size_t len, uint64_t *out)
{
    uint64_t e_shoff;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
    const char *strtab;
    uint64_t strtab_sz;
    uint16_t i;

    if (len < 64) {
        return 0;
    }
    if (memcmp(data, "\x7f""ELF", 4) != 0 || data[4] != 2) {
        return 0;
    }

    memcpy(&e_shoff, data + 40, 8);
    memcpy(&e_shentsize, data + 58, 2);
    memcpy(&e_shnum, data + 60, 2);
    memcpy(&e_shstrndx, data + 62, 2);
    if (e_shentsize != 64 || e_shoff > len || (uint64_t)e_shentsize * e_shnum > len - e_shoff ||
        e_shstrndx >= e_shnum) {
        return 0;
    }

    {
        const char *sh = data + e_shoff + (uint64_t)e_shentsize * e_shstrndx;
        uint64_t off;

        memcpy(&off, sh + 24, 8);
        memcpy(&strtab_sz, sh + 32, 8);
        if (off > len || strtab_sz > len - off) {
            return 0;
        }
        strtab = data + off;
    }

    for (i = 0; i < e_shnum; ++i) {
        const char *sh = data + e_shoff + (uint64_t)e_shentsize * i;
        uint32_t sh_name;
        uint32_t sh_type;
        uint64_t sh_off;
        uint64_t sh_size;

        memcpy(&sh_name, sh + 0, 4);
        memcpy(&sh_type, sh + 4, 4);
        memcpy(&sh_off, sh + 24, 8);
        memcpy(&sh_size, sh + 32, 8);
        if (sh_type != 7 || sh_name >= strtab_sz) {
            continue;
        }
        if (strcmp(strtab + sh_name, ".note.gnu.build-id") != 0) {
            continue;
        }
        if (sh_size < 16 || sh_off > len || sh_size > len - sh_off) {
            return 0;
        }
        {
            const char *p = data + sh_off;
            uint32_t namesz;
            uint32_t descsz;
            uint32_t type;
            size_t name_aligned;
            const char *desc;

            memcpy(&namesz, p + 0, 4);
            memcpy(&descsz, p + 4, 4);
            memcpy(&type, p + 8, 4);
            if (type != 3 || descsz < 8 || namesz > sh_size) {
                return 0;
            }
            name_aligned = (namesz + 3u) & ~3u;
            if (name_aligned > sh_size - 12 || sh_size - 12 - name_aligned < 8) {
                return 0;
            }
            desc = p + 12 + name_aligned;
            memcpy(out, desc, 8);
            return 1;
        }
    }
    return 0;
}

int esl_elf_lookup_symbol(const char *data, size_t len, const char *name, uint64_t *out_value)
{
    uint64_t e_shoff;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
    const char *strtab = NULL;
    uint64_t strtab_sz = 0;
    uint64_t symtab_off = 0;
    uint64_t symtab_size = 0;
    uint64_t sym_ent_size = 0;
    uint32_t symtab_link = 0;
    uint16_t i;

    if (data == NULL || name == NULL || out_value == NULL || len < 64) {
        return -1;
    }
    if (memcmp(data, "\x7f""ELF", 4) != 0 || data[4] != 2) {
        return -1;
    }

    memcpy(&e_shoff, data + 40, 8);
    memcpy(&e_shentsize, data + 58, 2);
    memcpy(&e_shnum, data + 60, 2);
    memcpy(&e_shstrndx, data + 62, 2);
    if (e_shentsize != 64 || e_shoff > len || (uint64_t)e_shentsize * e_shnum > len - e_shoff ||
        e_shstrndx >= e_shnum) {
        return -1;
    }

    for (i = 0; i < e_shnum; ++i) {
        const char *sh = data + e_shoff + (uint64_t)e_shentsize * i;
        uint32_t sh_name;
        uint32_t sh_type;
        uint64_t sh_off;
        uint64_t sh_size;
        uint64_t sh_entsize;
        uint32_t sh_link;

        memcpy(&sh_name, sh + 0, 4);
        memcpy(&sh_type, sh + 4, 4);
        memcpy(&sh_off, sh + 24, 8);
        memcpy(&sh_size, sh + 32, 8);
        memcpy(&sh_entsize, sh + 56, 8);
        memcpy(&sh_link, sh + 40, 4);
        if (sh_type != 2) {
            continue;
        }
        symtab_off = sh_off;
        symtab_size = sh_size;
        sym_ent_size = sh_entsize;
        symtab_link = sh_link;
    }

    if (symtab_off == 0 || sym_ent_size == 0 || symtab_size == 0 || symtab_link >= e_shnum ||
        symtab_off > len || symtab_size > len - symtab_off) {
        return -1;
    }

    {
        const char *sh = data + e_shoff + (uint64_t)e_shentsize * symtab_link;
        uint64_t off;

        memcpy(&off, sh + 24, 8);
        memcpy(&strtab_sz, sh + 32, 8);
        if (off > len || strtab_sz > len - off) {
            return -1;
        }
        strtab = data + off;
    }

    for (uint64_t off = 0; off + sym_ent_size <= symtab_size; off += sym_ent_size) {
        const char *sym = data + symtab_off + off;
        uint32_t st_name;
        uint64_t st_value;

        memcpy(&st_name, sym + 0, 4);
        memcpy(&st_value, sym + 8, 8);
        if (st_name == 0 || st_name >= strtab_sz) {
            continue;
        }
        if (strcmp(strtab + st_name, name) == 0) {
            *out_value = st_value;
            return 0;
        }
    }

    return -1;
}

uint64_t esl_fingerprint_bytes(const void *data, size_t len)
{
    uint64_t v = 0;

    if (read_elf_build_id((const char *)data, len, &v)) {
        return v;
    }
    return fnv1a64((const char *)data, len);
}

int64_t *grow_array(int64_t **arr, size_t *cap, size_t *len, int64_t value)
{
    if (*len >= *cap) {
        size_t new_cap = (*cap == 0) ? 16 : (*cap * 2);
        int64_t *new_arr = (int64_t *)realloc(*arr, new_cap * sizeof(int64_t));

        if (new_arr == NULL) {
            return NULL;
        }
        *arr = new_arr;
        *cap = new_cap;
    }
    (*arr)[(*len)++] = value;
    return *arr;
}

int read_file(const char *path, char **out_data, size_t *out_len)
{
    FILE *f;
    long sz;
    char *buf;
    size_t nread;

    f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "open %s: %s\n", path, strerror(errno));
        return 0;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fprintf(stderr, "seek %s failed\n", path);
        fclose(f);
        return 0;
    }
    sz = ftell(f);
    if (sz <= 0) {
        fprintf(stderr, "empty file: %s\n", path);
        fclose(f);
        return 0;
    }
    buf = (char *)malloc((size_t)sz);
    if (buf == NULL) {
        fclose(f);
        return 0;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        free(buf);
        fclose(f);
        return 0;
    }
    nread = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (nread != (size_t)sz) {
        free(buf);
        return 0;
    }
    *out_data = buf;
    *out_len = (size_t)sz;
    return 1;
}

int write_bytes(const char *path, const char *data, uint64_t len)
{
    char tmp_path[320];
    FILE *f;
    size_t written;

    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%d", path, (int)getpid());
    f = fopen(tmp_path, "wb");
    if (f == NULL) {
        fprintf(stderr, "open %s for write failed: %s\n", tmp_path, strerror(errno));
        return 0;
    }
    written = fwrite(data, 1, (size_t)len, f);
    if (written != (size_t)len || ferror(f)) {
        fprintf(stderr, "write %s failed\n", tmp_path);
        fclose(f);
        unlink(tmp_path);
        return 0;
    }
    fclose(f);
    (void)chmod(tmp_path, 0755);
    if (rename(tmp_path, path) != 0) {
        fprintf(stderr, "rename %s -> %s failed: %s\n", tmp_path, path, strerror(errno));
        unlink(tmp_path);
        return 0;
    }
    return 1;
}

void esl_make_inner_basename(uint64_t fp, int device_id, char *buf, size_t buf_size)
{
    snprintf(buf, buf_size, ESL_INNER_SO_BASENAME_FMT, (unsigned long)fp, device_id);
}

void esl_make_aicpu_op_type(const char *base, uint64_t fp, char *buf, size_t buf_size)
{
    snprintf(buf, buf_size, "%s_%016lx", base, (unsigned long)fp);
}

int esl_write_aicpu_kernel_json(const char *path, const char *kernel_so, uint64_t fp)
{
    FILE *out;
    char init_op[128];
    char exec_op[128];

    out = fopen(path, "w");
    if (out == NULL) {
        return 0;
    }

    esl_make_aicpu_op_type(ESL_AICPU_INIT_NAME, fp, init_op, sizeof(init_op));
    esl_make_aicpu_op_type(ESL_AICPU_EXEC_NAME, fp, exec_op, sizeof(exec_op));

    fprintf(out, "{\n");
    fprintf(out, "  \"%s\": {\n", init_op);
    fprintf(out, "    \"opInfo\": {\n");
    fprintf(out, "      \"functionName\": \"%s\",\n", ESL_AICPU_INIT_NAME);
    fprintf(out, "      \"kernelSo\": \"%s\",\n", kernel_so);
    fprintf(out, "      \"opKernelLib\": \"AICPUKernel\",\n");
    fprintf(out, "      \"computeCost\": \"100\",\n");
    fprintf(out, "      \"engine\": \"DNN_VM_AICPU\",\n");
    fprintf(out, "      \"flagAsync\": \"False\",\n");
    fprintf(out, "      \"flagPartial\": \"False\",\n");
    fprintf(out, "      \"userDefined\": \"False\"\n");
    fprintf(out, "    }\n  },\n");
    fprintf(out, "  \"%s\": {\n", exec_op);
    fprintf(out, "    \"opInfo\": {\n");
    fprintf(out, "      \"functionName\": \"%s\",\n", ESL_AICPU_EXEC_NAME);
    fprintf(out, "      \"kernelSo\": \"%s\",\n", kernel_so);
    fprintf(out, "      \"opKernelLib\": \"AICPUKernel\",\n");
    fprintf(out, "      \"computeCost\": \"100\",\n");
    fprintf(out, "      \"engine\": \"DNN_VM_AICPU\",\n");
    fprintf(out, "      \"flagAsync\": \"False\",\n");
    fprintf(out, "      \"flagPartial\": \"False\",\n");
    fprintf(out, "      \"userDefined\": \"False\"\n");
    fprintf(out, "    }\n  }\n");
    fprintf(out, "}\n");
    fclose(out);
    return 1;
}

uint64_t get_time_ns(void)
{
    return esl_onboard_time_ns();
}

#ifdef ESL_PROXY_ONBOARD_HOST

#include <acl/acl_rt.h>

void esl_host_dump_device_wall(const void *dev_wall_ptr)
{
    uint64_t wall[ESL_DEVICE_WALL_SLOTS];
    aclError rc;

    if (dev_wall_ptr == NULL) {
        return;
    }
    rc = aclrtMemcpy(wall, sizeof(wall), dev_wall_ptr, sizeof(wall),
        ACL_MEMCPY_DEVICE_TO_HOST);
    if (rc != ACL_SUCCESS) {
        fprintf(stderr, "[esl_proxy] D2H device_wall failed: %d\n", (int)rc);
        return;
    }

    fprintf(stderr,
        "[esl_proxy] device_wall stats: task_cnt=%llu subtask_cnt=%llu "
        "completed_cnt=%llu wall_ns=%llu\n",
        (unsigned long long)wall[0], (unsigned long long)wall[1],
        (unsigned long long)wall[2], (unsigned long long)wall[3]);
    fprintf(stderr,
        "[esl_proxy] device_wall diag: commit=%llu n_uncomp=%llu "
        "first_uncomp=%u pred_cnt[first]=%u ready_cube=%u ready_vec=%u\n",
        (unsigned long long)wall[4], (unsigned long long)wall[5],
        (unsigned)(wall[6] & 0xffffffffULL), (unsigned)(wall[6] >> 32),
        (unsigned)(wall[7] & 0xffffffffULL), (unsigned)(wall[7] >> 32));
    fflush(stderr);
}

#endif /* ESL_PROXY_ONBOARD_HOST */
