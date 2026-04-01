#ifndef __CPEN212VM_H__
#define __CPEN212VM_H__

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

typedef uint32_t vaddr_t; // virtual address
typedef uint32_t paddr_t; // physical address
typedef uint32_t asid_t;  // address space identifier

typedef enum {
    VM_EXEC = 0, // instruction fetch
    VM_READ = 1, // data read
    VM_WRITE = 2 // data write
} access_type_t;

typedef enum {
    VM_OK = 0,         // success
    VM_BAD_ADDR = 1,   // virtual address / address space not mapped
    VM_BAD_PERM = 2,   // insufficient permissions for access
    VM_DUPLICATE = 4,  // request to map an existing page
    VM_OUT_OF_MEM = 3, // out of physical memory / swap
    VM_BAD_IO = 5      // I/O operation failed
} vm_status_t;

typedef struct {
    vm_status_t status; // translation outcome
    paddr_t addr;       // translated physical address, relevant only if status is VM_OK
} vm_result_t;

// description:
// - initializes a VM system
// arguments:
// - physmem: pointer to an area of at least 4096 * num_phys_pages bytes that models the physical memory
//   - no vm_* functions may access memory outside of [physmem, physmem+4096*num_phys_pages)
//   - all physical addresses are offsets from physmem (i.e., physical address is exactly 0 at physmem)
// - num_phys_pages: total number of 4096-byte physical pages available
//   - it is guaranteed that 4 <= num_phys_pages <= 1048576
//   - physical page 0 starts at physmem
// - swap
//   - if non-null: pointer to a swap file opened in read-write mode with size 4096 * num_swap_pages bytes
//   - if null: no swap space is available for this VM instance
// - num_swap_pages: total number of 4096-byte pages available in the swap file
//   - only relevant if swap is not null
//   - if swap is non-null, it is guaranteed that 2 <= num_swap_pages <= 67108864
// returns:
// - on success, a non-NULL handle that uniquely identifies this VM instance;
//   this will be passed unchanged to other vm_* functions
// - on failure (I/O error), NULL
void *vm_init(void *physmem, size_t num_phys_pages, FILE *swap, size_t num_swap_pages);

// description:
// - translates a virtual address to a physical address if possible
// arguments:
// - vm: a VM system handle returned from vm_init
// - pt: physical address of the top-level page table of the address space being accessed
// - addr: the virtual address to translate
// - access: the access being made (instruction fetch, read, or write)
// - user: the access is a user-level access (i.e., not a kernel access)
// input invariants:
// - pt was previously returned by vm_new_addr_space()
// returns:
// - the success status of the translation:
//   - VM_OK if translation succeeded
//   - VM_BAD_ADDR if there is no translation for this address
//   - VM_BAD_PERM if permissions are not sufficient for the type / source of access requested
//   - VM_BAD_IO if accessing the swap file failed
// - the resulting physical address (relevant only if status is VM_OK)
vm_result_t vm_translate(void *vm, paddr_t pt, vaddr_t addr, access_type_t access, bool user);

// description:
// - adds a top-level page table for an address space
// arguments:
// - vm: a VM system handle returned from vm_init
// - asid: address space ID of for which the toplevel page table should be created
// returns:
// - the success status:
//   - VM_OK if a new page table was created
//   - VM_OUT_OF_MEM if no free pages remain in the physical memory and any relevant swap
//   - VM_BAD_IO if accessing the swap file failed
// - the physical address of the *top-level* page table for this address space (relevant only if status is VM_OK)
// input invariants:
// - 0 <= asid < 512
// - asid is not currently active
// output invariants:
// - a toplevel page table for address space asid exists in physical memory
vm_result_t vm_new_addr_space(void *vm, asid_t asid);

// description:
// - entirely removes an address space
// arguments:
// - vm: a VM system handle returned from vm_init
// - asid: the ID of the address space to be removed
// returns:
// - the success status:
//   - VM_OK if the address space was successfully removed
//   - VM_BAD_ADDR if the toplevel page table for this address pace does not exist
//   - VM_BAD_IO if accessing the swap file failed
// input invariants:
// - 0 <= asid < 512
// - asid is currently active
// output invariants:
// - all pages and page tables used by address space asid are no longer allocated in physical memory or swap
vm_status_t vm_destroy_addr_space(void *vm, asid_t asid);

// description:
// - creates a mapping for a new page in the virtual address space and map it to a physical page
// arguments:
// - vm: a VM system handle returned from vm_init
// - pt: physical address of the top-level page table
// - addr: the virtual address on a page that is to be mapped (not necessarily the start of the page)
// - user: the page is accessible from user-level processes
// - exec: instructions may be fetched from this page
// - write: data may be written to this page
// - read: data may be read from this page
// returns:
// - the success status of the mapping:
//   - VM_OK if the mapping succeeded
//   - VM_OUT_OF_MEM if no free pages remain in the physical memory and any relevant swap
//   - VM_DUPLICATE if a mapping for this page already exists
//   - VM_BAD_IO if accessing the swap file failed
// input invariants:
// - pt was previously returned by vm_new_addr_space()
vm_status_t vm_map_page(void *vm, paddr_t pt, vaddr_t addr, bool user, bool exec, bool write, bool read);

// description:
// - removes the mapping for the page that contains virtual address addr
// - returns any unmapped pages and any page tables with no mappings to the free page pool
// arguments:
// - vm: a VM system handle returned from vm_init
// - pt: physical address of the top-level page table of the address space
// - addr: the virtual address on a page that is to be unmapped (not necessarily the start of the page)
// returns:
// - the success status of the unmapping:
//   - VM_OK if the page was successfully unmapped
//   - VM_BAD_ADDR if this address space has no mapping for virtual address addr
//   - VM_BAD_IO if accessing the swap file failed
vm_status_t vm_unmap_page(void *vm, paddr_t pt, vaddr_t addr);

#endif // __CPEN212VM_H__
