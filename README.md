# Virtual Memory Manager (CPEN 212)

## Overview
This repository contains a C implementation of a 32-bit Virtual Memory (VM) manager. It simulates a memory management unit (MMU) featuring a **two-level page table architecture**, **Address Space ID (ASID) management**, and **file-backed swapping**. 

The system manages physical memory allocation, translates virtual addresses to physical addresses, enforces read/write/execute permissions, and handles page faults by evicting pages to a swap file when physical memory is exhausted.

## Features
* **Two-Level Page Table:** 32-bit virtual addresses are split into a 10-bit Level 1 index, a 10-bit Level 2 index, and a 12-bit page offset (4KB page size).
* **ASID Management:** Supports up to 512 distinct address spaces, allowing multiple isolated virtual memory contexts to coexist.
* **Demand Paging & Swapping:** When physical memory is full, the manager evicts pages to a disk-backed swap file using a linear-scan eviction strategy. Missing pages are automatically swapped back into physical memory upon access.
* **Memory Protection:** Enforces granular page-level permissions, including `READ`, `WRITE`, `EXEC`, and `USER` access privileges.
* **Efficient Free List:** Manages unallocated physical frames using an intrusive linked list embedded directly within the free pages.

## Architecture & Memory Layout

### Physical Memory 
The initial block of physical memory (Page 0) is reserved for metadata:
1. `vm_state_t` Struct: Stores base pointers, page counts, and offsets.
2. **ASID Table:** Maps ASIDs (0-511) to the physical addresses of their respective Level 1 page tables.
3. **Swap Table:** A bitmap tracking the usage of available swap slots.

The remaining physical memory is divided into 4KB pages and organized into a linked list. The `free_head` pointer in the state struct points to the first available page.

### Page Table Entry (PTE) Flags
Each 32-bit Page Table Entry utilizes the lower bits for hardware and OS flags:
* `Bit 0`: **VALID** - The entry contains a valid mapping.
* `Bit 1`: **PRESENT** - The page is currently in physical memory (if 0, it is in swap).
* `Bit 2`: **READ** - Read permission.
* `Bit 3`: **WRITE** - Write permission.
* `Bit 4`: **EXEC** - Execute permission.
* `Bit 5`: **USER** - User-mode accessibility.
* `Bit 6`: **ACCESSED** - Set automatically upon translation (useful for future clock-sweep eviction policies).

## Core API

### Initialization
* `vm_init`: Initializes the virtual memory system, carves out Page 0 for metadata, zeroes out the ASID and Swap tables, and builds the physical free list.

### Address Space Management
* `vm_new_addr_space`: Allocates a new Level 1 page table for a given ASID.
* `vm_destroy_addr_space`: Walks the entire page table for a given ASID, safely returning all associated physical pages and swap slots to the system, and tearing down the L1 and L2 tables.

### Memory Mapping
* `vm_map_page`: Maps a specific virtual address to a physical frame, allocating intermediate Level 2 tables dynamically if they do not exist. Sets the requested permissions.
* `vm_unmap_page`: Removes a virtual-to-physical mapping. If an L2 table becomes completely empty as a result, the L2 table itself is unmapped and freed to prevent fragmentation.

### Translation & Fault Handling
* `vm_translate`: Translates a virtual address to a physical address. 
  * Checks bounds and permissions.
  * Triggers a page fault and reads from the swap file if the PTE is `VALID` but not `PRESENT`.
  * Evicts an existing page if a swap-in requires a physical frame and memory is full.

## Eviction Policy
Currently, physical page allocation relies on `get_free_physical_page`. If the free list is empty, the system iterates through the ASID table, probing L1 and L2 tables sequentially to find the first valid, present page to act as the victim. The victim is written to the swap file, its PTE is updated to reflect its swap index, and the newly freed physical page is returned. 

## Dependencies
* Standard C libraries (`stdint.h`, `stddef.h`, `stdio.h`, `stdbool.h`).
* `cpen212vm.h` (Hardware interface and system definitions).
