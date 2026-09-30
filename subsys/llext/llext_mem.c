/*
 * Copyright (c) 2023 Intel Corporation
 * Copyright (c) 2024 Arduino SA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/sys/util.h>
#include <zephyr/sys/minmax.h>
#include <zephyr/llext/loader.h>
#include <zephyr/llext/llext.h>
#include <zephyr/arch/common/instr_mem.h>
#include <zephyr/kernel.h>
#include <zephyr/cache.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(llext, CONFIG_LLEXT_LOG_LEVEL);

#include <string.h>

#include "llext_priv.h"
#include "llext_mem.h"

#ifdef LLEXT_PMP_DEP
#include <zephyr/arch/riscv/csr.h>
#include <zephyr/arch/riscv/pmp.h>
#include <zephyr/linker/linker-defs.h>
#endif

#ifdef CONFIG_LLEXT_HEAP_DYNAMIC
bool llext_heap_inited;
#endif

/* PMP granularity is only defined when RISC-V PMP is built in. */
#ifdef CONFIG_PMP_GRANULARITY
#define LLEXT_PMP_GRANULARITY CONFIG_PMP_GRANULARITY
#else
#define LLEXT_PMP_GRANULARITY 1
#endif

#ifdef LLEXT_PMP_DEP
/* Machine mode can only fetch from the read-only image and from executable
 * SoC PMP regions.
 */
bool llext_dep_allows_exec(const void *addr, size_t len)
{
	uintptr_t start = (uintptr_t)addr;
	uintptr_t end = start + len;

	if (start >= (uintptr_t)__rom_region_start &&
	    end <= (uintptr_t)__rom_region_start + (uintptr_t)__rom_region_size) {
		return true;
	}

	STRUCT_SECTION_FOREACH(pmp_soc_region, region) {
		if ((region->perm & PMP_X) != 0U && start >= (uintptr_t)region->start &&
		    end <= (uintptr_t)region->end) {
			return true;
		}
	}

	return false;
}
#endif

/*
 * Initialize the memory partition associated with the specified memory region
 */
static void llext_init_mem_part(struct llext *ext, enum llext_mem mem_idx,
			uintptr_t start, size_t len)
{
#ifdef CONFIG_USERSPACE
	if (mem_idx < LLEXT_MEM_PARTITIONS) {
		ext->mem_parts[mem_idx].start = start;
		ext->mem_parts[mem_idx].size = len;

		switch (mem_idx) {
		case LLEXT_MEM_TEXT:
#ifdef CONFIG_LLEXT_VENEERS
		case LLEXT_MEM_VENEER:
#endif
			ext->mem_parts[mem_idx].attr = K_MEM_PARTITION_P_RX_U_RX;
			break;
		case LLEXT_MEM_DATA:
		case LLEXT_MEM_BSS:
			ext->mem_parts[mem_idx].attr = K_MEM_PARTITION_P_RW_U_RW;
			break;
		case LLEXT_MEM_RODATA:
			ext->mem_parts[mem_idx].attr = K_MEM_PARTITION_P_RO_U_RO;
			break;
		default:
			break;
		}
	}
#endif

	LOG_DBG("region %d: start %#zx, size %zd", mem_idx, (size_t)start, len);
}

/*
 * Calculate the minimum region size and alignment that can satisfy
 * MMU/MPU requirements. This only applies to regions that contain
 * program-accessible data (not to string tables, for example).
 */
static int llext_region_layout(const elf_shdr_t *region, enum llext_mem mem_idx,
			       uintptr_t *alloc, uintptr_t *align)
{
	uintptr_t region_alloc = region->sh_size;
	uintptr_t region_align = region->sh_addralign;

	if (region->sh_flags & SHF_ALLOC) {
		if (IS_ENABLED(CONFIG_MMU)) {
			/* MMU targets map memory in page-sized chunks. Round
			 * the region to multiples of those.
			 */
			region_alloc = ROUND_UP(region_alloc, LLEXT_PAGE_SIZE);
			region_align = MAX(region_align, LLEXT_PAGE_SIZE);
		} else if (IS_ENABLED(CONFIG_USERSPACE)) {
			if (IS_ENABLED(CONFIG_MPU_REQUIRES_POWER_OF_TWO_ALIGNMENT)) {
				/* Some MPU architectures (ARMv7-M, older ARC) require regions
				 * to be sized and aligned to the same power of two.
				 */
				uintptr_t block_sz =
					max3(region_alloc, region_align, LLEXT_PAGE_SIZE);

				block_sz = 1 << LOG2CEIL(block_sz); /* align to next power of two */
				region_alloc = block_sz;
				region_align = block_sz;
			} else if (IS_ENABLED(CONFIG_ARM_MPU) || IS_ENABLED(CONFIG_ARC_MPU)) {
				/* ARMv8-M and newer ARC MPUs use 32-byte alignment. */
				region_alloc = ROUND_UP(region_alloc, LLEXT_PAGE_SIZE);
				region_align = MAX(region_align, LLEXT_PAGE_SIZE);
			} else if (IS_ENABLED(CONFIG_RISCV_PMP)) {
				/*
				 * RISC-V PMP regions only need to be sized and
				 * aligned to the PMP granularity; TOR matching
				 * removes any power-of-two requirement.
				 */
				region_alloc = ROUND_UP(region_alloc, LLEXT_PMP_GRANULARITY);
				region_align = MAX(region_align, LLEXT_PMP_GRANULARITY);
			} else {
				LOG_ERR("region %d: no memory protection alignment "
					"rule for this architecture", mem_idx);
				return -ENOTSUP;
			}
		}
	}

	*alloc = region_alloc;
	*align = region_align;
	return 0;
}

/* Copy the contents of a region from the ELF file, or zero it if it has none. */
static int llext_fill_region(struct llext_loader *ldr, const elf_shdr_t *region, void *mem)
{
	uintptr_t base = (uintptr_t)mem;
	size_t offset = region->sh_offset;
	size_t length = region->sh_size;
	int ret;

	if (region->sh_type == SHT_NOBITS) {
		memset(mem, 0, region->sh_size);
		return 0;
	}

	if (region->sh_flags & SHF_ALLOC) {
		/* zero out any prepad bytes, not part of the data area */
		size_t prepad = region->sh_info;

		memset((void *)base, 0, prepad);
		base += prepad;
		offset += prepad;
		length -= prepad;
	}

	/* actual data area without prepad bytes */
	ret = llext_seek(ldr, offset);
	if (ret != 0) {
		return ret;
	}

	return llext_read(ldr, (void *)base, length);
}

/*
 * Place a region: at @p dest when the caller already reserved room for it,
 * otherwise in the ELF buffer or in a heap allocation of its own.
 */
static int llext_copy_region(struct llext_loader *ldr, struct llext *ext,
			      enum llext_mem mem_idx, const struct llext_load_param *ldr_parm,
			      void *dest)
{
	int ret;
	elf_shdr_t *region = ldr->sects + mem_idx;
	uintptr_t region_alloc;
	uintptr_t region_align;

	if (!region->sh_size) {
		return 0;
	}
	ext->mem_size[mem_idx] = region->sh_size;

	ret = llext_region_layout(region, mem_idx, &region_alloc, &region_align);
	if (ret != 0) {
		return ret;
	}

	if (dest != NULL) {
		ext->mem[mem_idx] = dest;
		llext_init_mem_part(ext, mem_idx, (uintptr_t)dest, region_alloc);
		return llext_fill_region(ldr, region, dest);
	}

	if (ldr->storage == LLEXT_STORAGE_WRITABLE ||           /* writable storage         */
	    (ldr->storage == LLEXT_STORAGE_PERSISTENT &&        /* || persistent storage    */
	     !(region->sh_flags & SHF_WRITE) &&                 /*    && read-only region   */
	     !(region->sh_flags & SHF_LLEXT_HAS_RELOCS))) {     /*    && no relocs to apply */
		/*
		 * Try to reuse data areas from the ELF buffer, if possible.
		 * If any of the following tests fail, a normal allocation
		 * will be attempted.
		 */
		if (region->sh_type != SHT_NOBITS) {
			/* Region has data in the file, check if peek() is supported */
			ext->mem[mem_idx] = llext_peek(ldr, region->sh_offset);
			if (ext->mem[mem_idx]) {
				if ((IS_ALIGNED(ext->mem[mem_idx], region_align) ||
				     ldr_parm->pre_located) &&
				    ((mem_idx != LLEXT_MEM_TEXT) ||
				     (arch_is_instr_mem(ext->mem[mem_idx], region_alloc) &&
				      llext_dep_allows_exec(ext->mem[mem_idx], region_alloc)))) {
					/* Map this region directly to the ELF buffer */
					llext_init_mem_part(ext, mem_idx,
							    (uintptr_t)ext->mem[mem_idx],
							    region_alloc);
					ext->mem_on_heap[mem_idx] = false;
					return 0;
				}

				if ((mem_idx == LLEXT_MEM_TEXT) &&
				    !arch_is_instr_mem(ext->mem[mem_idx], region_alloc)) {
					LOG_WRN("Cannot reuse ELF buffer for region %d, not "
						"instruction memory: %p-%p",
						mem_idx, ext->mem[mem_idx],
						(void *)((uintptr_t)(ext->mem[mem_idx]) +
							 region->sh_size));
				}
				if (!IS_ALIGNED(ext->mem[mem_idx], region_align)) {
					LOG_WRN("Cannot peek region %d: %p not aligned to %#zx",
						mem_idx, ext->mem[mem_idx], (size_t)region_align);
				}
			}
		} else if (ldr_parm->pre_located
#ifdef CONFIG_LLEXT_VENEERS
		   && mem_idx != LLEXT_MEM_VENEER
#endif
		   ) {
			/*
			 * In pre-located files all regions, including BSS,
			 * are placed by the user with a linker script. No
			 * additional memory allocation is needed here.
			 */
			ext->mem[mem_idx] = NULL;
			ext->mem_on_heap[mem_idx] = false;
			return 0;
		}
	}

	if (ldr_parm->pre_located
#ifdef CONFIG_LLEXT_VENEERS
	    && mem_idx != LLEXT_MEM_VENEER
#endif
	    ) {
		/*
		 * The ELF file is supposed to be pre-located, but some
		 * regions are not accessible or not in the correct place.
		 */
		return -EFAULT;
	}

#ifdef CONFIG_LLEXT_HEAP_MEMBLK
	/* If allocating to heap, allocation must be multiple of block size */
	region_alloc = ROUND_UP(region_alloc, CONFIG_LLEXT_HEAP_MEMBLK_BLOCK_SIZE);
#endif

	/* Allocate a suitably aligned area for the region. */
	if (region->sh_flags & SHF_EXECINSTR) {
		ext->mem[mem_idx] = llext_aligned_alloc_instr(ext, region_align, region_alloc);
	} else {
		ext->mem[mem_idx] = llext_aligned_alloc_data(ext, region_align, region_alloc);
	}

	if (!ext->mem[mem_idx]) {
		LOG_ERR("Failed allocating %zd bytes %zd-aligned for region %d",
			(size_t)region_alloc, (size_t)region_align, mem_idx);
		return -ENOMEM;
	}

	if ((region->sh_flags & SHF_EXECINSTR) != 0 &&
	    !llext_dep_allows_exec(ext->mem[mem_idx], region_alloc)) {
		LOG_ERR("region %d at %p is not executable with data execution prevention",
			mem_idx, ext->mem[mem_idx]);
		ret = -ENOEXEC;
		goto err;
	}

	ext->alloc_size += region_alloc;

	llext_init_mem_part(ext, mem_idx, (uintptr_t)ext->mem[mem_idx],
		region_alloc);

	ret = llext_fill_region(ldr, region, ext->mem[mem_idx]);
	if (ret != 0) {
		goto err;
	}

	ext->mem_on_heap[mem_idx] = true;

	return 0;

err:
	if (region->sh_flags & SHF_EXECINSTR) {
		llext_free_instr(ext, ext->mem[mem_idx]);
	} else {
		llext_free_data(ext, ext->mem[mem_idx]);
	}
	ext->mem[mem_idx] = NULL;
	return ret;
}

int llext_copy_strings(struct llext_loader *ldr, struct llext *ext,
		       const struct llext_load_param *ldr_parm)
{
	llext_heap_reset(ext);

	int ret = llext_copy_region(ldr, ext, LLEXT_MEM_SHSTRTAB, ldr_parm, NULL);

	if (!ret) {
		ret = llext_copy_region(ldr, ext, LLEXT_MEM_STRTAB, ldr_parm, NULL);
	}

	return ret;
}

#ifdef CONFIG_LLEXT_CONTIGUOUS_REGIONS
/* Partition regions in the order they are placed in the region block. */
static const enum llext_mem block_order[] = {
	LLEXT_MEM_TEXT,
#ifdef CONFIG_LLEXT_VENEERS
	LLEXT_MEM_VENEER,
#endif
	LLEXT_MEM_RODATA,
	LLEXT_MEM_DATA,
	LLEXT_MEM_BSS,
};

/* Reserve one heap block for the partition regions and return the place of
 * each region in it. Data and BSS are laid out as one region, whose size is
 * returned in @p data_bss_size, so that a single partition covers both.
 */
static int llext_alloc_region_block(struct llext_loader *ldr, struct llext *ext,
				    void *dest[LLEXT_MEM_PARTITIONS], uintptr_t *data_bss_size)
{
	const elf_shdr_t *data = ldr->sects + LLEXT_MEM_DATA;
	const elf_shdr_t *bss = ldr->sects + LLEXT_MEM_BSS;
	bool merge = (data->sh_size != 0) && (bss->sh_size != 0) &&
		     (data->sh_flags & SHF_ALLOC) && (bss->sh_flags & SHF_ALLOC);
	uintptr_t offset[LLEXT_MEM_PARTITIONS];
	uintptr_t bss_offset = 0;
	uintptr_t block_size = 0;
	uintptr_t block_align = 1;

	for (size_t i = 0; i < ARRAY_SIZE(block_order); i++) {
		enum llext_mem mem_idx = block_order[i];
		elf_shdr_t region = ldr->sects[mem_idx];
		uintptr_t alloc;
		uintptr_t align;
		int ret;

		if (region.sh_size == 0 || !(region.sh_flags & SHF_ALLOC) ||
		    (merge && mem_idx == LLEXT_MEM_BSS)) {
			continue;
		}

		if (merge && mem_idx == LLEXT_MEM_DATA) {
			bss_offset = ROUND_UP(data->sh_size, MAX(bss->sh_addralign, 1));
			region.sh_size = bss_offset + bss->sh_size;
			region.sh_addralign = MAX(data->sh_addralign, bss->sh_addralign);
		}

		ret = llext_region_layout(&region, mem_idx, &alloc, &align);
		if (ret != 0) {
			return ret;
		}

		block_size = ROUND_UP(block_size, MAX(align, 1));
		offset[mem_idx] = block_size;
		block_size += alloc;
		block_align = MAX(block_align, align);

		if (merge && mem_idx == LLEXT_MEM_DATA) {
			offset[LLEXT_MEM_BSS] = offset[mem_idx] + bss_offset;
			*data_bss_size = alloc;
		}
	}

	if (block_size == 0) {
		return 0;
	}

	ext->region_block = llext_aligned_alloc_instr(ext, block_align, block_size);
	if (ext->region_block == NULL) {
		LOG_ERR("Failed allocating %zd bytes %zd-aligned for the region block",
			(size_t)block_size, (size_t)block_align);
		return -ENOMEM;
	}
	ext->alloc_size += block_size;

	for (size_t i = 0; i < ARRAY_SIZE(block_order); i++) {
		enum llext_mem mem_idx = block_order[i];
		const elf_shdr_t *region = ldr->sects + mem_idx;

		if (region->sh_size != 0 && (region->sh_flags & SHF_ALLOC)) {
			dest[mem_idx] = (uint8_t *)ext->region_block + offset[mem_idx];
		}
	}

	return 0;
}
#endif /* CONFIG_LLEXT_CONTIGUOUS_REGIONS */

int llext_copy_regions(struct llext_loader *ldr, struct llext *ext,
		       const struct llext_load_param *ldr_parm)
{
	void *dest[LLEXT_MEM_PARTITIONS] = { NULL };

#ifdef CONFIG_LLEXT_CONTIGUOUS_REGIONS
	uintptr_t data_bss_size = 0;

	/* A detached section is addressed through the text region in place in
	 * the ELF buffer, so an extension with one keeps its regions there.
	 */
	if (!ldr_parm->pre_located && ldr_parm->section_detached == NULL) {
		int ret = llext_alloc_region_block(ldr, ext, dest, &data_bss_size);

		if (ret < 0) {
			return ret;
		}
	}
#endif

	for (enum llext_mem mem_idx = 0; mem_idx < LLEXT_MEM_COUNT; mem_idx++) {
		/* strings have already been copied */
		if (ext->mem[mem_idx]) {
			continue;
		}

		int ret = llext_copy_region(ldr, ext, mem_idx, ldr_parm,
					    mem_idx < LLEXT_MEM_PARTITIONS ? dest[mem_idx] : NULL);

		if (ret < 0) {
			return ret;
		}
	}

#ifdef CONFIG_LLEXT_CONTIGUOUS_REGIONS
	if (data_bss_size != 0) {
		ext->mem_parts[LLEXT_MEM_DATA].size = data_bss_size;
		ext->mem_parts[LLEXT_MEM_BSS].size = 0;
	}
#endif

	if (IS_ENABLED(CONFIG_LLEXT_LOG_LEVEL_DBG)) {
		LOG_DBG("gdb add-symbol-file flags:");
		for (int i = 0; i < ext->sect_cnt; ++i) {
			elf_shdr_t *shdr = ext->sect_hdrs + i;
			enum llext_mem mem_idx = ldr->sect_map[i].mem_idx;
			const char *name = llext_section_name(ldr, ext, shdr);

			/* only show sections mapped to program memory */
			if (mem_idx < LLEXT_MEM_EXPORT) {
				if (name == NULL) {
					LOG_WRN("-s (out of bounds section name string table "
						"index) %#zx",
						(size_t)ext->mem[mem_idx] +
							ldr->sect_map[i].offset);
				} else {
					LOG_DBG("-s %s %#zx", name,
						(size_t)ext->mem[mem_idx] +
							ldr->sect_map[i].offset);
				}
			}
		}
	}

	return 0;
}

void llext_adjust_mmu_permissions(struct llext *ext)
{
#ifdef CONFIG_MMU
	void *addr;
	size_t size;
	uint32_t flags;

	for (enum llext_mem mem_idx = 0; mem_idx < LLEXT_MEM_PARTITIONS; mem_idx++) {
		addr = ext->mem[mem_idx];
		size = ROUND_UP(ext->mem_size[mem_idx], LLEXT_PAGE_SIZE);
		if (size == 0) {
			continue;
		}
		switch (mem_idx) {
		case LLEXT_MEM_TEXT:
#ifdef CONFIG_LLEXT_VENEERS
		case LLEXT_MEM_VENEER:
#endif
			flags = K_MEM_PERM_EXEC;
			break;
		case LLEXT_MEM_DATA:
		case LLEXT_MEM_BSS:
			/* memory is already K_MEM_PERM_RW by default */
			continue;
		case LLEXT_MEM_RODATA:
			flags = 0;
			break;
		default:
			continue;
		}
		sys_cache_data_flush_range(addr, size);
		if ((flags & K_MEM_PERM_EXEC) != 0) {
			/* new code must reach PoU (flush above) before the
			 * stale instruction lines are dropped
			 */
			sys_cache_instr_invd_range(addr, size);
		}
		k_mem_update_flags(addr, size, flags);
	}

	ext->mmu_permissions_set = true;
#endif
}

void llext_free_regions(struct llext *ext)
{
	for (int i = 0; i < LLEXT_MEM_COUNT; i++) {
#ifdef CONFIG_MMU
		if (ext->mmu_permissions_set && ext->mem_size[i] != 0 &&
		    (i == LLEXT_MEM_TEXT || i == LLEXT_MEM_RODATA
#ifdef CONFIG_LLEXT_VENEERS
		     || i == LLEXT_MEM_VENEER
#endif
		     )) {
			/* restore default RAM permissions of changed regions */
			k_mem_update_flags(ext->mem[i],
					   ROUND_UP(ext->mem_size[i], LLEXT_PAGE_SIZE),
					   K_MEM_PERM_RW);
		}
#endif
		if (ext->mem_on_heap[i]) {
			LOG_DBG("freeing memory region %d", i);

			if (i == LLEXT_MEM_TEXT
#ifdef CONFIG_LLEXT_VENEERS
			    || i == LLEXT_MEM_VENEER
#endif
			    ) {
				llext_free_instr(ext, ext->mem[i]);
			} else {
				llext_free_data(ext, ext->mem[i]);
			}

			ext->mem[i] = NULL;
		}
	}

#ifdef CONFIG_LLEXT_CONTIGUOUS_REGIONS
	if (ext->region_block != NULL) {
		llext_free_instr(ext, ext->region_block);
		ext->region_block = NULL;
	}
#endif

	llext_heap_reset(ext);
}

int llext_add_domain(struct llext *ext, struct k_mem_domain *domain)
{
#ifdef CONFIG_USERSPACE
	int ret = 0;

	for (int i = 0; i < LLEXT_MEM_PARTITIONS; i++) {
		if (ext->mem_parts[i].size == 0) {
			continue;
		}
		ret = k_mem_domain_add_partition(domain, &ext->mem_parts[i]);
		if (ret != 0) {
			LOG_ERR("Failed adding memory partition %d to domain %p",
				i, domain);
			return ret;
		}
	}

	return ret;
#else
	return -ENOSYS;
#endif
}

int llext_heap_init_harvard(void *instr_mem, size_t instr_bytes, void *data_mem, size_t data_bytes)
{
#if !defined(CONFIG_LLEXT_HEAP_DYNAMIC) || !defined(CONFIG_HARVARD)
	return -ENOSYS;
#else
	if (llext_heap_inited) {
		return -EEXIST;
	}

	k_heap_init(&llext_instr_heap, instr_mem, instr_bytes);
	k_heap_init(&llext_data_heap, data_mem, data_bytes);

	llext_heap_inited = true;
	return 0;
#endif
}

int llext_heap_init(void *mem, size_t bytes)
{
#if !defined(CONFIG_LLEXT_HEAP_DYNAMIC) || defined(CONFIG_HARVARD)
	return -ENOSYS;
#else
	if (llext_heap_inited) {
		return -EEXIST;
	}

	k_heap_init(&llext_heap, mem, bytes);

	llext_heap_inited = true;
	return 0;
#endif
}

#ifdef CONFIG_LLEXT_HEAP_DYNAMIC
static int llext_loaded(struct llext *ext, void *arg)
{
	return 1;
}
#endif

int llext_heap_uninit(void)
{
#ifdef CONFIG_LLEXT_HEAP_DYNAMIC
	if (!llext_heap_inited) {
		return -EEXIST;
	}
	if (llext_iterate(llext_loaded, NULL)) {
		return -EBUSY;
	}
	llext_heap_inited = false;
	return 0;
#else
	return -ENOSYS;
#endif
}
