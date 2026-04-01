#include "cpen212vm.h"

#define BITS_PER_BYTE 8
#define PAGE_SIZE 4096
#define ASID_TABLE_ENTRY 512
#define PAGE_TABLE_ENTRIES 1024

#define PAGE_ADDR_MASK 0xFFFFF000

#define PTE_VALID (1 << 0)
#define PTE_PRESENT (1 << 1)
#define PTE_READ (1 << 2)
#define PTE_WRITE (1 << 3)
#define PTE_EXEC (1 << 4)
#define PTE_USER (1 << 5)
#define PTE_ACCESSED (1 << 6)

typedef struct {
    uint8_t *physmem_base;
    size_t num_phys_pages;
    FILE *swap;
    size_t num_swap_pages;
    paddr_t free_head;
    size_t asid_offset;
    size_t swap_offset;
} vm_state_t;

// Helper function to align x to the next multiple of y
static inline size_t align_bytes(size_t x, size_t y) {
    return (x + (y - 1)) & ~(y - 1);
}

static inline uint32_t pte_get_addr(uint32_t pte) {
    return pte & PAGE_ADDR_MASK;
}

static inline uint32_t pte_get_swap_idx(uint32_t pte) {
    return (pte & PAGE_ADDR_MASK) >> 12;
}

static int alloc_swap_slot(vm_state_t *state) {
    uint8_t *swap_used = state->physmem_base + state->swap_offset;

    for (size_t i = 0; i < state->num_swap_pages; i++) {
        if (swap_used[i] == 0) {
            swap_used[i] = 1;
            return (int)i;
        }
    }
    return -1;
}

static void free_swap(vm_state_t *state, uint32_t swap_idx) {
    if (swap_idx < state->num_swap_pages) {
        uint8_t *swap_used = state->physmem_base + state->swap_offset;
        swap_used[swap_idx] = 0;
    }
}

static vm_status_t swap_write_page(vm_state_t *state, uint32_t swap_idx, paddr_t page_base) {
    size_t total_physical_bytes = state->num_phys_pages * PAGE_SIZE;
    size_t swap_offset = (size_t)swap_idx * PAGE_SIZE;

    if (swap_idx >= state->num_swap_pages) {
        return VM_BAD_IO;
    }

    if (page_base >= total_physical_bytes || (page_base + PAGE_SIZE) > total_physical_bytes) {
        return VM_BAD_IO;
    }

    if (fseek(state->swap, (long)swap_offset, SEEK_SET) != 0) {
        return VM_BAD_IO;
    }

    if (fwrite(state->physmem_base + page_base, 1, PAGE_SIZE, state->swap) != PAGE_SIZE) {
        return VM_BAD_IO;
    }

    if (fflush(state->swap) != 0) {
        return VM_BAD_IO;
    }

    return VM_OK;
}

static vm_status_t swap_read_page(vm_state_t *state, uint32_t swap_idx, paddr_t page_base) {
    size_t total_phys_bytes = state->num_phys_pages * PAGE_SIZE;
    size_t swap_offset = (size_t)swap_idx * PAGE_SIZE;

    if (swap_idx >= state->num_swap_pages) {
        return VM_BAD_IO;
    }

    if (page_base >= total_phys_bytes || (page_base + PAGE_SIZE) > total_phys_bytes) {
        return VM_BAD_IO;
    }

    if (fseek(state->swap, (long)swap_offset, SEEK_SET) != 0) {
        return VM_BAD_IO;
    }

    if (fread(state->physmem_base + page_base, 1, PAGE_SIZE, state->swap) != PAGE_SIZE) {
        return VM_BAD_IO;
    }

    return VM_OK;
}

static vm_status_t get_free_physical_page(vm_state_t *state, paddr_t *out_page) {
    paddr_t new_page = state->free_head;

    if (new_page != 0) {
        state->free_head = *(paddr_t *)(state->physmem_base + new_page);
        *out_page = new_page;
        return VM_OK;
    }

    uint32_t *victim_pte = NULL;
    paddr_t victim_page = 0;
    paddr_t *asid_table = (paddr_t *)(state->physmem_base + state->asid_offset);

    for (size_t a = 0; a < ASID_TABLE_ENTRY && victim_pte == NULL; a++) {
        if (asid_table[a] == 0) continue;

        uint32_t *victim_l1 = (uint32_t *)(state->physmem_base + asid_table[a]);

        for (size_t i = 0; i < PAGE_TABLE_ENTRIES && victim_pte == NULL; i++) {
            if ((victim_l1[i] & PTE_VALID) == 0 || (victim_l1[i] & PTE_PRESENT) == 0) continue;

            paddr_t victim_l2_addr = victim_l1[i] & PAGE_ADDR_MASK;
            uint32_t *victim_l2 = (uint32_t *)(state->physmem_base + victim_l2_addr);

            for (size_t j = 0; j < PAGE_TABLE_ENTRIES; j++) {
                if ((victim_l2[j] & PTE_VALID) && (victim_l2[j] & PTE_PRESENT)) {
                    victim_pte = &victim_l2[j];
                    victim_page = victim_l2[j] & PAGE_ADDR_MASK;
                    break;
                }
            }
        }
    }

    if (victim_pte == NULL) {
        return VM_OUT_OF_MEM;
    }

    int swap_idx = alloc_swap_slot(state);
    if (swap_idx < 0) {
        return VM_OUT_OF_MEM; // Swap is also full
    }

    if (swap_write_page(state, (uint32_t)swap_idx, victim_page) != VM_OK) {
        free_swap(state, (uint32_t)swap_idx);
        return VM_BAD_IO;
    }

    uint32_t victim_old = *victim_pte;
    uint32_t victim_flags = victim_old & (PTE_READ | PTE_WRITE | PTE_EXEC | PTE_USER | PTE_ACCESSED);
    *victim_pte = ((uint32_t)swap_idx << 12) | PTE_VALID | victim_flags;

    *out_page = victim_page;
    return VM_OK;
}

void *vm_init(void *physmem, size_t num_phys_pages, FILE *swap, size_t num_swap_pages) {

    if (physmem == NULL) {
        return NULL;
    }

    vm_state_t *state = (vm_state_t *)physmem;

    // Set a local physical memory pointer
    uint8_t *pm = (uint8_t *)physmem;

    // Clear page 0 metadata
    for (size_t i = 0; i < PAGE_SIZE; i++) {
        pm[i] = 0;
    }

    // Fill the struct with passed in values
    state->physmem_base = pm;
    state->num_phys_pages = num_phys_pages;
    state->swap = swap;
    state->num_swap_pages = num_swap_pages;

    size_t header_bytes = sizeof(vm_state_t);
    size_t asid_offset = align_bytes(header_bytes, sizeof(paddr_t));
    state->asid_offset = asid_offset;

    // Take the overall size of the ASID table (512 * 4)
    size_t asid_table_bytes = ASID_TABLE_ENTRY * sizeof(paddr_t);

    // Implement swap 
    size_t swap_offset = align_bytes(asid_offset + asid_table_bytes, sizeof(paddr_t));
    state->swap_offset = swap_offset;

    size_t swap_table_bytes = num_swap_pages * sizeof(uint8_t);

    // Ensures the metadata is less than the max page size (4KB)
    if ((asid_offset + asid_table_bytes) > PAGE_SIZE) {
        return NULL; // Failure
    }

    if ((swap_offset + swap_table_bytes) > PAGE_SIZE) {
        return NULL;
    }

    // Initializes the ASID table pointer after the offset
    paddr_t *asid_ptr = (paddr_t *)(pm + asid_offset);
    for (size_t i = 0; i < ASID_TABLE_ENTRY; i++) {
        asid_ptr[i] = 0;
    }

    // Initializes the swap table
    uint8_t *swap_used = pm + swap_offset;
    for (size_t i = 0; i < num_swap_pages; i++) {
        swap_used[i] = 0;
    }

    if (num_phys_pages <= 1) {
        state->free_head = 0;
        return state;
    }

    state->free_head = PAGE_SIZE;

    for (size_t i = 1; i < num_phys_pages; i++) {
        paddr_t page_addr = (paddr_t)(i * PAGE_SIZE);
        paddr_t next_addr = ((i + 1) < num_phys_pages) ? (paddr_t)((i + 1) * PAGE_SIZE) : 0;
        *(paddr_t *)(pm + page_addr) = next_addr;
    }

    return state;
}

vm_result_t vm_translate(void *vm, paddr_t pt, vaddr_t addr, access_type_t access, bool user) {

    vm_result_t result;
    vm_state_t *state = (vm_state_t *)vm;

    vaddr_t l1 = (addr >> 22) & 0x3FF;
    vaddr_t l2 = (addr >> 12) & 0x3FF;
    vaddr_t offset = addr & 0xFFF;

    size_t total_bytes = (size_t)PAGE_SIZE * state->num_phys_pages;

    // Check memory bounds
    if ((pt >= total_bytes) || (pt % PAGE_SIZE != 0) || (pt + 4 * (l1 + 1) > total_bytes)) {
        result.status = VM_BAD_ADDR;
        result.addr = 0;
        return result;
    }

    paddr_t *top_pt = (uint32_t *)(state->physmem_base + pt);
    paddr_t pde = top_pt[l1];

    // Validity and present checks
    if (!(pde & PTE_VALID) || !(pde & PTE_PRESENT)) {
        result.status = VM_BAD_ADDR;
        result.addr = 0;
        return result;
    }

    paddr_t l2_pt_paddr = pde & PAGE_ADDR_MASK;

    // Check memory bounds
    if ((l2_pt_paddr >= total_bytes) || (l2_pt_paddr % PAGE_SIZE != 0) || (l2_pt_paddr + 4 * (l2 + 1) > total_bytes)) {
        result.status = VM_BAD_ADDR;
        result.addr = 0;
        return result;
    }

    paddr_t *l2_pt = (uint32_t *)(state->physmem_base + l2_pt_paddr);
    paddr_t pte = l2_pt[l2];

    // Validity and present checks
    if (!(pte & PTE_VALID)) {
        result.status = VM_BAD_ADDR;
        result.addr = 0;
        return result;
    }

    if (!(pte & PTE_PRESENT)) {
        uint32_t swap_idx = (pte & PAGE_ADDR_MASK) >> 12;

        paddr_t data_page = 0;
        vm_status_t status = get_free_physical_page(state, &data_page);

        if (status != VM_OK) {
            result.status = status;
            result.addr = 0;
            return result;
        }

        if (swap_read_page(state, swap_idx, data_page) != VM_OK) {
            *(paddr_t *)(state->physmem_base + data_page) = state->free_head;
            state->free_head = data_page;
            result.status = VM_BAD_IO;
            result.addr = 0;
            return result;
        }

        free_swap(state, swap_idx);

        uint32_t old_flags = pte & (PTE_READ | PTE_WRITE | PTE_EXEC | PTE_USER | PTE_ACCESSED);
        l2_pt[l2] = data_page | PTE_VALID | PTE_PRESENT | old_flags;
        pte = l2_pt[l2];
    }

    // User permissions
    if (user && !(pte & PTE_USER)) {
        result.status = VM_BAD_PERM;
        result.addr = 0;
        return result;
    }

    // Read, write and exec access
    if ((access == VM_READ && !(pte & PTE_READ)) || (access == VM_WRITE && !(pte & PTE_WRITE))
        || (access == VM_EXEC && !(pte & PTE_EXEC))) {
        result.status = VM_BAD_PERM;
        result.addr = 0;
        return result;
    }

    paddr_t phys_page = pte & ~0xFFF;
    paddr_t phys_addr = phys_page + offset;

    if ((phys_page >= total_bytes) || (phys_addr >= total_bytes)) {
        result.status = VM_BAD_ADDR;
        result.addr = 0;
        return result;
    }

    l2_pt[l2] = pte | PTE_ACCESSED;

    result.status = VM_OK;
    result.addr = phys_addr;

    return result;
}

vm_result_t vm_new_addr_space(void *vm, asid_t asid) {
    vm_result_t result;
    vm_state_t *state = (vm_state_t *)vm;
    paddr_t *asid_table = (paddr_t *)(state->physmem_base + state->asid_offset);

    if (asid_table[asid] != 0) {
        result.status = VM_DUPLICATE;
        result.addr = 0;
        return result;
    }

    paddr_t root = 0;
    vm_status_t status = get_free_physical_page(state, &root);
    if (status != VM_OK) {
        result.status = status;
        result.addr = 0;
        return result;
    }

    for (size_t i = 0; i < PAGE_SIZE; i++) {
        state->physmem_base[i + root] = 0;
    }

    asid_table[asid] = root;

    result.status = VM_OK;
    result.addr = root;
    return result;
}

vm_status_t vm_destroy_addr_space(void *vm, asid_t asid) {

    vm_state_t *state = (vm_state_t *)vm;

    paddr_t *asid_table = (paddr_t *)(state->physmem_base + state->asid_offset);
    paddr_t pt = asid_table[asid];

    if (pt == 0) {
        return VM_BAD_ADDR;
    }

    paddr_t *top_table = (uint32_t *)(state->physmem_base + pt);

    for (size_t i = 0; i < PAGE_TABLE_ENTRIES; i++) {
        if ((top_table[i] & PTE_VALID) == 0) {
            continue;
        }

        paddr_t l2_addr = top_table[i] & PAGE_ADDR_MASK;
        uint32_t *l2_table = (uint32_t *)(state->physmem_base + l2_addr);

        for (size_t j = 0; j < PAGE_TABLE_ENTRIES; j++) {
            uint32_t pte = l2_table[j];

            if ((pte & PTE_VALID) == 0) {
                continue;
            }

            if (pte & PTE_PRESENT) {
                paddr_t data_page = pte & PAGE_ADDR_MASK;
                *(paddr_t *)(state->physmem_base + data_page) = state->free_head;
                state->free_head = data_page;
            } else {
                uint32_t swap_idx = (pte & PAGE_ADDR_MASK) >> 12;
                free_swap(state, swap_idx);
            }
        }
        *(paddr_t *)(state->physmem_base + l2_addr) = state->free_head;
        state->free_head = l2_addr;
    }

    *(paddr_t *)(state->physmem_base + pt) = state->free_head;
    state->free_head = pt;
    asid_table[asid] = 0;
    return VM_OK;
}

vm_status_t vm_map_page(void *vm, paddr_t pt, vaddr_t addr, bool user, bool exec, bool write, bool read) {
    vm_state_t *state = (vm_state_t *)vm;

    vaddr_t l1 = (addr >> 22) & 0x3FF;
    vaddr_t l2 = (addr >> 12) & 0x3FF;

    paddr_t *top_table = (uint32_t *)(state->physmem_base + pt);

    bool new_l2 = false;
    paddr_t new_l2_page = 0;

    if ((top_table[l1] & PTE_VALID) == 0) {
        vm_status_t status = get_free_physical_page(state, &new_l2_page);
        if (status != VM_OK) {
            return status;
        }

        for (size_t i = 0; i < PAGE_SIZE; i++) {
            state->physmem_base[i + new_l2_page] = 0;
        }
        top_table[l1] = new_l2_page | PTE_VALID | PTE_PRESENT;
        new_l2 = true;
    }

    paddr_t l2_addr = top_table[l1] & PAGE_ADDR_MASK;
    uint32_t *l2_table = (uint32_t *)(state->physmem_base + l2_addr);

    if ((l2_table[l2] & PTE_VALID) != 0) {
        if (new_l2) {
            top_table[l1] = 0;
            *(paddr_t *)(state->physmem_base + new_l2_page) = state->free_head;
            state->free_head = new_l2_page;
        }
        return VM_DUPLICATE;
    }

    paddr_t data_page = 0;
    vm_status_t status = get_free_physical_page(state, &data_page);
    
    if (status != VM_OK) {
        if (new_l2) {
            top_table[l1] = 0;
            *(paddr_t *)(state->physmem_base + new_l2_page) = state->free_head;
            state->free_head = new_l2_page;
        }
        return status;
    }

    uint32_t pte = data_page | PTE_VALID | PTE_PRESENT;

    if (read) pte |= PTE_READ;
    if (write) pte |= PTE_WRITE;
    if (exec) pte |= PTE_EXEC;
    if (user) pte |= PTE_USER;

    l2_table[l2] = pte;

    return VM_OK;
}

vm_status_t vm_unmap_page(void *vm, paddr_t pt, vaddr_t addr) {
    
    vm_state_t *state = (vm_state_t *)vm;

    vaddr_t l1 = (addr >> 22) & 0x3FF;
    vaddr_t l2 = (addr >> 12) & 0x3FF;

    uint32_t *top_table = (uint32_t *)(state->physmem_base + pt);

    if ((top_table[l1] & PTE_VALID) == 0) {
        return VM_BAD_ADDR;
    }

    paddr_t l2_addr = top_table[l1] & PAGE_ADDR_MASK;
    uint32_t *l2_table = (uint32_t *)(state->physmem_base + l2_addr);

    if ((l2_table[l2] & PTE_VALID) == 0) {
        return VM_BAD_ADDR;
    }

    uint32_t pte = l2_table[l2];

    if (pte & PTE_PRESENT) {
        paddr_t data_page = pte & PAGE_ADDR_MASK;
        *(paddr_t *)(state->physmem_base + data_page) = state->free_head;
        state->free_head = data_page;
    } else {
        uint32_t swap_idx = (pte & PAGE_ADDR_MASK) >> 12;
        free_swap(state, swap_idx);
    }

    l2_table[l2] = 0;

    bool l2_empty = true;
    for (size_t i = 0; i < PAGE_TABLE_ENTRIES; i++) {
        if (l2_table[i] & PTE_VALID) {
            l2_empty = false;
            break;
        }
    }

    if (l2_empty) {
        top_table[l1] = 0;
        *(paddr_t *)(state->physmem_base + l2_addr) = state->free_head;
        state->free_head = l2_addr;
    }
    return VM_OK;
}
