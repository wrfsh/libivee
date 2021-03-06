#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include <gelf.h>

#include "libivee/libivee.h"
#include "platform.h"
#include "memory.h"
#include "x86.h"
#include "kvm.h"

struct ivee {
    /* Underlying KVM VM/VCPU */
    struct ivee_kvm_vm* vm;

    /* Active memory map */
    struct ivee_memory_map memory_map;

    /* x86 boot processor state */
    struct x86_cpu_state x86_cpu;

    /* Loaded executable entry point */
    uint64_t entry_addr;

    /* Region that maps guest page table pages */
    struct ivee_guest_memory_region* gpt_mr;

    /* Flag set to true if guest requested termination */
    bool should_terminate;
};

uint64_t ivee_list_platform_capabilities(void)
{
    /* TODO: we don't support any caps yet */
    return 0;
}

int ivee_create(enum ivee_capabilities caps, struct ivee** out_ivee_ptr)
{
    if (!out_ivee_ptr) {
        return -EINVAL;
    }

    if (caps & ~ivee_list_platform_capabilities()) {
        return -ENOTSUP;
    }

    int res = 0;

    struct ivee* ivee = ivee_zalloc(sizeof(*ivee));
    if (!ivee) {
        return -ENOMEM;
    }

    res = ivee_init_kvm();
    if (res != 0) {
        goto error_out;
    }

    ivee->vm = ivee_create_kvm_vm();
    if (!ivee->vm) {
        res = -ENXIO;
        goto error_out;
    }

    res = ivee_init_memory_map(&ivee->memory_map);
    if (res != 0) {
        goto error_out;
    }

    *out_ivee_ptr = ivee;
    return 0;

error_out:
    ivee_destroy(ivee);
    return res;
}

void ivee_destroy(struct ivee* ivee)
{
    if (!ivee) {
        return;
    }

    ivee_release_kvm_vm(ivee->vm);
    ivee_free(ivee);
}

/*
 * We need the following amount of 4KiB guest page table pages to map 1GiB of memory in 4KiB pages:
 * 1 for PML4 + 1 for PDPE + 1 for PDE + 512 for PTEs = 515 pages
 *
 * Statically compute guest GPA for PML4 base address if we map it at the end of 4GiB address space.
 */
#define IVEE_GUEST_MEMORY_SIZE  (0x40000000ull)
#define IVEE_GUEST_PAGES_COUNT  (IVEE_GUEST_MEMORY_SIZE >> X86_PAGE_SHIFT)
#define IVEE_PAGE_TABLE_SIZE    (X86_PAGE_SIZE * 515)
#define IVEE_PML4_BASE_GPA      (IVEE_GUEST_MEMORY_SIZE - IVEE_PAGE_TABLE_SIZE)
#define IVEE_PDPE_BASE_GPA      (IVEE_PML4_BASE_GPA + X86_PAGE_SIZE)
#define IVEE_PDE_BASE_GPA       (IVEE_PDPE_BASE_GPA + X86_PAGE_SIZE)
#define IVEE_PTE_BASE_GPA       (IVEE_PDE_BASE_GPA + X86_PAGE_SIZE)

/*
 * Setup guest identity-mapped 4KB page tables based on current guest memory map.
 * Memory map should be finalized at this point.
 *
 * We will allocate and map host memory enough to hold full 1GiB guest identity mapping,
 * however only currently mapped physical memory will be mapped in those page tables.
 * The rest is reserved for guest to make it's own mappings when needed.
 */
static int init_guest_page_table(struct ivee* ivee)
{
    /* Allocate and map entire page table space.
     * This has an additional benefit of mapping page table pages region first,
     * which we will reflect in page tables later. */
    ivee->gpt_mr = ivee_map_host_memory(&ivee->memory_map,
                                        IVEE_PML4_BASE_GPA,
                                        IVEE_PAGE_TABLE_SIZE,
                                        -1,
                                        false,
                                        IVEE_READ | IVEE_WRITE);
    if (!ivee->gpt_mr) {
        return -ENOMEM;
    }

    uint64_t* pentry = (uint64_t*) ivee->gpt_mr->hva;

    /* 1 entry in PML4 always present */
    *pentry = IVEE_PDPE_BASE_GPA | X86_PTE_PRESENT;
    pentry += X86_PAGE_SIZE / sizeof(*pentry);

    /* 1 entry in PDPE always present */
    *pentry = IVEE_PDE_BASE_GPA | X86_PTE_PRESENT;
    pentry += X86_PAGE_SIZE / sizeof(*pentry);

    /* 4KiB worth of PDE mappings always present */
    for (size_t i = 0; i < X86_PTES_PER_PAGE; ++i, ++pentry) {
        *pentry = (IVEE_PTE_BASE_GPA + X86_PAGE_SIZE * i) | X86_PTE_PRESENT | X86_PTE_RW;
    }

    /* Mark all PTEs as non-present first */
    uint64_t* pte_pages_base = pentry;
    memset(pte_pages_base, 0, IVEE_GUEST_PAGES_COUNT * sizeof(*pentry));

    /* Go over guest regions and map present PTE entries */
    struct ivee_guest_memory_region* mr;
    LIST_FOREACH(mr, &ivee->memory_map.regions, link) {
        for (uint64_t gfn = mr->first_gfn; gfn <= mr->last_gfn; ++gfn) {
            uint64_t* ppte = &pte_pages_base[((gfn >> 9) & 0x1FF) * X86_PTES_PER_PAGE];
            ppte[gfn & 0x1FF] = (gfn << X86_PAGE_SHIFT) |
                (mr->prot & IVEE_WRITE ? X86_PTE_RW : 0) |
                (mr->prot & IVEE_EXEC ?  0 : X86_PTE_NX) |
                X86_PTE_PRESENT;
        }
    }

    return 0;
}

static void reset_x86_segment(struct x86_segment* seg,
                              uint16_t selector,
                              uint32_t limit,
                              uint8_t type,
                              uint8_t flags)
{
    seg->base = 0;
    seg->limit = limit;
    seg->selector = selector;
    seg->type = type;
    seg->dpl = 0;
    seg->flags = flags;
}

/*
 * Set initial state for x86 boot processor.
 * We are putting the cpu directly in x86_64 long mode.
 */
static void init_x86_cpu(struct x86_cpu_state* x86_cpu)
{
    /*
     * IDT and GDT limits are also set to 0 here,
     * which means if exception occurs inside a guest, it will end in a triple fault.
     *
     * For now this is fine (insert meme here).
     * Guest runtime can opt to set it's own exception handlers later on
     */
    memset(x86_cpu, 0, sizeof(*x86_cpu));

    x86_cpu->rflags = 0x2; /* Bit 1 is always set */

    /*
     * Although segmentation is deprecated in 64-bit mode,
     * vmentry checks still require us to setup flat 64-bit segment model.
     */
    reset_x86_segment(&x86_cpu->cs, 0x8, 0xFFFFFFFF, X86_SEG_TYPE_CODE | X86_SEG_TYPE_ACC,
            X86_SEG_S | X86_SEG_P | X86_SEG_G | X86_SEG_L);
    reset_x86_segment(&x86_cpu->ds, 0x10, 0xFFFFFFFF, X86_SEG_TYPE_DATA | X86_SEG_TYPE_ACC,
            X86_SEG_S | X86_SEG_P | X86_SEG_G | X86_SEG_DB);
    reset_x86_segment(&x86_cpu->ss, 0x10, 0xFFFFFFFF, X86_SEG_TYPE_DATA | X86_SEG_TYPE_ACC,
            X86_SEG_S | X86_SEG_P | X86_SEG_G | X86_SEG_DB);
    reset_x86_segment(&x86_cpu->es, 0x10, 0xFFFFFFFF, X86_SEG_TYPE_DATA | X86_SEG_TYPE_ACC,
            X86_SEG_S | X86_SEG_P | X86_SEG_G | X86_SEG_DB);
    reset_x86_segment(&x86_cpu->fs, 0x10, 0xFFFFFFFF, X86_SEG_TYPE_DATA | X86_SEG_TYPE_ACC,
            X86_SEG_S | X86_SEG_P | X86_SEG_G | X86_SEG_DB);
    reset_x86_segment(&x86_cpu->gs, 0x10, 0xFFFFFFFF, X86_SEG_TYPE_DATA | X86_SEG_TYPE_ACC,
            X86_SEG_S | X86_SEG_P | X86_SEG_G | X86_SEG_DB);
    reset_x86_segment(&x86_cpu->tr, 0, 0, X86_SEG_TYPE_TSS32,
            X86_SEG_P);
    reset_x86_segment(&x86_cpu->ldt, 0, 0, X86_SEG_TYPE_LDT,
            X86_SEG_P);

    /*
     * Setup the rest of 64-bit control register context
     */
    x86_cpu->cr0 = 0x80010001;  /* PG | PE | WP */
    x86_cpu->cr4 = 0x20;        /* PAE */
    x86_cpu->efer = 0x500;      /* LMA | LME */
    x86_cpu->cr3 = IVEE_PML4_BASE_GPA;
}

/* Load flat binary into VM and create a page table for it */
static int load_bin(struct ivee* ivee, const char* file)
{
    int res = 0;

    struct stat st;
    res = stat(file, &st);
    if (res != 0) {
        return res;
    }

    size_t size = st.st_size;
    if (size == 0) {
        return -EINVAL;
    }

    /*
     * Memory map the binary and map that into guest directly for readonly.
     * No other memory is mapped.
     */

    int fd = open(file, O_RDONLY);
    if (fd < 0) {
        return fd;
    }

    ivee->entry_addr = 0x400000;
    struct ivee_guest_memory_region* image_mr = ivee_map_host_memory(&ivee->memory_map,
                                                                     ivee->entry_addr,
                                                                     size,
                                                                     fd,
                                                                     true,
                                                                     IVEE_READ | IVEE_EXEC);
    close(fd);
    if (!image_mr) {
        return -ENOMEM;
    }

    return 0;
}

int load_elf64(struct ivee* ivee, const char* file)
{
    int res = 0;

    if (elf_version(EV_CURRENT) == EV_NONE) {
        return -ENOTSUP;
    }

    int fd = open(file, O_RDONLY);
    if (fd < 0) {
        return fd;
    }

    Elf* elf = elf_begin(fd, ELF_C_READ, NULL);
    if (!elf) {
        res = -elf_errno();
        goto error_out;
    }

    if (elf_kind(elf) != ELF_K_ELF) {
        res = -elf_errno();
        goto error_out;
    }

    /*
     * Accepted ELF type: ELF64 executable or dso
     */

    GElf_Ehdr ehdr;
    if (!gelf_getehdr(elf, &ehdr)) {
        res = -elf_errno();
        goto error_out;
    }

    if (gelf_getclass(elf) != ELFCLASS64) {
        res = -ENOTSUP;
        goto error_out;
    }

    if (ehdr.e_type != ET_EXEC && ehdr.e_type != ET_DYN) {
        res = -ENOTSUP;
        goto error_out;
    }

    if (ehdr.e_machine != EM_X86_64) {
        res = -ENOTSUP;
        goto error_out;
    }

    /*
     * For each segment in program header table:
     * - Create a memory region to be mapped into guest with proper permissions
     * - Load segment into memory region
     * - Map region into guest address space at the base address specified in segment entry
     */

    for (size_t i = 0; i < ehdr.e_phnum; ++i ) {
        GElf_Phdr phdr;

        if (gelf_getphdr(elf, i, &phdr) != &phdr) {
            res = -elf_errno();
            goto error_out;
        }

        if (phdr.p_type != PT_LOAD) {
            continue;
        }

        struct ivee_guest_memory_region* segment_mr = ivee_map_host_memory(&ivee->memory_map,
                                                                           phdr.p_vaddr,
                                                                           phdr.p_memsz,
                                                                           -1,
                                                                           false,
                                                                           (phdr.p_flags & PF_X ? IVEE_EXEC : 0) |
                                                                           (phdr.p_flags & PF_R ? IVEE_READ : 0) |
                                                                           (phdr.p_flags & PF_W ? IVEE_WRITE : 0));
        if (!segment_mr) {
            goto error_out;
        }

        ssize_t nbytes = pread(fd, segment_mr->hva, phdr.p_filesz, phdr.p_offset);
        if (nbytes != phdr.p_filesz) {
            res = -errno;
            goto error_out;
        }
    }

    ivee->entry_addr = ehdr.e_entry;

    elf_end(elf);
    close(fd);

    return 0;

error_out:
    elf_end(elf);
    close(fd);

    return res;
}

int load_any(struct ivee* ivee, const char* file)
{
    int res = 0;

    res = load_elf64(ivee, file);
    if (res == 0) {
        return res;
    }

    return load_bin(ivee, file);
}

int ivee_load_executable(struct ivee* ivee, const char* file, ivee_executable_format_t format)
{
    int res = 0;

    if (!ivee) {
        return -EINVAL;
    }

    if (!file) {
        return -EINVAL;
    }

    /* We must have read and execute access for the file */
    if (0 != access(file, R_OK | X_OK)) {
        return -EINVAL;
    }

    switch (format) {
    case IVEE_EXEC_BIN:
        res = load_bin(ivee, file);
        break;
    case IVEE_EXEC_ELF64:
        res = load_elf64(ivee, file);
        break;
    case IVEE_EXEC_ANY:
        res = load_any(ivee, file);
        break;
    default:
        res = -ENOTSUP;
    };

    if (res != 0) {
        goto error_out;
    }

    res = init_guest_page_table(ivee);
    if (res != 0) {
        goto error_out;
    }

    res = ivee_set_kvm_memory_map(ivee->vm, &ivee->memory_map);
    if (res != 0) {
        goto error_out;
    }

    init_x86_cpu(&ivee->x86_cpu);
    return 0;

error_out:
    /* On failure drop memory map we've accumulated */
    ivee_free_memory_map(&ivee->memory_map);
    return res;
}

static int load_vcpu_state(struct ivee* ivee, struct ivee_arch_state* state)
{
    struct x86_cpu_state* x86_cpu = &ivee->x86_cpu;
    x86_cpu->rax = state->rax;
    x86_cpu->rbx = state->rbx;
    x86_cpu->rcx = state->rcx;
    x86_cpu->rdx = state->rdx;
    x86_cpu->rsi = state->rsi;
    x86_cpu->rdi = state->rdi;
    x86_cpu->rbp = state->rbp;
    x86_cpu->r8 = state->r8;
    x86_cpu->r9 = state->r9;
    x86_cpu->r10 = state->r10;
    x86_cpu->r11 = state->r11;
    x86_cpu->r12 = state->r12;
    x86_cpu->r13 = state->r13;
    x86_cpu->r14 = state->r14;
    x86_cpu->r15 = state->r15;
    x86_cpu->rip = ivee->entry_addr;

    return ivee_kvm_load_vcpu_state(ivee->vm, x86_cpu);
}

static int store_vcpu_state(struct ivee* ivee, struct ivee_arch_state* state)
{
    struct x86_cpu_state* x86_cpu = &ivee->x86_cpu;
    int res = ivee_kvm_store_vcpu_state(ivee->vm, x86_cpu);
    if (res != 0) {
        return res;
    }

    state->rax = x86_cpu->rax;
    state->rbx = x86_cpu->rbx;
    state->rcx = x86_cpu->rcx;
    state->rdx = x86_cpu->rdx;
    state->rsi = x86_cpu->rsi;
    state->rdi = x86_cpu->rdi;
    state->r8 = x86_cpu->r8;
    state->r9 = x86_cpu->r9;
    state->r10 = x86_cpu->r10;
    state->r11 = x86_cpu->r11;
    state->r12 = x86_cpu->r12;
    state->r13 = x86_cpu->r13;
    state->r14 = x86_cpu->r14;
    state->r15 = x86_cpu->r15;

    return 0;
}

static int handle_pio(struct ivee* ivee, struct ivee_pio_exit* pio)
{
    switch (pio->port) {
    case IVEE_PIO_EXIT_PORT:
        /* Don't care about value */
        ivee->should_terminate = true;
        return 0;
    default:
        return -ENOTSUP;
    }
}

int ivee_call(struct ivee* ivee, struct ivee_arch_state* state)
{
    int res = 0;

    if (!ivee || !state) {
        return -EINVAL;
    }

    res = load_vcpu_state(ivee, state);
    if (res != 0) {
        return res;
    }

    ivee->should_terminate = false;

    do {
        struct ivee_exit exit;
        res = ivee_kvm_run(ivee->vm, &exit);
        if (res != 0) {
            return res;
        }

        switch (exit.exit_reason) {
        case IVEE_EXIT_IO:
            res = handle_pio(ivee, &exit.io);
            break;
        default:
            res = -ENOTSUP;
            break;
        }

        if (res != 0) {
            return res;
        }
    } while (!ivee->should_terminate);

    return store_vcpu_state(ivee, state);
}
