// SPDX-License-Identifier: GPL-2.0-only
/*
 * AArch64 loadable module support.
 *
 * Copyright (C) 2012 ARM Limited
 *
 * Author: Will Deacon <will.deacon@arm.com>
 */

#define pr_fmt(fmt) "Modules: " fmt

#include <linux/bitops.h>
#include <linux/elf.h>
#include <linux/ftrace.h>
#include <linux/gfp.h>
#include <linux/kasan.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/moduleloader.h>
#include <linux/random.h>
#include <linux/scs.h>
#include <linux/vmalloc.h>

#include <asm/alternative.h>
#include <asm/insn.h>
#include <asm/kvm_hyptrace.h>
#include <asm/kvm_hypevents_defs.h>
#include <asm/scs.h>
#include <asm/sections.h>

#ifdef CONFIG_RKP
u64 module_direct_base __ro_after_init = 0;
u64 module_plt_base __ro_after_init = 0;
#else
static u64 module_direct_base __ro_after_init = 0;
static u64 module_plt_base __ro_after_init = 0;
#endif

/*
 * Choose a random page-aligned base address for a window of 'size' bytes which
 * entirely contains the interval [start, end - 1].
 */
static u64 __init random_bounding_box(u64 size, u64 start, u64 end)
{
	u64 max_pgoff, pgoff;

	if ((end - start) >= size)
		return 0;

	max_pgoff = (size - (end - start)) / PAGE_SIZE;
	pgoff = get_random_u32_inclusive(0, max_pgoff);

	return start - pgoff * PAGE_SIZE;
}

/*
 * Modules may directly reference data and text anywhere within the kernel
 * image and other modules. References using PREL32 relocations have a +/-2G
 * range, and so we need to ensure that the entire kernel image and all modules
 * fall within a 2G window such that these are always within range.
 *
 * Modules may directly branch to functions and code within the kernel text,
 * and to functions and code within other modules. These branches will use
 * CALL26/JUMP26 relocations with a +/-128M range. Without PLTs, we must ensure
 * that the entire kernel text and all module text falls within a 128M window
 * such that these are always within range. With PLTs, we can expand this to a
 * 2G window.
 *
 * We chose the 128M region to surround the entire kernel image (rather than
 * just the text) as using the same bounds for the 128M and 2G regions ensures
 * by construction that we never select a 128M region that is not a subset of
 * the 2G region. For very large and unusual kernel configurations this means
 * we may fall back to PLTs where they could have been avoided, but this keeps
 * the logic significantly simpler.
 */
static int __init module_init_limits(void)
{
	u64 kernel_end = (u64)_end;
	u64 kernel_start = (u64)_text;
	u64 kernel_size = kernel_end - kernel_start;

	/*
	 * The default modules region is placed immediately below the kernel
	 * image, and is large enough to use the full 2G relocation range.
	 */
	BUILD_BUG_ON(KIMAGE_VADDR != MODULES_END);
	BUILD_BUG_ON(MODULES_VSIZE < SZ_2G);

	if (!kaslr_enabled()) {
		if (kernel_size < SZ_128M)
			module_direct_base = kernel_end - SZ_128M;
		if (kernel_size < SZ_2G)
			module_plt_base = kernel_end - SZ_2G;
	} else {
		u64 min = kernel_start;
		u64 max = kernel_end;

		if (IS_ENABLED(CONFIG_RANDOMIZE_MODULE_REGION_FULL)) {
			pr_info("2G module region forced by RANDOMIZE_MODULE_REGION_FULL\n");
		} else {
			module_direct_base = random_bounding_box(SZ_128M, min, max);
			if (module_direct_base) {
				min = module_direct_base;
				max = module_direct_base + SZ_128M;
			}
		}

		module_plt_base = random_bounding_box(SZ_2G, min, max);
	}

	pr_info("%llu pages in range for non-PLT usage",
		module_direct_base ? (SZ_128M - kernel_size) / PAGE_SIZE : 0);
	pr_info("%llu pages in range for PLT usage",
		module_plt_base ? (SZ_2G - kernel_size) / PAGE_SIZE : 0);

	return 0;
}
subsys_initcall(module_init_limits);

void *module_alloc(unsigned long size)
{
	void *p = NULL;

	/*
	 * Where possible, prefer to allocate within direct branch range of the
	 * kernel such that no PLTs are necessary.
	 */
	if (module_direct_base) {
		p = __vmalloc_node_range(size, MODULE_ALIGN,
					 module_direct_base,
					 module_direct_base + SZ_128M,
					 GFP_KERNEL | __GFP_NOWARN,
					 PAGE_KERNEL, 0, NUMA_NO_NODE,
					 __builtin_return_address(0));
	}

	if (!p && module_plt_base) {
		p = __vmalloc_node_range(size, MODULE_ALIGN,
					 module_plt_base,
					 module_plt_base + SZ_2G,
					 GFP_KERNEL | __GFP_NOWARN,
					 PAGE_KERNEL, 0, NUMA_NO_NODE,
					 __builtin_return_address(0));
	}

	if (!p) {
		pr_warn_ratelimited("%s: unable to allocate memory\n",
				    __func__);
	}

	if (p && (kasan_alloc_module_shadow(p, size, GFP_KERNEL) < 0)) {
		vfree(p);
		return NULL;
	}

	/* Memory is intended to be executable, reset the pointer tag. */
	return kasan_reset_tag(p);
}

enum aarch64_reloc_op {
	RELOC_OP_NONE,
	RELOC_OP_ABS,
	RELOC_OP_PREL,
	RELOC_OP_PAGE,
};

static u64 do_reloc(enum aarch64_reloc_op reloc_op, __le32 *place, u64 val)
{
	switch (reloc_op) {
	case RELOC_OP_ABS:
		return val;
	case RELOC_OP_PREL:
		return val - (u64)place;
	case RELOC_OP_PAGE:
		return (val & ~0xfff) - ((u64)place & ~0xfff);
	case RELOC_OP_NONE:
		return 0;
	}

	pr_err("do_reloc: unknown relocation operation %d\n", reloc_op);
	return 0;
}

static int reloc_data(enum aarch64_reloc_op op, void *place, u64 val, int len)
{
	s64 sval = do_reloc(op, place, val);

	/*
	 * The ELF psABI for AArch64 documents the 16-bit and 32-bit place
	 * relative and absolute relocations as having a range of [-2^15, 2^16)
	 * or [-2^31, 2^32), respectively. However, in order to be able to
	 * detect overflows reliably, we have to choose whether we interpret
	 * such quantities as signed or as unsigned, and stick with it.
	 * The way we organize our address space requires a signed
	 * interpretation of 32-bit relative references, so let's use that
	 * for all R_AARCH64_PRELxx relocations. This means our upper
	 * bound for overflow detection should be Sxx_MAX rather than Uxx_MAX.
	 */

	switch (len) {
	case 16:
		*(s16 *)place = sval;
		switch (op) {
		case RELOC_OP_ABS:
			if (sval < 0 || sval > U16_MAX)
				return -ERANGE;
			break;
		case RELOC_OP_PREL:
			if (sval < S16_MIN || sval > S16_MAX)
				return -ERANGE;
			break;
		default:
			pr_err("Invalid 16-bit data relocation (%d)\n", op);
			return 0;
		}
		break;
	case 32:
		*(s32 *)place = sval;
		switch (op) {
		case RELOC_OP_ABS:
			if (sval < 0 || sval > U32_MAX)
				return -ERANGE;
			break;
		case RELOC_OP_PREL:
			if (sval < S32_MIN || sval > S32_MAX)
				return -ERANGE;
			break;
		default:
			pr_err("Invalid 32-bit data relocation (%d)\n", op);
			return 0;
		}
		break;
	case 64:
		*(s64 *)place = sval;
		break;
	default:
		pr_err("Invalid length (%d) for data relocation\n", len);
		return 0;
	}
	return 0;
}

enum aarch64_insn_movw_imm_type {
	AARCH64_INSN_IMM_MOVNZ,
	AARCH64_INSN_IMM_MOVKZ,
};

static int reloc_insn_movw(enum aarch64_reloc_op op, __le32 *place, u64 val,
			   int lsb, enum aarch64_insn_movw_imm_type imm_type)
{
	u64 imm;
	s64 sval;
	u32 insn = le32_to_cpu(*place);

	sval = do_reloc(op, place, val);
	imm = sval >> lsb;

	if (imm_type == AARCH64_INSN_IMM_MOVNZ) {
		/*
		 * For signed MOVW relocations, we have to manipulate the
		 * instruction encoding depending on whether or not the
		 * immediate is less than zero.
		 */
		insn &= ~(3 << 29);
		if (sval >= 0) {
			/* >=0: Set the instruction to MOVZ (opcode 10b). */
			insn |= 2 << 29;
		} else {
			/*
			 * <0: Set the instruction to MOVN (opcode 00b).
			 *     Since we've masked the opcode already, we
			 *     don't need to do anything other than
			 *     inverting the new immediate field.
			 */
			imm = ~imm;
		}
	}

	/* Update the instruction with the new encoding. */
	insn = aarch64_insn_encode_immediate(AARCH64_INSN_IMM_16, insn, imm);
	*place = cpu_to_le32(insn);

	if (imm > U16_MAX)
		return -ERANGE;

	return 0;
}

static int reloc_insn_imm(enum aarch64_reloc_op op, __le32 *place, u64 val,
			  int lsb, int len, enum aarch64_insn_imm_type imm_type)
{
	u64 imm, imm_mask;
	s64 sval;
	u32 insn = le32_to_cpu(*place);

	/* Calculate the relocation value. */
	sval = do_reloc(op, place, val);
	sval >>= lsb;

	/* Extract the value bits and shift them to bit 0. */
	imm_mask = (BIT(lsb + len) - 1) >> lsb;
	imm = sval & imm_mask;

	/* Update the instruction's immediate field. */
	insn = aarch64_insn_encode_immediate(imm_type, insn, imm);
	*place = cpu_to_le32(insn);

	/*
	 * Extract the upper value bits (including the sign bit) and
	 * shift them to bit 0.
	 */
	sval = (s64)(sval & ~(imm_mask >> 1)) >> (len - 1);

	/*
	 * Overflow has occurred if the upper bits are not all equal to
	 * the sign bit of the value.
	 */
	if ((u64)(sval + 1) >= 2)
		return -ERANGE;

	return 0;
}

static int reloc_insn_adrp(struct module *mod, Elf64_Shdr *sechdrs,
			   __le32 *place, u64 val)
{
	u32 insn;

	if (!is_forbidden_offset_for_adrp(place))
		return reloc_insn_imm(RELOC_OP_PAGE, place, val, 12, 21,
				      AARCH64_INSN_IMM_ADR);

	/* patch ADRP to ADR if it is in range */
	if (!reloc_insn_imm(RELOC_OP_PREL, place, val & ~0xfff, 0, 21,
			    AARCH64_INSN_IMM_ADR)) {
		insn = le32_to_cpu(*place);
		insn &= ~BIT(31);
	} else {
		/* out of range for ADR -> emit a veneer */
		val = module_emit_veneer_for_adrp(mod, sechdrs, place, val & ~0xfff);
		if (!val)
			return -ENOEXEC;
		insn = aarch64_insn_gen_branch_imm((u64)place, val,
						   AARCH64_INSN_BRANCH_NOLINK);
	}

	*place = cpu_to_le32(insn);
	return 0;
}

int apply_relocate_add(Elf64_Shdr *sechdrs,
		       const char *strtab,
		       unsigned int symindex,
		       unsigned int relsec,
		       struct module *me)
{
	unsigned int i;
	int ovf;
	bool overflow_check;
	Elf64_Sym *sym;
	void *loc;
	u64 val;
	Elf64_Rela *rel = (void *)sechdrs[relsec].sh_addr;

	for (i = 0; i < sechdrs[relsec].sh_size / sizeof(*rel); i++) {
		/* loc corresponds to P in the AArch64 ELF document. */
		loc = (void *)sechdrs[sechdrs[relsec].sh_info].sh_addr
			+ rel[i].r_offset;

		/* sym is the ELF symbol we're referring to. */
		sym = (Elf64_Sym *)sechdrs[symindex].sh_addr
			+ ELF64_R_SYM(rel[i].r_info);

		/* val corresponds to (S + A) in the AArch64 ELF document. */
		val = sym->st_value + rel[i].r_addend;

		/* Check for overflow by default. */
		overflow_check = true;

		/* Perform the static relocation. */
		switch (ELF64_R_TYPE(rel[i].r_info)) {
		/* Null relocations. */
		case R_ARM_NONE:
		case R_AARCH64_NONE:
			ovf = 0;
			break;

		/* Data relocations. */
		case R_AARCH64_ABS64:
			overflow_check = false;
			ovf = reloc_data(RELOC_OP_ABS, loc, val, 64);
			break;
		case R_AARCH64_ABS32:
			ovf = reloc_data(RELOC_OP_ABS, loc, val, 32);
			break;
		case R_AARCH64_ABS16:
			ovf = reloc_data(RELOC_OP_ABS, loc, val, 16);
			break;
		case R_AARCH64_PREL64:
			overflow_check = false;
			ovf = reloc_data(RELOC_OP_PREL, loc, val, 64);
			break;
		case R_AARCH64_PREL32:
			ovf = reloc_data(RELOC_OP_PREL, loc, val, 32);
			break;
		case R_AARCH64_PREL16:
			ovf = reloc_data(RELOC_OP_PREL, loc, val, 16);
			break;

		/* MOVW instruction relocations. */
		case R_AARCH64_MOVW_UABS_G0_NC:
			overflow_check = false;
			fallthrough;
		case R_AARCH64_MOVW_UABS_G0:
			ovf = reloc_insn_movw(RELOC_OP_ABS, loc, val, 0,
					      AARCH64_INSN_IMM_MOVKZ);
			break;
		case R_AARCH64_MOVW_UABS_G1_NC:
			overflow_check = false;
			fallthrough;
		case R_AARCH64_MOVW_UABS_G1:
			ovf = reloc_insn_movw(RELOC_OP_ABS, loc, val, 16,
					      AARCH64_INSN_IMM_MOVKZ);
			break;
		case R_AARCH64_MOVW_UABS_G2_NC:
			overflow_check = false;
			fallthrough;
		case R_AARCH64_MOVW_UABS_G2:
			ovf = reloc_insn_movw(RELOC_OP_ABS, loc, val, 32,
					      AARCH64_INSN_IMM_MOVKZ);
			break;
		case R_AARCH64_MOVW_UABS_G3:
			/* We're using the top bits so we can't overflow. */
			overflow_check = false;
			ovf = reloc_insn_movw(RELOC_OP_ABS, loc, val, 48,
					      AARCH64_INSN_IMM_MOVKZ);
			break;
		case R_AARCH64_MOVW_SABS_G0:
			ovf = reloc_insn_movw(RELOC_OP_ABS, loc, val, 0,
					      AARCH64_INSN_IMM_MOVNZ);
			break;
		case R_AARCH64_MOVW_SABS_G1:
			ovf = reloc_insn_movw(RELOC_OP_ABS, loc, val, 16,
					      AARCH64_INSN_IMM_MOVNZ);
			break;
		case R_AARCH64_MOVW_SABS_G2:
			ovf = reloc_insn_movw(RELOC_OP_ABS, loc, val, 32,
					      AARCH64_INSN_IMM_MOVNZ);
			break;
		case R_AARCH64_MOVW_PREL_G0_NC:
			overflow_check = false;
			ovf = reloc_insn_movw(RELOC_OP_PREL, loc, val, 0,
					      AARCH64_INSN_IMM_MOVKZ);
			break;
		case R_AARCH64_MOVW_PREL_G0:
			ovf = reloc_insn_movw(RELOC_OP_PREL, loc, val, 0,
					      AARCH64_INSN_IMM_MOVNZ);
			break;
		case R_AARCH64_MOVW_PREL_G1_NC:
			overflow_check = false;
			ovf = reloc_insn_movw(RELOC_OP_PREL, loc, val, 16,
					      AARCH64_INSN_IMM_MOVKZ);
			break;
		case R_AARCH64_MOVW_PREL_G1:
			ovf = reloc_insn_movw(RELOC_OP_PREL, loc, val, 16,
					      AARCH64_INSN_IMM_MOVNZ);
			break;
		case R_AARCH64_MOVW_PREL_G2_NC:
			overflow_check = false;
			ovf = reloc_insn_movw(RELOC_OP_PREL, loc, val, 32,
					      AARCH64_INSN_IMM_MOVKZ);
			break;
		case R_AARCH64_MOVW_PREL_G2:
			ovf = reloc_insn_movw(RELOC_OP_PREL, loc, val, 32,
					      AARCH64_INSN_IMM_MOVNZ);
			break;
		case R_AARCH64_MOVW_PREL_G3:
			/* We're using the top bits so we can't overflow. */
			overflow_check = false;
			ovf = reloc_insn_movw(RELOC_OP_PREL, loc, val, 48,
					      AARCH64_INSN_IMM_MOVNZ);
			break;

		/* Immediate instruction relocations. */
		case R_AARCH64_LD_PREL_LO19:
			ovf = reloc_insn_imm(RELOC_OP_PREL, loc, val, 2, 19,
					     AARCH64_INSN_IMM_19);
			break;
		case R_AARCH64_ADR_PREL_LO21:
			ovf = reloc_insn_imm(RELOC_OP_PREL, loc, val, 0, 21,
					     AARCH64_INSN_IMM_ADR);
			break;
		case R_AARCH64_ADR_PREL_PG_HI21_NC:
			overflow_check = false;
			fallthrough;
		case R_AARCH64_ADR_PREL_PG_HI21:
			ovf = reloc_insn_adrp(me, sechdrs, loc, val);
			if (ovf && ovf != -ERANGE)
				return ovf;
			break;
		case R_AARCH64_ADD_ABS_LO12_NC:
		case R_AARCH64_LDST8_ABS_LO12_NC:
			overflow_check = false;
			ovf = reloc_insn_imm(RELOC_OP_ABS, loc, val, 0, 12,
					     AARCH64_INSN_IMM_12);
			break;
		case R_AARCH64_LDST16_ABS_LO12_NC:
			overflow_check = false;
			ovf = reloc_insn_imm(RELOC_OP_ABS, loc, val, 1, 11,
					     AARCH64_INSN_IMM_12);
			break;
		case R_AARCH64_LDST32_ABS_LO12_NC:
			overflow_check = false;
			ovf = reloc_insn_imm(RELOC_OP_ABS, loc, val, 2, 10,
					     AARCH64_INSN_IMM_12);
			break;
		case R_AARCH64_LDST64_ABS_LO12_NC:
			overflow_check = false;
			ovf = reloc_insn_imm(RELOC_OP_ABS, loc, val, 3, 9,
					     AARCH64_INSN_IMM_12);
			break;
		case R_AARCH64_LDST128_ABS_LO12_NC:
			overflow_check = false;
			ovf = reloc_insn_imm(RELOC_OP_ABS, loc, val, 4, 8,
					     AARCH64_INSN_IMM_12);
			break;
		case R_AARCH64_TSTBR14:
			ovf = reloc_insn_imm(RELOC_OP_PREL, loc, val, 2, 14,
					     AARCH64_INSN_IMM_14);
			break;
		case R_AARCH64_CONDBR19:
			ovf = reloc_insn_imm(RELOC_OP_PREL, loc, val, 2, 19,
					     AARCH64_INSN_IMM_19);
			break;
		case R_AARCH64_JUMP26:
		case R_AARCH64_CALL26:
			ovf = reloc_insn_imm(RELOC_OP_PREL, loc, val, 2, 26,
					     AARCH64_INSN_IMM_26);
			if (ovf == -ERANGE) {
				val = module_emit_plt_entry(me, sechdrs, loc, &rel[i], sym);
				if (!val)
					return -ENOEXEC;
				ovf = reloc_insn_imm(RELOC_OP_PREL, loc, val, 2,
						     26, AARCH64_INSN_IMM_26);
			}
			break;

		default:
			pr_err("module %s: unsupported RELA relocation: %llu\n",
			       me->name, ELF64_R_TYPE(rel[i].r_info));
			return -ENOEXEC;
		}

		if (overflow_check && ovf == -ERANGE)
			goto overflow;

	}

	return 0;

overflow:
	pr_err("module %s: overflow in relocation type %d val %Lx\n",
	       me->name, (int)ELF64_R_TYPE(rel[i].r_info), val);
	return -ENOEXEC;
}

static inline void __init_plt(struct plt_entry *plt, unsigned long addr)
{
	*plt = get_plt_entry(addr, plt);
}

static int module_init_ftrace_plt(const Elf_Ehdr *hdr,
				  const Elf_Shdr *sechdrs,
				  struct module *mod)
{
#if defined(CONFIG_DYNAMIC_FTRACE)
	const Elf_Shdr *s;
	struct plt_entry *plts;

	s = find_section(hdr, sechdrs, ".text.ftrace_trampoline");
	if (!s)
		return -ENOEXEC;

	plts = (void *)s->sh_addr;

	__init_plt(&plts[FTRACE_PLT_IDX], FTRACE_ADDR);

	mod->arch.ftrace_trampolines = plts;
#endif
	return 0;
}

static int module_init_hyp(const Elf_Ehdr *hdr, const Elf_Shdr *sechdrs,
			   struct module *mod)
{
#ifdef CONFIG_KVM
	struct pkvm_el2_module *hyp_mod = &mod->arch.hyp;
	const Elf_Shdr *s;

	/*
	 * If the .hyp.text is missing or empty, this is not a hypervisor
	 * module so ignore the rest of it.
	 */
	s = find_section(hdr, sechdrs, ".hyp.text");
	if (!s || !s->sh_size)
		return 0;

	hyp_mod->text = (struct pkvm_module_section) {
		.start	= (void *)s->sh_addr,
		.end	= (void *)s->sh_addr + s->sh_size,
	};

	s = find_section(hdr, sechdrs, ".hyp.reloc");
	if (!s)
		return -ENOEXEC;

	mod->arch.hyp.relocs = (void *)s->sh_addr;
	mod->arch.hyp.nr_relocs = s->sh_size / sizeof(*mod->arch.hyp.relocs);

	s = find_section(hdr, sechdrs, ".hyp.bss");
	if (s && s->sh_size) {
		mod->arch.hyp.bss = (struct pkvm_module_section) {
			.start	= (void *)s->sh_addr,
			.end	= (void *)s->sh_addr + s->sh_size,
		};
	}

	s = find_section(hdr, sechdrs, ".hyp.rodata");
	if (s && s->sh_size) {
		mod->arch.hyp.rodata = (struct pkvm_module_section) {
			.start	= (void *)s->sh_addr,
			.end	= (void *)s->sh_addr + s->sh_size,
		};
	}

	s = find_section(hdr, sechdrs, ".hyp.data");
	if (s && s->sh_size) {
		mod->arch.hyp.data = (struct pkvm_module_section) {
			.start	= (void *)s->sh_addr,
			.end	= (void *)s->sh_addr + s->sh_size,
		};
	}

	s = find_section(hdr, sechdrs, "_hyp_events");
	if (s) {
		hyp_mod->hyp_events = (void *)s->sh_addr;
		hyp_mod->nr_hyp_events = s->sh_size /
			sizeof(*hyp_mod->hyp_events);

		s = find_section(hdr, sechdrs, ".hyp.event_ids");
		if (s) {
			mod->arch.hyp.event_ids = (struct pkvm_module_section) {
				.start	= (void *)s->sh_addr,
				.end	= (void *)s->sh_addr + s->sh_size,
			};
		} else {
			hyp_mod->hyp_events = NULL;
			hyp_mod->nr_hyp_events = 0;
			WARN(1, "%s: Did you forget define_events.h in the EL2 (hyp) code?",
				mod->name);
		}
	} else {
		s = find_section(hdr, sechdrs, ".hyp.event_ids");
		WARN(s && s->sh_size,
		     "%s: Did you forget kvm_define_hypevents.h in the EL1 code?",
		     mod->name);
	}
#endif
	return 0;
}

int module_finalize(const Elf_Ehdr *hdr,
		    const Elf_Shdr *sechdrs,
		    struct module *me)
{
	int err;
	const Elf_Shdr *s;

	s = find_section(hdr, sechdrs, ".altinstructions");
	if (s)
		apply_alternatives_module((void *)s->sh_addr, s->sh_size);

	if (scs_is_dynamic()) {
		s = find_section(hdr, sechdrs, ".init.eh_frame");
		if (s)
			scs_patch((void *)s->sh_addr, s->sh_size);
	}

	err = module_init_ftrace_plt(hdr, sechdrs, me);
	if (err)
		return err;

	return module_init_hyp(hdr, sechdrs, me);
}
