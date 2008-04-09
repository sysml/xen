/*
 * svm.c: handling SVM architecture-related VM exits
 * Copyright (c) 2004, Intel Corporation.
 * Copyright (c) 2005-2007, Advanced Micro Devices, Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 59 Temple
 * Place - Suite 330, Boston, MA 02111-1307 USA.
 */

#include <xen/config.h>
#include <xen/init.h>
#include <xen/lib.h>
#include <xen/trace.h>
#include <xen/sched.h>
#include <xen/irq.h>
#include <xen/softirq.h>
#include <xen/hypercall.h>
#include <xen/domain_page.h>
#include <asm/current.h>
#include <asm/io.h>
#include <asm/paging.h>
#include <asm/p2m.h>
#include <asm/regs.h>
#include <asm/cpufeature.h>
#include <asm/processor.h>
#include <asm/types.h>
#include <asm/debugreg.h>
#include <asm/msr.h>
#include <asm/spinlock.h>
#include <asm/hvm/hvm.h>
#include <asm/hvm/support.h>
#include <asm/hvm/io.h>
#include <asm/hvm/svm/asid.h>
#include <asm/hvm/svm/svm.h>
#include <asm/hvm/svm/vmcb.h>
#include <asm/hvm/svm/emulate.h>
#include <asm/hvm/svm/intr.h>
#include <asm/x86_emulate.h>
#include <public/sched.h>
#include <asm/hvm/vpt.h>
#include <asm/hvm/trace.h>
#include <asm/hap.h>

u32 svm_feature_flags;

#define set_segment_register(name, value)  \
    asm volatile ( "movw %%ax ,%%" STR(name) "" : : "a" (value) )

enum handler_return { HNDL_done, HNDL_unhandled, HNDL_exception_raised };

asmlinkage void do_IRQ(struct cpu_user_regs *);

static void svm_update_guest_cr(struct vcpu *v, unsigned int cr);
static void svm_update_guest_efer(struct vcpu *v);
static void svm_inject_exception(
    unsigned int trapnr, int errcode, unsigned long cr2);
static void svm_cpuid_intercept(
    unsigned int *eax, unsigned int *ebx,
    unsigned int *ecx, unsigned int *edx);
static void svm_wbinvd_intercept(void);
static void svm_fpu_dirty_intercept(void);
static int svm_msr_read_intercept(struct cpu_user_regs *regs);
static int svm_msr_write_intercept(struct cpu_user_regs *regs);
static void svm_invlpg_intercept(unsigned long vaddr);

/* va of hardware host save area     */
static void *hsa[NR_CPUS] __read_mostly;

/* vmcb used for extended host state */
static void *root_vmcb[NR_CPUS] __read_mostly;

static void inline __update_guest_eip(
    struct cpu_user_regs *regs, unsigned int inst_len)
{
    struct vcpu *curr = current;

    if ( unlikely((inst_len == 0) || (inst_len > 15)) )
    {
        gdprintk(XENLOG_ERR, "Bad instruction length %u\n", inst_len);
        domain_crash(curr->domain);
        return;
    }

    ASSERT(regs == guest_cpu_user_regs());

    regs->eip += inst_len;
    regs->eflags &= ~X86_EFLAGS_RF;

    curr->arch.hvm_svm.vmcb->interrupt_shadow = 0;

    if ( regs->eflags & X86_EFLAGS_TF )
        svm_inject_exception(TRAP_debug, HVM_DELIVER_NO_ERROR_CODE, 0);
}

static void svm_cpu_down(void)
{
    write_efer(read_efer() & ~EFER_SVME);
}

static enum handler_return long_mode_do_msr_write(struct cpu_user_regs *regs)
{
    u64 msr_content = (u32)regs->eax | ((u64)regs->edx << 32);
    u32 ecx = regs->ecx;

    HVM_DBG_LOG(DBG_LEVEL_0, "msr %x msr_content %"PRIx64,
                ecx, msr_content);

    switch ( ecx )
    {
    case MSR_EFER:
        if ( hvm_set_efer(msr_content) )
            return HNDL_exception_raised;
        break;

    case MSR_IA32_MC4_MISC: /* Threshold register */
    case MSR_F10_MC4_MISC1 ... MSR_F10_MC4_MISC3:
        /*
         * MCA/MCE: Threshold register is reported to be locked, so we ignore
         * all write accesses. This behaviour matches real HW, so guests should
         * have no problem with this.
         */
        break;

    default:
        return HNDL_unhandled;
    }

    return HNDL_done;
}

static void svm_save_dr(struct vcpu *v)
{
    struct vmcb_struct *vmcb = v->arch.hvm_svm.vmcb;

    if ( !v->arch.hvm_vcpu.flag_dr_dirty )
        return;

    /* Clear the DR dirty flag and re-enable intercepts for DR accesses. */
    v->arch.hvm_vcpu.flag_dr_dirty = 0;
    v->arch.hvm_svm.vmcb->dr_intercepts = ~0u;

    v->arch.guest_context.debugreg[0] = read_debugreg(0);
    v->arch.guest_context.debugreg[1] = read_debugreg(1);
    v->arch.guest_context.debugreg[2] = read_debugreg(2);
    v->arch.guest_context.debugreg[3] = read_debugreg(3);
    v->arch.guest_context.debugreg[6] = vmcb->dr6;
    v->arch.guest_context.debugreg[7] = vmcb->dr7;
}

static void __restore_debug_registers(struct vcpu *v)
{
    struct vmcb_struct *vmcb = v->arch.hvm_svm.vmcb;

    if ( v->arch.hvm_vcpu.flag_dr_dirty )
        return;

    v->arch.hvm_vcpu.flag_dr_dirty = 1;
    vmcb->dr_intercepts = 0;

    write_debugreg(0, v->arch.guest_context.debugreg[0]);
    write_debugreg(1, v->arch.guest_context.debugreg[1]);
    write_debugreg(2, v->arch.guest_context.debugreg[2]);
    write_debugreg(3, v->arch.guest_context.debugreg[3]);
    vmcb->dr6 = v->arch.guest_context.debugreg[6];
    vmcb->dr7 = v->arch.guest_context.debugreg[7];
}

/*
 * DR7 is saved and restored on every vmexit.  Other debug registers only
 * need to be restored if their value is going to affect execution -- i.e.,
 * if one of the breakpoints is enabled.  So mask out all bits that don't
 * enable some breakpoint functionality.
 */
static void svm_restore_dr(struct vcpu *v)
{
    if ( unlikely(v->arch.guest_context.debugreg[7] & DR7_ACTIVE_MASK) )
        __restore_debug_registers(v);
}

static int svm_vmcb_save(struct vcpu *v, struct hvm_hw_cpu *c)
{
    struct vmcb_struct *vmcb = v->arch.hvm_svm.vmcb;

    c->cr0 = v->arch.hvm_vcpu.guest_cr[0];
    c->cr2 = v->arch.hvm_vcpu.guest_cr[2];
    c->cr3 = v->arch.hvm_vcpu.guest_cr[3];
    c->cr4 = v->arch.hvm_vcpu.guest_cr[4];

    c->sysenter_cs = vmcb->sysenter_cs;
    c->sysenter_esp = vmcb->sysenter_esp;
    c->sysenter_eip = vmcb->sysenter_eip;

    c->pending_event = 0;
    c->error_code = 0;
    if ( vmcb->eventinj.fields.v &&
         hvm_event_needs_reinjection(vmcb->eventinj.fields.type,
                                     vmcb->eventinj.fields.vector) )
    {
        c->pending_event = (uint32_t)vmcb->eventinj.bytes;
        c->error_code = vmcb->eventinj.fields.errorcode;
    }

    return 1;
}

static int svm_vmcb_restore(struct vcpu *v, struct hvm_hw_cpu *c)
{
    unsigned long mfn = 0;
    p2m_type_t p2mt;
    struct vmcb_struct *vmcb = v->arch.hvm_svm.vmcb;

    if ( c->pending_valid &&
         ((c->pending_type == 1) || (c->pending_type > 6) ||
          (c->pending_reserved != 0)) )
    {
        gdprintk(XENLOG_ERR, "Invalid pending event 0x%"PRIx32".\n",
                 c->pending_event);
        return -EINVAL;
    }

    if ( !paging_mode_hap(v->domain) )
    {
        if ( c->cr0 & X86_CR0_PG )
        {
            mfn = mfn_x(gfn_to_mfn(v->domain, c->cr3 >> PAGE_SHIFT, &p2mt));
            if ( !p2m_is_ram(p2mt) || !get_page(mfn_to_page(mfn), v->domain) )
            {
                gdprintk(XENLOG_ERR, "Invalid CR3 value=0x%"PRIx64"\n",
                         c->cr3);
                return -EINVAL;
            }
        }

        if ( v->arch.hvm_vcpu.guest_cr[0] & X86_CR0_PG )
            put_page(pagetable_get_page(v->arch.guest_table));

        v->arch.guest_table = pagetable_from_pfn(mfn);
    }

    v->arch.hvm_vcpu.guest_cr[0] = c->cr0 | X86_CR0_ET;
    v->arch.hvm_vcpu.guest_cr[2] = c->cr2;
    v->arch.hvm_vcpu.guest_cr[3] = c->cr3;
    v->arch.hvm_vcpu.guest_cr[4] = c->cr4;
    svm_update_guest_cr(v, 0);
    svm_update_guest_cr(v, 2);
    svm_update_guest_cr(v, 4);

    vmcb->sysenter_cs =  c->sysenter_cs;
    vmcb->sysenter_esp = c->sysenter_esp;
    vmcb->sysenter_eip = c->sysenter_eip;

    if ( paging_mode_hap(v->domain) )
    {
        vmcb->np_enable = 1;
        vmcb->g_pat = 0x0007040600070406ULL; /* guest PAT */
        vmcb->h_cr3 = pagetable_get_paddr(v->domain->arch.phys_table);
    }

    if ( c->pending_valid ) 
    {
        gdprintk(XENLOG_INFO, "Re-injecting 0x%"PRIx32", 0x%"PRIx32"\n",
                 c->pending_event, c->error_code);

        if ( hvm_event_needs_reinjection(c->pending_type, c->pending_vector) )
        {
            vmcb->eventinj.bytes = c->pending_event;
            vmcb->eventinj.fields.errorcode = c->error_code;
        }
    }

    paging_update_paging_modes(v);

    return 0;
}

        
static void svm_save_cpu_state(struct vcpu *v, struct hvm_hw_cpu *data)
{
    struct vmcb_struct *vmcb = v->arch.hvm_svm.vmcb;

    data->shadow_gs        = vmcb->kerngsbase;
    data->msr_lstar        = vmcb->lstar;
    data->msr_star         = vmcb->star;
    data->msr_cstar        = vmcb->cstar;
    data->msr_syscall_mask = vmcb->sfmask;
    data->msr_efer         = v->arch.hvm_vcpu.guest_efer;
    data->msr_flags        = -1ULL;

    data->tsc = hvm_get_guest_time(v);
}


static void svm_load_cpu_state(struct vcpu *v, struct hvm_hw_cpu *data)
{
    struct vmcb_struct *vmcb = v->arch.hvm_svm.vmcb;

    vmcb->kerngsbase = data->shadow_gs;
    vmcb->lstar      = data->msr_lstar;
    vmcb->star       = data->msr_star;
    vmcb->cstar      = data->msr_cstar;
    vmcb->sfmask     = data->msr_syscall_mask;
    v->arch.hvm_vcpu.guest_efer = data->msr_efer;
    svm_update_guest_efer(v);

    hvm_set_guest_time(v, data->tsc);
}

static void svm_save_vmcb_ctxt(struct vcpu *v, struct hvm_hw_cpu *ctxt)
{
    svm_save_cpu_state(v, ctxt);
    svm_vmcb_save(v, ctxt);
}

static int svm_load_vmcb_ctxt(struct vcpu *v, struct hvm_hw_cpu *ctxt)
{
    svm_load_cpu_state(v, ctxt);
    if (svm_vmcb_restore(v, ctxt)) {
        printk("svm_vmcb restore failed!\n");
        domain_crash(v->domain);
        return -EINVAL;
    }

    return 0;
}

static void svm_fpu_enter(struct vcpu *v)
{
    struct vmcb_struct *vmcb = v->arch.hvm_svm.vmcb;

    setup_fpu(v);
    vmcb->exception_intercepts &= ~(1U << TRAP_no_device);
}

static void svm_fpu_leave(struct vcpu *v)
{
    struct vmcb_struct *vmcb = v->arch.hvm_svm.vmcb;

    ASSERT(!v->fpu_dirtied);
    ASSERT(read_cr0() & X86_CR0_TS);

    /*
     * If the guest does not have TS enabled then we must cause and handle an 
     * exception on first use of the FPU. If the guest *does* have TS enabled 
     * then this is not necessary: no FPU activity can occur until the guest 
     * clears CR0.TS, and we will initialise the FPU when that happens.
     */
    if ( !(v->arch.hvm_vcpu.guest_cr[0] & X86_CR0_TS) )
    {
        v->arch.hvm_svm.vmcb->exception_intercepts |= 1U << TRAP_no_device;
        vmcb->cr0 |= X86_CR0_TS;
    }
}

static unsigned int svm_get_interrupt_shadow(struct vcpu *v)
{
    struct vmcb_struct *vmcb = v->arch.hvm_svm.vmcb;
    return (vmcb->interrupt_shadow ?
            (HVM_INTR_SHADOW_MOV_SS|HVM_INTR_SHADOW_STI) : 0);
}

static void svm_set_interrupt_shadow(struct vcpu *v, unsigned int intr_shadow)
{
    struct vmcb_struct *vmcb = v->arch.hvm_svm.vmcb;
    vmcb->interrupt_shadow =
        !!(intr_shadow & (HVM_INTR_SHADOW_MOV_SS|HVM_INTR_SHADOW_STI));
}

static int svm_guest_x86_mode(struct vcpu *v)
{
    struct vmcb_struct *vmcb = v->arch.hvm_svm.vmcb;

    if ( unlikely(!(v->arch.hvm_vcpu.guest_cr[0] & X86_CR0_PE)) )
        return 0;
    if ( unlikely(guest_cpu_user_regs()->eflags & X86_EFLAGS_VM) )
        return 1;
    if ( hvm_long_mode_enabled(v) && likely(vmcb->cs.attr.fields.l) )
        return 8;
    return (likely(vmcb->cs.attr.fields.db) ? 4 : 2);
}

static void svm_update_host_cr3(struct vcpu *v)
{
    /* SVM doesn't have a HOST_CR3 equivalent to update. */
}

static void svm_update_guest_cr(struct vcpu *v, unsigned int cr)
{
    struct vmcb_struct *vmcb = v->arch.hvm_svm.vmcb;

    switch ( cr )
    {
    case 0: {
        unsigned long hw_cr0_mask = 0;

        if ( !(v->arch.hvm_vcpu.guest_cr[0] & X86_CR0_TS) )
        {
            if ( v != current )
                hw_cr0_mask |= X86_CR0_TS;
            else if ( vmcb->cr0 & X86_CR0_TS )
                svm_fpu_enter(v);
        }

        vmcb->cr0 = v->arch.hvm_vcpu.guest_cr[0] | hw_cr0_mask;
        if ( !paging_mode_hap(v->domain) )
            vmcb->cr0 |= X86_CR0_PG | X86_CR0_WP;
        break;
    }
    case 2:
        vmcb->cr2 = v->arch.hvm_vcpu.guest_cr[2];
        break;
    case 3:
        vmcb->cr3 = v->arch.hvm_vcpu.hw_cr[3];
        svm_asid_inv_asid(v);
        break;
    case 4:
        vmcb->cr4 = HVM_CR4_HOST_MASK;
        if ( paging_mode_hap(v->domain) )
            vmcb->cr4 &= ~X86_CR4_PAE;
        vmcb->cr4 |= v->arch.hvm_vcpu.guest_cr[4];
        break;
    default:
        BUG();
    }
}

static void svm_update_guest_efer(struct vcpu *v)
{
    struct vmcb_struct *vmcb = v->arch.hvm_svm.vmcb;

    vmcb->efer = (v->arch.hvm_vcpu.guest_efer | EFER_SVME) & ~EFER_LME;
    if ( vmcb->efer & EFER_LMA )
        vmcb->efer |= EFER_LME;
}

static void svm_flush_guest_tlbs(void)
{
    /* Roll over the CPU's ASID generation, so it gets a clean TLB when we
     * next VMRUN.  (If ASIDs are disabled, the whole TLB is flushed on
     * VMRUN anyway). */
    svm_asid_inc_generation();
}

static void svm_sync_vmcb(struct vcpu *v)
{
    struct arch_svm_struct *arch_svm = &v->arch.hvm_svm;

    if ( arch_svm->vmcb_in_sync )
        return;

    arch_svm->vmcb_in_sync = 1;

    svm_vmsave(arch_svm->vmcb);
}

static void svm_get_segment_register(struct vcpu *v, enum x86_segment seg,
                                     struct segment_register *reg)
{
    struct vmcb_struct *vmcb = v->arch.hvm_svm.vmcb;

    ASSERT(v == current);

    switch ( seg )
    {
    case x86_seg_cs:
        memcpy(reg, &vmcb->cs, sizeof(*reg));
        break;
    case x86_seg_ds:
        memcpy(reg, &vmcb->ds, sizeof(*reg));
        break;
    case x86_seg_es:
        memcpy(reg, &vmcb->es, sizeof(*reg));
        break;
    case x86_seg_fs:
        svm_sync_vmcb(v);
        memcpy(reg, &vmcb->fs, sizeof(*reg));
        break;
    case x86_seg_gs:
        svm_sync_vmcb(v);
        memcpy(reg, &vmcb->gs, sizeof(*reg));
        break;
    case x86_seg_ss:
        memcpy(reg, &vmcb->ss, sizeof(*reg));
        reg->attr.fields.dpl = vmcb->cpl;
        break;
    case x86_seg_tr:
        svm_sync_vmcb(v);
        memcpy(reg, &vmcb->tr, sizeof(*reg));
        break;
    case x86_seg_gdtr:
        memcpy(reg, &vmcb->gdtr, sizeof(*reg));
        break;
    case x86_seg_idtr:
        memcpy(reg, &vmcb->idtr, sizeof(*reg));
        break;
    case x86_seg_ldtr:
        svm_sync_vmcb(v);
        memcpy(reg, &vmcb->ldtr, sizeof(*reg));
        break;
    default:
        BUG();
    }
}

static void svm_set_segment_register(struct vcpu *v, enum x86_segment seg,
                                     struct segment_register *reg)
{
    struct vmcb_struct *vmcb = v->arch.hvm_svm.vmcb;
    int sync = 0;

    ASSERT((v == current) || !vcpu_runnable(v));

    switch ( seg )
    {
    case x86_seg_fs:
    case x86_seg_gs:
    case x86_seg_tr:
    case x86_seg_ldtr:
        sync = (v == current);
        break;
    default:
        break;
    }

    if ( sync )
        svm_sync_vmcb(v);

    switch ( seg )
    {
    case x86_seg_cs:
        memcpy(&vmcb->cs, reg, sizeof(*reg));
        break;
    case x86_seg_ds:
        memcpy(&vmcb->ds, reg, sizeof(*reg));
        break;
    case x86_seg_es:
        memcpy(&vmcb->es, reg, sizeof(*reg));
        break;
    case x86_seg_fs:
        memcpy(&vmcb->fs, reg, sizeof(*reg));
        break;
    case x86_seg_gs:
        memcpy(&vmcb->gs, reg, sizeof(*reg));
        break;
    case x86_seg_ss:
        memcpy(&vmcb->ss, reg, sizeof(*reg));
        vmcb->cpl = vmcb->ss.attr.fields.dpl;
        break;
    case x86_seg_tr:
        memcpy(&vmcb->tr, reg, sizeof(*reg));
        break;
    case x86_seg_gdtr:
        memcpy(&vmcb->gdtr, reg, sizeof(*reg));
        break;
    case x86_seg_idtr:
        memcpy(&vmcb->idtr, reg, sizeof(*reg));
        break;
    case x86_seg_ldtr:
        memcpy(&vmcb->ldtr, reg, sizeof(*reg));
        break;
    default:
        BUG();
    }

    if ( sync )
        svm_vmload(vmcb);
}

static void svm_set_tsc_offset(struct vcpu *v, u64 offset)
{
    v->arch.hvm_svm.vmcb->tsc_offset = offset;
}

static void svm_init_hypercall_page(struct domain *d, void *hypercall_page)
{
    char *p;
    int i;

    for ( i = 0; i < (PAGE_SIZE / 32); i++ )
    {
        p = (char *)(hypercall_page + (i * 32));
        *(u8  *)(p + 0) = 0xb8; /* mov imm32, %eax */
        *(u32 *)(p + 1) = i;
        *(u8  *)(p + 5) = 0x0f; /* vmmcall */
        *(u8  *)(p + 6) = 0x01;
        *(u8  *)(p + 7) = 0xd9;
        *(u8  *)(p + 8) = 0xc3; /* ret */
    }

    /* Don't support HYPERVISOR_iret at the moment */
    *(u16 *)(hypercall_page + (__HYPERVISOR_iret * 32)) = 0x0b0f; /* ud2 */
}

static void svm_ctxt_switch_from(struct vcpu *v)
{
    int cpu = smp_processor_id();

    svm_fpu_leave(v);

    svm_save_dr(v);

    svm_sync_vmcb(v);
    svm_vmload(root_vmcb[cpu]);

#ifdef __x86_64__
    /* Resume use of ISTs now that the host TR is reinstated. */
    idt_tables[cpu][TRAP_double_fault].a  |= IST_DF << 32;
    idt_tables[cpu][TRAP_nmi].a           |= IST_NMI << 32;
    idt_tables[cpu][TRAP_machine_check].a |= IST_MCE << 32;
#endif
}

static void svm_ctxt_switch_to(struct vcpu *v)
{
    int cpu = smp_processor_id();

#ifdef  __x86_64__
    /* 
     * This is required, because VMRUN does consistency check
     * and some of the DOM0 selectors are pointing to 
     * invalid GDT locations, and cause AMD processors
     * to shutdown.
     */
    set_segment_register(ds, 0);
    set_segment_register(es, 0);
    set_segment_register(ss, 0);

    /*
     * Cannot use ISTs for NMI/#MC/#DF while we are running with the guest TR.
     * But this doesn't matter: the IST is only req'd to handle SYSCALL/SYSRET.
     */
    idt_tables[cpu][TRAP_double_fault].a  &= ~(7UL << 32);
    idt_tables[cpu][TRAP_nmi].a           &= ~(7UL << 32);
    idt_tables[cpu][TRAP_machine_check].a &= ~(7UL << 32);
#endif

    svm_restore_dr(v);

    svm_vmsave(root_vmcb[cpu]);
    svm_vmload(v->arch.hvm_svm.vmcb);
}

static void svm_do_resume(struct vcpu *v) 
{
    bool_t debug_state = v->domain->debugger_attached;

    if ( unlikely(v->arch.hvm_vcpu.debug_state_latch != debug_state) )
    {
        uint32_t mask = (1U << TRAP_debug) | (1U << TRAP_int3);
        v->arch.hvm_vcpu.debug_state_latch = debug_state;
        if ( debug_state )
            v->arch.hvm_svm.vmcb->exception_intercepts |= mask;
        else
            v->arch.hvm_svm.vmcb->exception_intercepts &= ~mask;
    }

    if ( v->arch.hvm_svm.launch_core != smp_processor_id() )
    {
        v->arch.hvm_svm.launch_core = smp_processor_id();
        hvm_migrate_timers(v);

        /* Migrating to another ASID domain.  Request a new ASID. */
        svm_asid_init_vcpu(v);
    }

    /* Reflect the vlapic's TPR in the hardware vtpr */
    v->arch.hvm_svm.vmcb->vintr.fields.tpr = 
        (vlapic_get_reg(vcpu_vlapic(v), APIC_TASKPRI) & 0xFF) >> 4;

    hvm_do_resume(v);
    reset_stack_and_jump(svm_asm_do_resume);
}

static int svm_domain_initialise(struct domain *d)
{
    return 0;
}

static void svm_domain_destroy(struct domain *d)
{
}

static int svm_vcpu_initialise(struct vcpu *v)
{
    int rc;

    v->arch.schedule_tail    = svm_do_resume;
    v->arch.ctxt_switch_from = svm_ctxt_switch_from;
    v->arch.ctxt_switch_to   = svm_ctxt_switch_to;

    v->arch.hvm_svm.launch_core = -1;

    if ( (rc = svm_create_vmcb(v)) != 0 )
    {
        dprintk(XENLOG_WARNING,
                "Failed to create VMCB for vcpu %d: err=%d.\n",
                v->vcpu_id, rc);
        return rc;
    }

    return 0;
}

static void svm_vcpu_destroy(struct vcpu *v)
{
    svm_destroy_vmcb(v);
}

static void svm_inject_exception(
    unsigned int trapnr, int errcode, unsigned long cr2)
{
    struct vcpu *curr = current;
    struct vmcb_struct *vmcb = curr->arch.hvm_svm.vmcb;
    eventinj_t event = vmcb->eventinj;

    if ( unlikely(event.fields.v) &&
         (event.fields.type == X86_EVENTTYPE_HW_EXCEPTION) )
    {
        trapnr = hvm_combine_hw_exceptions(event.fields.vector, trapnr);
        if ( trapnr == TRAP_double_fault )
            errcode = 0;
    }

    event.bytes = 0;
    event.fields.v = 1;
    event.fields.type = X86_EVENTTYPE_HW_EXCEPTION;
    event.fields.vector = trapnr;
    event.fields.ev = (errcode != HVM_DELIVER_NO_ERROR_CODE);
    event.fields.errorcode = errcode;

    vmcb->eventinj = event;

    if ( trapnr == TRAP_page_fault )
    {
        vmcb->cr2 = curr->arch.hvm_vcpu.guest_cr[2] = cr2;
        HVMTRACE_2D(PF_INJECT, curr, curr->arch.hvm_vcpu.guest_cr[2], errcode);
    }
    else
    {
        HVMTRACE_2D(INJ_EXC, curr, trapnr, errcode);
    }

    if ( (trapnr == TRAP_debug) &&
         (guest_cpu_user_regs()->eflags & X86_EFLAGS_TF) )
    {
        __restore_debug_registers(curr);
        vmcb->dr6 |= 0x4000;
    }
}

static int svm_event_pending(struct vcpu *v)
{
    struct vmcb_struct *vmcb = v->arch.hvm_svm.vmcb;
    return vmcb->eventinj.fields.v;
}

static int svm_do_pmu_interrupt(struct cpu_user_regs *regs)
{
    return 0;
}

static struct hvm_function_table svm_function_table = {
    .name                 = "SVM",
    .cpu_down             = svm_cpu_down,
    .domain_initialise    = svm_domain_initialise,
    .domain_destroy       = svm_domain_destroy,
    .vcpu_initialise      = svm_vcpu_initialise,
    .vcpu_destroy         = svm_vcpu_destroy,
    .save_cpu_ctxt        = svm_save_vmcb_ctxt,
    .load_cpu_ctxt        = svm_load_vmcb_ctxt,
    .get_interrupt_shadow = svm_get_interrupt_shadow,
    .set_interrupt_shadow = svm_set_interrupt_shadow,
    .guest_x86_mode       = svm_guest_x86_mode,
    .get_segment_register = svm_get_segment_register,
    .set_segment_register = svm_set_segment_register,
    .update_host_cr3      = svm_update_host_cr3,
    .update_guest_cr      = svm_update_guest_cr,
    .update_guest_efer    = svm_update_guest_efer,
    .flush_guest_tlbs     = svm_flush_guest_tlbs,
    .set_tsc_offset       = svm_set_tsc_offset,
    .inject_exception     = svm_inject_exception,
    .init_hypercall_page  = svm_init_hypercall_page,
    .event_pending        = svm_event_pending,
    .do_pmu_interrupt     = svm_do_pmu_interrupt,
    .cpuid_intercept      = svm_cpuid_intercept,
    .wbinvd_intercept     = svm_wbinvd_intercept,
    .fpu_dirty_intercept  = svm_fpu_dirty_intercept,
    .msr_read_intercept   = svm_msr_read_intercept,
    .msr_write_intercept  = svm_msr_write_intercept,
    .invlpg_intercept     = svm_invlpg_intercept
};

int start_svm(struct cpuinfo_x86 *c)
{
    u32 eax, ecx, edx;
    u32 phys_hsa_lo, phys_hsa_hi;   
    u64 phys_hsa;
    int cpu = smp_processor_id();
 
    /* Xen does not fill x86_capability words except 0. */
    ecx = cpuid_ecx(0x80000001);
    boot_cpu_data.x86_capability[5] = ecx;
    
    if ( !(test_bit(X86_FEATURE_SVME, &boot_cpu_data.x86_capability)) )
        return 0;

    /* Check whether SVM feature is disabled in BIOS */
    rdmsr(MSR_K8_VM_CR, eax, edx);
    if ( eax & K8_VMCR_SVME_DISABLE )
    {
        printk("AMD SVM Extension is disabled in BIOS.\n");
        return 0;
    }

    if ( ((hsa[cpu] = alloc_host_save_area()) == NULL) ||
         ((root_vmcb[cpu] = alloc_vmcb()) == NULL) )
        return 0;

    write_efer(read_efer() | EFER_SVME);

    /* Initialize the HSA for this core. */
    phys_hsa = (u64) virt_to_maddr(hsa[cpu]);
    phys_hsa_lo = (u32) phys_hsa;
    phys_hsa_hi = (u32) (phys_hsa >> 32);    
    wrmsr(MSR_K8_VM_HSAVE_PA, phys_hsa_lo, phys_hsa_hi);

    /* Initialize core's ASID handling. */
    svm_asid_init(c);

    if ( cpu != 0 )
        return 1;

    setup_vmcb_dump();

    svm_feature_flags = ((cpuid_eax(0x80000000) >= 0x8000000A) ?
                         cpuid_edx(0x8000000A) : 0);

    svm_function_table.hap_supported = cpu_has_svm_npt;

    hvm_enable(&svm_function_table);

    return 1;
}

static void svm_do_nested_pgfault(paddr_t gpa, struct cpu_user_regs *regs)
{
    p2m_type_t p2mt;
    mfn_t mfn;
    unsigned long gfn = gpa >> PAGE_SHIFT;

    /* If this GFN is emulated MMIO, pass the fault to the mmio handler */
    mfn = gfn_to_mfn_current(gfn, &p2mt);
    if ( p2mt == p2m_mmio_dm )
    {
        if ( !handle_mmio() )
            hvm_inject_exception(TRAP_gp_fault, 0, 0);
        return;
    }

    /* Log-dirty: mark the page dirty and let the guest write it again */
    paging_mark_dirty(current->domain, mfn_x(mfn));
    p2m_change_type(current->domain, gfn, p2m_ram_logdirty, p2m_ram_rw);
}

static void svm_fpu_dirty_intercept(void)
{
    struct vcpu *curr = current;
    struct vmcb_struct *vmcb = curr->arch.hvm_svm.vmcb;

    svm_fpu_enter(curr);

    if ( !(curr->arch.hvm_vcpu.guest_cr[0] & X86_CR0_TS) )
        vmcb->cr0 &= ~X86_CR0_TS;
}

#define bitmaskof(idx)  (1U << ((idx) & 31))
static void svm_cpuid_intercept(
    unsigned int *eax, unsigned int *ebx,
    unsigned int *ecx, unsigned int *edx)
{
    unsigned int input = *eax;
    struct vcpu *v = current;

    hvm_cpuid(input, eax, ebx, ecx, edx);

    switch ( input )
    {
    case 0x00000001:
        /* Mask Intel-only features. */
        *ecx &= ~(bitmaskof(X86_FEATURE_SSSE3) |
                  bitmaskof(X86_FEATURE_SSE4_1) |
                  bitmaskof(X86_FEATURE_SSE4_2));
        break;

    case 0x80000001:
        /* Filter features which are shared with 0x00000001:EDX. */
        if ( vlapic_hw_disabled(vcpu_vlapic(v)) )
            __clear_bit(X86_FEATURE_APIC & 31, edx);
#if CONFIG_PAGING_LEVELS >= 3
        if ( !v->domain->arch.hvm_domain.params[HVM_PARAM_PAE_ENABLED] )
#endif
            __clear_bit(X86_FEATURE_PAE & 31, edx);
        __clear_bit(X86_FEATURE_PSE36 & 31, edx);

        /* Filter all other features according to a whitelist. */
        *ecx &= (bitmaskof(X86_FEATURE_LAHF_LM) |
                 bitmaskof(X86_FEATURE_ALTMOVCR) |
                 bitmaskof(X86_FEATURE_ABM) |
                 bitmaskof(X86_FEATURE_SSE4A) |
                 bitmaskof(X86_FEATURE_MISALIGNSSE) |
                 bitmaskof(X86_FEATURE_3DNOWPF));
        *edx &= (0x0183f3ff | /* features shared with 0x00000001:EDX */
                 bitmaskof(X86_FEATURE_NX) |
                 bitmaskof(X86_FEATURE_LM) |
                 bitmaskof(X86_FEATURE_SYSCALL) |
                 bitmaskof(X86_FEATURE_MP) |
                 bitmaskof(X86_FEATURE_MMXEXT) |
                 bitmaskof(X86_FEATURE_FFXSR));
        break;

    case 0x80000007:
    case 0x8000000A:
        /* Mask out features of power management and SVM extension. */
        *eax = *ebx = *ecx = *edx = 0;
        break;

    case 0x80000008:
        /* Make sure Number of CPU core is 1 when HTT=0 */
        *ecx &= 0xFFFFFF00;
        break;
    }

    HVMTRACE_3D(CPUID, v, input,
                ((uint64_t)*eax << 32) | *ebx, ((uint64_t)*ecx << 32) | *edx);
}

static void svm_vmexit_do_cpuid(struct cpu_user_regs *regs)
{
    unsigned int eax, ebx, ecx, edx, inst_len;

    inst_len = __get_instruction_length(current, INSTR_CPUID, NULL);
    if ( inst_len == 0 ) 
        return;

    eax = regs->eax;
    ebx = regs->ebx;
    ecx = regs->ecx;
    edx = regs->edx;

    svm_cpuid_intercept(&eax, &ebx, &ecx, &edx);

    regs->eax = eax;
    regs->ebx = ebx;
    regs->ecx = ecx;
    regs->edx = edx;

    __update_guest_eip(regs, inst_len);
}

static void svm_dr_access(struct vcpu *v, struct cpu_user_regs *regs)
{
    HVMTRACE_0D(DR_WRITE, v);
    __restore_debug_registers(v);
}

static int svm_msr_read_intercept(struct cpu_user_regs *regs)
{
    u64 msr_content = 0;
    u32 ecx = regs->ecx, eax, edx;
    struct vcpu *v = current;
    struct vmcb_struct *vmcb = v->arch.hvm_svm.vmcb;

    switch ( ecx )
    {
    case MSR_IA32_TSC:
        msr_content = hvm_get_guest_time(v);
        break;

    case MSR_IA32_APICBASE:
        msr_content = vcpu_vlapic(v)->hw.apic_base_msr;
        break;

    case MSR_EFER:
        msr_content = v->arch.hvm_vcpu.guest_efer;
        break;

    case MSR_IA32_MC4_MISC: /* Threshold register */
    case MSR_F10_MC4_MISC1 ... MSR_F10_MC4_MISC3:
        /*
         * MCA/MCE: We report that the threshold register is unavailable
         * for OS use (locked by the BIOS).
         */
        msr_content = 1ULL << 61; /* MC4_MISC.Locked */
        break;

    case MSR_IA32_EBC_FREQUENCY_ID:
        /*
         * This Intel-only register may be accessed if this HVM guest
         * has been migrated from an Intel host. The value zero is not
         * particularly meaningful, but at least avoids the guest crashing!
         */
        msr_content = 0;
        break;

    case MSR_K8_VM_HSAVE_PA:
        goto gpf;

    case MSR_IA32_MCG_CAP:
    case MSR_IA32_MCG_STATUS:
    case MSR_IA32_MC0_STATUS:
    case MSR_IA32_MC1_STATUS:
    case MSR_IA32_MC2_STATUS:
    case MSR_IA32_MC3_STATUS:
    case MSR_IA32_MC4_STATUS:
    case MSR_IA32_MC5_STATUS:
        /* No point in letting the guest see real MCEs */
        msr_content = 0;
        break;

    case MSR_IA32_DEBUGCTLMSR:
        msr_content = vmcb->debugctlmsr;
        break;

    case MSR_IA32_LASTBRANCHFROMIP:
        msr_content = vmcb->lastbranchfromip;
        break;

    case MSR_IA32_LASTBRANCHTOIP:
        msr_content = vmcb->lastbranchtoip;
        break;

    case MSR_IA32_LASTINTFROMIP:
        msr_content = vmcb->lastintfromip;
        break;

    case MSR_IA32_LASTINTTOIP:
        msr_content = vmcb->lastinttoip;
        break;

    default:
        if ( rdmsr_hypervisor_regs(ecx, &eax, &edx) ||
             rdmsr_safe(ecx, eax, edx) == 0 )
        {
            regs->eax = eax;
            regs->edx = edx;
            goto done;
        }
        goto gpf;
    }
    regs->eax = msr_content & 0xFFFFFFFF;
    regs->edx = msr_content >> 32;

 done:
    hvmtrace_msr_read(v, ecx, msr_content);
    HVM_DBG_LOG(DBG_LEVEL_1, "returns: ecx=%x, eax=%lx, edx=%lx",
                ecx, (unsigned long)regs->eax, (unsigned long)regs->edx);
    return X86EMUL_OKAY;

 gpf:
    svm_inject_exception(TRAP_gp_fault, 0, 0);
    return X86EMUL_EXCEPTION;
}

static int svm_msr_write_intercept(struct cpu_user_regs *regs)
{
    u64 msr_content = 0;
    u32 ecx = regs->ecx;
    struct vcpu *v = current;
    struct vmcb_struct *vmcb = v->arch.hvm_svm.vmcb;

    msr_content = (u32)regs->eax | ((u64)regs->edx << 32);

    hvmtrace_msr_write(v, ecx, msr_content);

    switch ( ecx )
    {
    case MSR_IA32_TSC:
        hvm_set_guest_time(v, msr_content);
        pt_reset(v);
        break;

    case MSR_IA32_APICBASE:
        vlapic_msr_set(vcpu_vlapic(v), msr_content);
        break;

    case MSR_K8_VM_HSAVE_PA:
        goto gpf;

    case MSR_IA32_DEBUGCTLMSR:
        vmcb->debugctlmsr = msr_content;
        if ( !msr_content || !cpu_has_svm_lbrv )
            break;
        vmcb->lbr_control.fields.enable = 1;
        svm_disable_intercept_for_msr(v, MSR_IA32_DEBUGCTLMSR);
        svm_disable_intercept_for_msr(v, MSR_IA32_LASTBRANCHFROMIP);
        svm_disable_intercept_for_msr(v, MSR_IA32_LASTBRANCHTOIP);
        svm_disable_intercept_for_msr(v, MSR_IA32_LASTINTFROMIP);
        svm_disable_intercept_for_msr(v, MSR_IA32_LASTINTTOIP);
        break;

    case MSR_IA32_LASTBRANCHFROMIP:
        vmcb->lastbranchfromip = msr_content;
        break;

    case MSR_IA32_LASTBRANCHTOIP:
        vmcb->lastbranchtoip = msr_content;
        break;

    case MSR_IA32_LASTINTFROMIP:
        vmcb->lastintfromip = msr_content;
        break;

    case MSR_IA32_LASTINTTOIP:
        vmcb->lastinttoip = msr_content;
        break;

    default:
        switch ( long_mode_do_msr_write(regs) )
        {
        case HNDL_unhandled:
            wrmsr_hypervisor_regs(ecx, regs->eax, regs->edx);
            break;
        case HNDL_exception_raised:
            return X86EMUL_EXCEPTION;
        case HNDL_done:
            break;
        }
        break;
    }

    return X86EMUL_OKAY;

 gpf:
    svm_inject_exception(TRAP_gp_fault, 0, 0);
    return X86EMUL_EXCEPTION;
}

static void svm_do_msr_access(struct cpu_user_regs *regs)
{
    int rc, inst_len;
    struct vcpu *v = current;
    struct vmcb_struct *vmcb = v->arch.hvm_svm.vmcb;

    if ( vmcb->exitinfo1 == 0 )
    {
        rc = svm_msr_read_intercept(regs);
        inst_len = __get_instruction_length(v, INSTR_RDMSR, NULL);
    }
    else
    {
        rc = svm_msr_write_intercept(regs);
        inst_len = __get_instruction_length(v, INSTR_WRMSR, NULL);
    }

    if ( rc == X86EMUL_OKAY )
        __update_guest_eip(regs, inst_len);
}

static void svm_vmexit_do_hlt(struct vmcb_struct *vmcb,
                              struct cpu_user_regs *regs)
{
    struct vcpu *curr = current;
    struct hvm_intack intack = hvm_vcpu_has_pending_irq(curr);
    unsigned int inst_len;

    inst_len = __get_instruction_length(curr, INSTR_HLT, NULL);
    if ( inst_len == 0 )
        return;
    __update_guest_eip(regs, inst_len);

    /* Check for pending exception or new interrupt. */
    if ( vmcb->eventinj.fields.v ||
         ((intack.source != hvm_intsrc_none) &&
          !hvm_interrupt_blocked(current, intack)) )
    {
        HVMTRACE_1D(HLT, curr, /*int pending=*/ 1);
        return;
    }

    HVMTRACE_1D(HLT, curr, /*int pending=*/ 0);
    hvm_hlt(regs->eflags);
}

static void wbinvd_ipi(void *info)
{
    wbinvd();
}

static void svm_wbinvd_intercept(void)
{
    if ( !list_empty(&(domain_hvm_iommu(current->domain)->pdev_list)) )
        on_each_cpu(wbinvd_ipi, NULL, 1, 1);
}

static void svm_vmexit_do_invalidate_cache(struct cpu_user_regs *regs)
{
    enum instruction_index list[] = { INSTR_INVD, INSTR_WBINVD };
    int inst_len;

    svm_wbinvd_intercept();

    inst_len = __get_instruction_length_from_list(
        current, list, ARRAY_SIZE(list), NULL, NULL);
    __update_guest_eip(regs, inst_len);
}

static void svm_invlpg_intercept(unsigned long vaddr)
{
    struct vcpu *curr = current;
    HVMTRACE_2D(INVLPG, curr, 0, vaddr);
    paging_invlpg(curr, vaddr);
    svm_asid_g_invlpg(curr, vaddr);
}

asmlinkage void svm_vmexit_handler(struct cpu_user_regs *regs)
{
    unsigned int exit_reason;
    struct vcpu *v = current;
    struct vmcb_struct *vmcb = v->arch.hvm_svm.vmcb;
    eventinj_t eventinj;
    int inst_len, rc;

    /*
     * Before doing anything else, we need to sync up the VLAPIC's TPR with
     * SVM's vTPR. It's OK if the guest doesn't touch CR8 (e.g. 32-bit Windows)
     * because we update the vTPR on MMIO writes to the TPR.
     */
    vlapic_set_reg(vcpu_vlapic(v), APIC_TASKPRI,
                   (vmcb->vintr.fields.tpr & 0x0F) << 4);

    exit_reason = vmcb->exitcode;

    hvmtrace_vmexit(v, regs->eip, exit_reason);

    if ( unlikely(exit_reason == VMEXIT_INVALID) )
    {
        svm_dump_vmcb(__func__, vmcb);
        goto exit_and_crash;
    }

    perfc_incra(svmexits, exit_reason);

    hvm_maybe_deassert_evtchn_irq();

    /* Event delivery caused this intercept? Queue for redelivery. */
    eventinj = vmcb->exitintinfo;
    if ( unlikely(eventinj.fields.v) &&
         hvm_event_needs_reinjection(eventinj.fields.type,
                                     eventinj.fields.vector) )
        vmcb->eventinj = eventinj;

    switch ( exit_reason )
    {
    case VMEXIT_INTR:
        /* Asynchronous event, handled when we STGI'd after the VMEXIT. */
        HVMTRACE_0D(INTR, v);
        break;

    case VMEXIT_NMI:
        /* Asynchronous event, handled when we STGI'd after the VMEXIT. */
        HVMTRACE_0D(NMI, v);
        break;

    case VMEXIT_SMI:
        /* Asynchronous event, handled when we STGI'd after the VMEXIT. */
        HVMTRACE_0D(SMI, v);
        break;

    case VMEXIT_EXCEPTION_DB:
        if ( !v->domain->debugger_attached )
            goto exit_and_crash;
        domain_pause_for_debugger();
        break;

    case VMEXIT_EXCEPTION_BP:
        if ( !v->domain->debugger_attached )
            goto exit_and_crash;
        /* AMD Vol2, 15.11: INT3, INTO, BOUND intercepts do not update RIP. */
        inst_len = __get_instruction_length(v, INSTR_INT3, NULL);
        __update_guest_eip(regs, inst_len);
        domain_pause_for_debugger();
        break;

    case VMEXIT_EXCEPTION_NM:
        svm_fpu_dirty_intercept();
        break;  

    case VMEXIT_EXCEPTION_PF: {
        unsigned long va;
        va = vmcb->exitinfo2;
        regs->error_code = vmcb->exitinfo1;
        HVM_DBG_LOG(DBG_LEVEL_VMMU,
                    "eax=%lx, ebx=%lx, ecx=%lx, edx=%lx, esi=%lx, edi=%lx",
                    (unsigned long)regs->eax, (unsigned long)regs->ebx,
                    (unsigned long)regs->ecx, (unsigned long)regs->edx,
                    (unsigned long)regs->esi, (unsigned long)regs->edi);

        if ( paging_fault(va, regs) )
        {
            HVMTRACE_2D(PF_XEN, v, va, regs->error_code);
            break;
        }

        svm_inject_exception(TRAP_page_fault, regs->error_code, va);
        break;
    }

    /* Asynchronous event, handled when we STGI'd after the VMEXIT. */
    case VMEXIT_EXCEPTION_MC:
        HVMTRACE_0D(MCE, v);
        break;

    case VMEXIT_VINTR:
        vmcb->vintr.fields.irq = 0;
        vmcb->general1_intercepts &= ~GENERAL1_INTERCEPT_VINTR;
        break;

    case VMEXIT_INVD:
    case VMEXIT_WBINVD:
        svm_vmexit_do_invalidate_cache(regs);
        break;

    case VMEXIT_TASK_SWITCH: {
        enum hvm_task_switch_reason reason;
        int32_t errcode = -1;
        if ( (vmcb->exitinfo2 >> 36) & 1 )
            reason = TSW_iret;
        else if ( (vmcb->exitinfo2 >> 38) & 1 )
            reason = TSW_jmp;
        else
            reason = TSW_call_or_int;
        if ( (vmcb->exitinfo2 >> 44) & 1 )
            errcode = (uint32_t)vmcb->exitinfo2;
        hvm_task_switch((uint16_t)vmcb->exitinfo1, reason, errcode);
        break;
    }

    case VMEXIT_CPUID:
        svm_vmexit_do_cpuid(regs);
        break;

    case VMEXIT_HLT:
        svm_vmexit_do_hlt(vmcb, regs);
        break;

    case VMEXIT_CR0_READ ... VMEXIT_CR15_READ:
    case VMEXIT_CR0_WRITE ... VMEXIT_CR15_WRITE:
    case VMEXIT_INVLPG:
    case VMEXIT_INVLPGA:
    case VMEXIT_IOIO:
        if ( !handle_mmio() )
            hvm_inject_exception(TRAP_gp_fault, 0, 0);
        break;

    case VMEXIT_VMMCALL:
        inst_len = __get_instruction_length(v, INSTR_VMCALL, NULL);
        if ( inst_len == 0 )
            break;
        HVMTRACE_1D(VMMCALL, v, regs->eax);
        rc = hvm_do_hypercall(regs);
        if ( rc != HVM_HCALL_preempted )
        {
            __update_guest_eip(regs, inst_len);
            if ( rc == HVM_HCALL_invalidate )
                send_invalidate_req();
        }
        break;

    case VMEXIT_DR0_READ ... VMEXIT_DR7_READ:
    case VMEXIT_DR0_WRITE ... VMEXIT_DR7_WRITE:
        svm_dr_access(v, regs);
        break;

    case VMEXIT_MSR:
        svm_do_msr_access(regs);
        break;

    case VMEXIT_SHUTDOWN:
        hvm_triple_fault();
        break;

    case VMEXIT_RDTSCP:
    case VMEXIT_MONITOR:
    case VMEXIT_MWAIT:
    case VMEXIT_VMRUN:
    case VMEXIT_VMLOAD:
    case VMEXIT_VMSAVE:
    case VMEXIT_STGI:
    case VMEXIT_CLGI:
    case VMEXIT_SKINIT:
        svm_inject_exception(TRAP_invalid_op, HVM_DELIVER_NO_ERROR_CODE, 0);
        break;

    case VMEXIT_NPF:
        perfc_incra(svmexits, VMEXIT_NPF_PERFC);
        regs->error_code = vmcb->exitinfo1;
        svm_do_nested_pgfault(vmcb->exitinfo2, regs);
        break;

    default:
    exit_and_crash:
        gdprintk(XENLOG_ERR, "unexpected VMEXIT: exit reason = 0x%x, "
                 "exitinfo1 = %"PRIx64", exitinfo2 = %"PRIx64"\n",
                 exit_reason, 
                 (u64)vmcb->exitinfo1, (u64)vmcb->exitinfo2);
        domain_crash(v->domain);
        break;
    }

    /* The exit may have updated the TPR: reflect this in the hardware vtpr */
    vmcb->vintr.fields.tpr = 
        (vlapic_get_reg(vcpu_vlapic(v), APIC_TASKPRI) & 0xFF) >> 4;
}

asmlinkage void svm_trace_vmentry(void)
{
    hvmtrace_vmentry(current);
}
  
/*
 * Local variables:
 * mode: C
 * c-set-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
