/*
 * PROJECT:         ReactOS HAL
 * LICENSE:         BSD - See COPYING.ARM in the top level directory
 * FILE:            hal/halx86/generic/pic.c
 * PURPOSE:         HAL PIC Management and Control Code
 * PROGRAMMERS:     ReactOS Portable Systems Group
 */

/* INCLUDES *******************************************************************/

#include <hal.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

/*
 * This table basically keeps track of level vs edge triggered interrupts.
 * Windows has 250+ entries, but it seems stupid to replicate that since the PIC
 * can't actually have that many.
 *
 * When a level interrupt is registered, the respective pointer in this table is
 * modified to point to a dimiss routine for level interrupts instead.
 *
 * The other thing this table does is special case IRQ7, IRQ13 and IRQ15:
 *
 * - If an IRQ line is deasserted before it is acknowledged due to a noise spike
 *   generated by an expansion device (since the IRQ line is low during the 1st
 *   acknowledge bus cycle), the i8259 will keep the line low for at least 100ns
 *   When the spike passes, a pull-up resistor will return the IRQ line to high.
 *   Since the PIC requires the input be high until the first acknowledge, the
 *   i8259 knows that this was a spurious interrupt, and on the second interrupt
 *   acknowledge cycle, it reports this to the CPU. Since no valid interrupt has
 *   actually happened Intel hardcoded the chip to report IRQ7 on the master PIC
 *   and IRQ15 on the slave PIC (IR7 either way).
 *
 *   "ISA System Architecture", 3rd Edition, states that these cases should be
 *   handled by reading the respective Interrupt Service Request (ISR) bits from
 *   the affected PIC, and validate whether or not IR7 is set. If it isn't, then
 *   the interrupt is spurious and should be ignored.
 *
 *   Note that for a spurious IRQ15, we DO have to send an EOI to the master for
 *   IRQ2 since the line was asserted by the slave when it received the spurious
 *   IRQ15!
 *
 * - When the 80287/80387 math co-processor generates an FPU/NPX trap, this is 
 *   connected to IRQ13, so we have to clear the busy latch on the NPX port.
 */
PHAL_DISMISS_INTERRUPT HalpSpecialDismissTable[16] =
{
    HalpDismissIrqGeneric,
    HalpDismissIrqGeneric,
    HalpDismissIrqGeneric,
    HalpDismissIrqGeneric,
    HalpDismissIrqGeneric,
    HalpDismissIrqGeneric,
    HalpDismissIrqGeneric,
    HalpDismissIrq07,
    HalpDismissIrqGeneric,
    HalpDismissIrqGeneric,
    HalpDismissIrqGeneric,
    HalpDismissIrqGeneric,
    HalpDismissIrqGeneric,
    HalpDismissIrq13,
    HalpDismissIrqGeneric,
    HalpDismissIrq15
};

/* This table contains the static x86 PIC mapping between IRQLs and IRQs */
ULONG KiI8259MaskTable[32] =
{
#ifdef __GNUC__
#if __GNUC__ * 100 + __GNUC_MINOR__ >= 404
    /*
     * It Device IRQLs only start at 4 or higher, so these are just software
     * IRQLs that don't really change anything on the hardware
     */
    0b00000000000000000000000000000000, /* IRQL 0 */
    0b00000000000000000000000000000000, /* IRQL 1 */
    0b00000000000000000000000000000000, /* IRQL 2 */
    0b00000000000000000000000000000000, /* IRQL 3 */
    
    /*
     * These next IRQLs are actually useless from the PIC perspective, because
     * with only 2 PICs, the mask you can send them is only 8 bits each, for 16
     * bits total, so these IRQLs are masking off a phantom PIC.
     */
    0b11111111100000000000000000000000, /* IRQL 4 */
    0b11111111110000000000000000000000, /* IRQL 5 */
    0b11111111111000000000000000000000, /* IRQL 6 */
    0b11111111111100000000000000000000, /* IRQL 7 */
    0b11111111111110000000000000000000, /* IRQL 8 */
    0b11111111111111000000000000000000, /* IRQL 9 */
    0b11111111111111100000000000000000, /* IRQL 10 */
    0b11111111111111110000000000000000, /* IRQL 11 */
    
    /*
     * Okay, now we're finally starting to mask off IRQs on the slave PIC, from
     * IRQ15 to IRQ8. This means the higher-level IRQs get less priority in the
     * IRQL sense.
     */
    0b11111111111111111000000000000000, /* IRQL 12 */
    0b11111111111111111100000000000000, /* IRQL 13 */
    0b11111111111111111110000000000000, /* IRQL 14 */
    0b11111111111111111111000000000000, /* IRQL 15 */
    0b11111111111111111111100000000000, /* IRQL 16 */
    0b11111111111111111111110000000000, /* IRQL 17 */
    0b11111111111111111111111000000000, /* IRQL 18 */
    0b11111111111111111111111000000000, /* IRQL 19 */
    
    /*
     * Now we mask off the IRQs on the master. Notice the 0 "droplet"? You might
     * have also seen that IRQL 18 and 19 are essentially equal as far as the
     * PIC is concerned. That bit is actually IRQ8, which happens to be the RTC.
     * The RTC will keep firing as long as we don't reach PROFILE_LEVEL which
     * actually kills it. The RTC clock (unlike the system clock) is used by the
     * profiling APIs in the HAL, so that explains the logic.
     */
    0b11111111111111111111111010000000, /* IRQL 20 */
    0b11111111111111111111111011000000, /* IRQL 21 */
    0b11111111111111111111111011100000, /* IRQL 22 */
    0b11111111111111111111111011110000, /* IRQL 23 */
    0b11111111111111111111111011111000, /* IRQL 24 */
    0b11111111111111111111111011111000, /* IRQL 25 */
    0b11111111111111111111111011111010, /* IRQL 26 */
    0b11111111111111111111111111111010, /* IRQL 27 */
    
    /*
     * IRQL 24 and 25 are actually identical, so IRQL 28 is actually the last
     * IRQL to modify a bit on the master PIC. It happens to modify the very
     * last of the IRQs, IRQ0, which corresponds to the system clock interval
     * timer that keeps track of time (the Windows heartbeat). We only want to
     * turn this off at a high-enough IRQL, which is why IRQLs 24 and 25 are the
     * same to give this guy a chance to come up higher. Note that IRQL 28 is
     * called CLOCK2_LEVEL, which explains the usage we just explained.
     */
    0b11111111111111111111111111111011, /* IRQL 28 */
    
    /*
     * We have finished off with the PIC so there's nothing left to mask at the
     * level of these IRQLs, making them only logical IRQLs on x86 machines.
     * Note that we have another 0 "droplet" you might've caught since IRQL 26.
     * In this case, it's the 2nd bit that never gets turned off, which is IRQ2,
     * the cascade IRQ that we use to bridge the slave PIC with the master PIC.
     * We never want to turn it off, so no matter the IRQL, it will be set to 0.
     */
    0b11111111111111111111111111111011, /* IRQL 29 */
    0b11111111111111111111111111111011, /* IRQL 30 */
    0b11111111111111111111111111111011  /* IRQL 31 */
#else
    0,                             /* IRQL 0 */
    0,                             /* IRQL 1 */
    0,                             /* IRQL 2 */
    0,                             /* IRQL 3 */
    0xFF800000,                    /* IRQL 4 */
    0xFFC00000,                    /* IRQL 5 */
    0xFFE00000,                    /* IRQL 6 */
    0xFFF00000,                    /* IRQL 7 */
    0xFFF80000,                    /* IRQL 8 */
    0xFFFC0000,                    /* IRQL 9 */
    0xFFFE0000,                    /* IRQL 10 */
    0xFFFF0000,                    /* IRQL 11 */
    0xFFFF8000,                    /* IRQL 12 */
    0xFFFFC000,                    /* IRQL 13 */
    0xFFFFE000,                    /* IRQL 14 */
    0xFFFFF000,                    /* IRQL 15 */
    0xFFFFF800,                    /* IRQL 16 */
    0xFFFFFC00,                    /* IRQL 17 */
    0xFFFFFE00,                    /* IRQL 18 */
    0xFFFFFE00,                    /* IRQL 19 */
    0xFFFFFE80,                    /* IRQL 20 */
    0xFFFFFEC0,                    /* IRQL 21 */
    0xFFFFFEE0,                    /* IRQL 22 */
    0xFFFFFEF0,                    /* IRQL 23 */
    0xFFFFFEF8,                    /* IRQL 24 */
    0xFFFFFEF8,                    /* IRQL 25 */
    0xFFFFFEFA,                    /* IRQL 26 */
    0xFFFFFFFA,                    /* IRQL 27 */
    0xFFFFFFFB,                    /* IRQL 28 */
    0xFFFFFFFB,                    /* IRQL 29 */
    0xFFFFFFFB,                    /* IRQL 30 */
    0xFFFFFFFB                     /* IRQL 31 */
#endif
#endif
};

/* Denotes minimum required IRQL before we can process pending SW interrupts */
KIRQL SWInterruptLookUpTable[8] =
{
    PASSIVE_LEVEL,                 /* IRR 0 */
    PASSIVE_LEVEL,                 /* IRR 1 */
    APC_LEVEL,                     /* IRR 2 */
    APC_LEVEL,                     /* IRR 3 */
    DISPATCH_LEVEL,                /* IRR 4 */
    DISPATCH_LEVEL,                /* IRR 5 */
    DISPATCH_LEVEL,                /* IRR 6 */
    DISPATCH_LEVEL                 /* IRR 7 */
};

/* Handlers for pending software interrupts */
PHAL_SW_INTERRUPT_HANDLER SWInterruptHandlerTable[3] =
{
    KiUnexpectedInterrupt,
    HalpApcInterrupt,
    HalpDispatchInterrupt
};

USHORT HalpEisaELCR;

/* FUNCTIONS ******************************************************************/

VOID
NTAPI
HalpInitializePICs(IN BOOLEAN EnableInterrupts)
{
    ULONG EFlags;
    I8259_ICW1 Icw1;
    I8259_ICW2 Icw2;
    I8259_ICW3 Icw3;
    I8259_ICW4 Icw4;
    EISA_ELCR Elcr;
    ULONG i, j;
    
    /* Save EFlags and disable interrupts */
    EFlags = __readeflags();
    _disable();
    
    /* Initialize ICW1 for master, interval 8, edge-triggered mode with ICW4 */
    Icw1.NeedIcw4 = TRUE;
    Icw1.InterruptMode = EdgeTriggered;
    Icw1.OperatingMode = Cascade;
    Icw1.Interval = Interval8;
    Icw1.Init = TRUE;
    Icw1.InterruptVectorAddress = 0; /* This is only used in MCS80/85 mode */
    __outbyte(PIC1_CONTROL_PORT, Icw1.Bits);
    
    /* Set interrupt vector base */
    Icw2.Bits = PRIMARY_VECTOR_BASE;
    __outbyte(PIC1_DATA_PORT, Icw2.Bits);
    
    /* Connect slave to IRQ 2 */
    Icw3.Bits = 0;
    Icw3.SlaveIrq2 = TRUE;
    __outbyte(PIC1_DATA_PORT, Icw3.Bits);
    
    /* Enable 8086 mode, non-automatic EOI, non-buffered mode, non special fully nested mode */
    Icw4.Reserved = 0;
    Icw4.SystemMode = New8086Mode;
    Icw4.EoiMode = NormalEoi;
    Icw4.BufferedMode = NonBuffered;
    Icw4.SpecialFullyNestedMode = FALSE;
    __outbyte(PIC1_DATA_PORT, Icw4.Bits);
    
    /* Mask all interrupts */
    __outbyte(PIC1_DATA_PORT, 0xFF);
    
    /* Initialize ICW1 for master, interval 8, edge-triggered mode with ICW4 */
    Icw1.NeedIcw4 = TRUE;
    Icw1.InterruptMode = EdgeTriggered;
    Icw1.OperatingMode = Cascade;
    Icw1.Interval = Interval8;
    Icw1.Init = TRUE;
    Icw1.InterruptVectorAddress = 0; /* This is only used in MCS80/85 mode */
    __outbyte(PIC2_CONTROL_PORT, Icw1.Bits);
    
    /* Set interrupt vector base */
    Icw2.Bits = PRIMARY_VECTOR_BASE + 8;
    __outbyte(PIC2_DATA_PORT, Icw2.Bits);
    
    /* Slave ID */
    Icw3.Bits = 0;
    Icw3.SlaveId = 2;
    __outbyte(PIC2_DATA_PORT, Icw3.Bits);
    
    /* Enable 8086 mode, non-automatic EOI, non-buffered mode, non special fully nested mode */
    Icw4.Reserved = 0;
    Icw4.SystemMode = New8086Mode;
    Icw4.EoiMode = NormalEoi;
    Icw4.BufferedMode = NonBuffered;
    Icw4.SpecialFullyNestedMode = FALSE;
    __outbyte(PIC2_DATA_PORT, Icw4.Bits);
    
    /* Mask all interrupts */
    __outbyte(PIC2_DATA_PORT, 0xFF);
    
    /* Read EISA Edge/Level Register for master and slave */
    Elcr.Bits = (__inbyte(EISA_ELCR_SLAVE) << 8) | __inbyte(EISA_ELCR_MASTER);
    
    /* IRQs 0, 1, 2, 8, and 13 are system-reserved and must be edge */
    if (!(Elcr.Master.Irq0Level) && !(Elcr.Master.Irq1Level) && !(Elcr.Master.Irq2Level) &&
        !(Elcr.Slave.Irq8Level) && !(Elcr.Slave.Irq13Level))
    {
        /* ELCR is as it's supposed to be, save it */
        HalpEisaELCR = Elcr.Bits;
        DPRINT1("HAL Detected EISA Interrupt Controller (ELCR: %lx)\n", HalpEisaELCR);
        
        /* Scan for level interrupts */
        for (i = 1, j = 0; j < 16; i <<= 1, j++)
        {
            /* Warn the user ReactOS does not (and has never) supported this */
            if (HalpEisaELCR & i) DPRINT1("WARNING: IRQ %d is SHARED and LEVEL-SENSITIVE. This is unsupported!\n", j);
        }
    }
    
    /* Restore interrupt state */
    if (EnableInterrupts) EFlags |= EFLAGS_INTERRUPT_MASK;
    __writeeflags(EFlags);
}

/* IRQL MANAGEMENT ************************************************************/

/*
 * @implemented
 */
KIRQL
NTAPI
KeGetCurrentIrql(VOID)
{
    /* Return the IRQL */
    return KeGetPcr()->Irql;
}

/*
 * @implemented
 */
KIRQL
NTAPI
KeRaiseIrqlToDpcLevel(VOID)
{
    PKPCR Pcr = KeGetPcr();
    KIRQL CurrentIrql;
    
    /* Save and update IRQL */
    CurrentIrql = Pcr->Irql;
    Pcr->Irql = DISPATCH_LEVEL;
    
#ifdef IRQL_DEBUG
    /* Validate correct raise */
    if (CurrentIrql > DISPATCH_LEVEL)
    {
        /* Crash system */
        KeBugCheckEx(IRQL_NOT_GREATER_OR_EQUAL,
                     CurrentIrql,
                     DISPATCH_LEVEL,
                     0,
                     1);
    }
#endif

    /* Return the previous value */
    return CurrentIrql;
}

/*
 * @implemented
 */
KIRQL
NTAPI
KeRaiseIrqlToSynchLevel(VOID)
{
    PKPCR Pcr = KeGetPcr();
    KIRQL CurrentIrql;
    
    /* Save and update IRQL */
    CurrentIrql = Pcr->Irql;
    Pcr->Irql = SYNCH_LEVEL;
    
#ifdef IRQL_DEBUG
    /* Validate correct raise */
    if (CurrentIrql > SYNCH_LEVEL)
    {
        /* Crash system */
        KeBugCheckEx(IRQL_NOT_GREATER_OR_EQUAL,
                     CurrentIrql,
                     SYNCH_LEVEL,
                     0,
                     1);
    }
#endif

    /* Return the previous value */
    return CurrentIrql;
}

/*
 * @implemented
 */
KIRQL
FASTCALL
KfRaiseIrql(IN KIRQL NewIrql)
{
    PKPCR Pcr = KeGetPcr();
    ULONG EFlags;
    KIRQL CurrentIrql;
    PIC_MASK Mask;

    /* Read current IRQL */
    CurrentIrql = Pcr->Irql;
    
#ifdef IRQL_DEBUG
    /* Validate correct raise */
    if (CurrentIrql > NewIrql)
    {
        /* Crash system */
        Pcr->Irql = PASSIVE_LEVEL;
        KeBugCheckEx(IRQL_NOT_GREATER_OR_EQUAL,
                     CurrentIrql,
                     NewIrql,
                     0,
                     9);
    }
#endif
    
    /* Check if new IRQL affects hardware state */
    if (NewIrql > DISPATCH_LEVEL)
    {
        /* Save current interrupt state and disable interrupts */
        EFlags = __readeflags();
        _disable();
        
        /* Update the IRQL */
        Pcr->Irql = NewIrql;
        
        /* Set new PIC mask */
        Mask.Both = KiI8259MaskTable[NewIrql] | Pcr->IDR;
        __outbyte(PIC1_DATA_PORT, Mask.Master);
        __outbyte(PIC2_DATA_PORT, Mask.Slave);
        
        /* Restore interrupt state */
        __writeeflags(EFlags);
    }
    else
    {
        /* Set new IRQL */
        Pcr->Irql = NewIrql;
    }
    
    /* Return old IRQL */
    return CurrentIrql;
}


/*
 * @implemented
 */
VOID
FASTCALL
KfLowerIrql(IN KIRQL OldIrql)
{
    ULONG EFlags;
    KIRQL PendingIrql;
    PKPCR Pcr = KeGetPcr();
    PIC_MASK Mask;
    
#ifdef IRQL_DEBUG
    /* Validate correct lower */
    if (OldIrql > Pcr->Irql)
    {
        /* Crash system */
        KIRQL CurrentIrql = Pcr->Irql;
        Pcr->Irql = HIGH_LEVEL;
        KeBugCheckEx(IRQL_NOT_LESS_OR_EQUAL,
                     CurrentIrql,
                     OldIrql,
                     0,
                     3);
    }
#endif
    
    /* Save EFlags and disable interrupts */
    EFlags = __readeflags();
    _disable();

    /* Check if currentl IRQL affects hardware state */
    if (Pcr->Irql > DISPATCH_LEVEL)
    {        
        /* Set new PIC mask */
        Mask.Both = KiI8259MaskTable[OldIrql] | Pcr->IDR;
        __outbyte(PIC1_DATA_PORT, Mask.Master);
        __outbyte(PIC2_DATA_PORT, Mask.Slave);
    }

    /* Set old IRQL */
    Pcr->Irql = OldIrql;
    
    /* Check for pending software interrupts and compare with current IRQL */
    PendingIrql = SWInterruptLookUpTable[Pcr->IRR];
    if (PendingIrql > OldIrql) SWInterruptHandlerTable[PendingIrql]();

    /* Restore interrupt state */
    __writeeflags(EFlags);
}

/* SOFTWARE INTERRUPTS ********************************************************/

/*
 * @implemented
 */
VOID
FASTCALL
HalRequestSoftwareInterrupt(IN KIRQL Irql)
{
    ULONG EFlags;
    PKPCR Pcr = KeGetPcr();
    KIRQL PendingIrql;
    
    /* Save EFlags and disable interrupts */
    EFlags = __readeflags();
    _disable();
    
    /* Mask out the requested bit */
    Pcr->IRR |= (1 << Irql);

    /* Check for pending software interrupts and compare with current IRQL */
    PendingIrql = SWInterruptLookUpTable[Pcr->IRR & 3];
    if (PendingIrql > Pcr->Irql) SWInterruptHandlerTable[PendingIrql]();
    
    /* Restore interrupt state */
    __writeeflags(EFlags);
}

/*
 * @implemented
 */
VOID
FASTCALL
HalClearSoftwareInterrupt(IN KIRQL Irql)
{
    /* Mask out the requested bit */
    KeGetPcr()->IRR &= ~(1 << Irql);
}

VOID
NTAPI
HalpEndSoftwareInterrupt(IN KIRQL OldIrql)
{
    KIRQL PendingIrql;
    PKPCR Pcr = KeGetPcr();
    PIC_MASK Mask;

    /* Check if currentl IRQL affects hardware state */
    if (Pcr->Irql > DISPATCH_LEVEL)
    {        
        /* Set new PIC mask */
        Mask.Both = KiI8259MaskTable[OldIrql] | Pcr->IDR;
        __outbyte(PIC1_DATA_PORT, Mask.Master);
        __outbyte(PIC2_DATA_PORT, Mask.Slave);
    }

    /* Set old IRQL */
    Pcr->Irql = OldIrql;
    
    /* Check for pending software interrupts and compare with current IRQL */
    PendingIrql = SWInterruptLookUpTable[Pcr->IRR];

    /* NOTE: We can do better! We need to support "jumping" a frame for nested cases! */
    if (PendingIrql > OldIrql) SWInterruptHandlerTable[PendingIrql]();
}

/* INTERRUPT DISMISSAL FUNCTIONS **********************************************/

BOOLEAN
FORCEINLINE
_HalpDismissIrqGeneric(IN KIRQL Irql,
                       IN ULONG Irq,
                       OUT PKIRQL OldIrql)
{
    PIC_MASK Mask;
    KIRQL CurrentIrql;
    I8259_OCW2 Ocw2;
    PKPCR Pcr = KeGetPcr();

    /* First save current IRQL and compare it to the requested one */
    CurrentIrql = Pcr->Irql;

    /* Set the new IRQL and return the current one */
    Pcr->Irql = Irql;
    *OldIrql = CurrentIrql;

    /* Set new PIC mask */
    Mask.Both = KiI8259MaskTable[Irql] | Pcr->IDR;
    __outbyte(PIC1_DATA_PORT, Mask.Master);
    __outbyte(PIC2_DATA_PORT, Mask.Slave);
    
    /* Prepare OCW2 for EOI */
    Ocw2.Bits = 0;
    Ocw2.EoiMode = SpecificEoi;

    /* Check which PIC needs the EOI */
    if (Irq > 8)
    {
        /* Send the EOI for the IRQ */
        __outbyte(PIC2_CONTROL_PORT, Ocw2.Bits | (Irq - 8));
    
        /* Send the EOI for IRQ2 on the master because this was cascaded */
        __outbyte(PIC1_CONTROL_PORT, Ocw2.Bits | 2);
    }
    else
    {
        /* Send the EOI for the IRQ */
        __outbyte(PIC1_CONTROL_PORT, Ocw2.Bits | Irq);
    }
    
    /* Enable interrupts and return success */
    _enable();
    return TRUE;
}

BOOLEAN
__attribute__((regparm(3)))
HalpDismissIrqGeneric(IN KIRQL Irql,
                      IN ULONG Irq,
                      OUT PKIRQL OldIrql)
{
    /* Run the inline code */
    return _HalpDismissIrqGeneric(Irql, Irq, OldIrql);
}

BOOLEAN
__attribute__((regparm(3)))
HalpDismissIrq15(IN KIRQL Irql,
                 IN ULONG Irq,
                 OUT PKIRQL OldIrql)
{
    I8259_OCW3 Ocw3;
    I8259_OCW2 Ocw2;
    I8259_ISR Isr;
        
    /* Request the ISR */
    Ocw3.Bits = 0;
    Ocw3.Sbo = 1; /* This encodes an OCW3 vs. an OCW2 */
    Ocw3.ReadRequest = ReadIsr;
    __outbyte(PIC2_CONTROL_PORT, Ocw3.Bits);
    
    /* Read the ISR */
    Isr.Bits = __inbyte(PIC2_CONTROL_PORT);
    
    /* Is IRQ15 really active (this is IR7) */
    if (Isr.Irq7 == FALSE)
    {
        /* It isn't, so we have to EOI IRQ2 because this was cascaded */
        Ocw2.Bits = 0;
        Ocw2.EoiMode = SpecificEoi;
        __outbyte(PIC1_CONTROL_PORT, Ocw2.Bits | 2);
        
        /* And now fail since this was spurious */
        return FALSE;
    }

    /* Do normal interrupt dismiss */
    return _HalpDismissIrqGeneric(Irql, Irq, OldIrql);
}


BOOLEAN
__attribute__((regparm(3)))
HalpDismissIrq13(IN KIRQL Irql,
                 IN ULONG Irq,
                 OUT PKIRQL OldIrql)
{
    /* Clear the FPU busy latch */
    __outbyte(0xF0, 0);
    
    /* Do normal interrupt dismiss */
    return _HalpDismissIrqGeneric(Irql, Irq, OldIrql);
}

BOOLEAN
__attribute__((regparm(3)))
HalpDismissIrq07(IN KIRQL Irql,
                 IN ULONG Irq,
                 OUT PKIRQL OldIrql)
{
    I8259_OCW3 Ocw3;
    I8259_ISR Isr;
        
    /* Request the ISR */
    Ocw3.Bits = 0;
    Ocw3.Sbo = 1;
    Ocw3.ReadRequest = ReadIsr;
    __outbyte(PIC1_CONTROL_PORT, Ocw3.Bits);
    
    /* Read the ISR */
    Isr.Bits = __inbyte(PIC1_CONTROL_PORT);
    
    /* Is IRQ 7 really active? If it isn't, this is spurious so fail */
    if (Isr.Irq7 == FALSE) return FALSE;
    
    /* Do normal interrupt dismiss */
    return _HalpDismissIrqGeneric(Irql, Irq, OldIrql);
}

/* SYSTEM INTERRUPTS **********************************************************/

/*
 * @implemented
 */
BOOLEAN
NTAPI
HalEnableSystemInterrupt(IN UCHAR Vector,
                         IN KIRQL Irql,
                         IN KINTERRUPT_MODE InterruptMode)
{
    ULONG Irq;
    PKPCR Pcr = KeGetPcr();
    PIC_MASK PicMask;
    
    /* Validate the IRQ */
    Irq = Vector - PRIMARY_VECTOR_BASE;
    if (Irq >= CLOCK2_LEVEL) return FALSE;
    
#ifdef PCI_IRQ_MP
    /* Check if there is a PCI IRQ Routing Miniport Driver */
    if (HalpIrqMiniportInitialized)
    {
        UNIMPLEMENTED;
        while (TRUE);
    }
#endif
    
    /* Disable interrupts */
    _disable();
    
    /* Update software IDR */
    Pcr->IDR &= ~(1 << Irq);

    /* Set new PIC mask */
    PicMask.Both = KiI8259MaskTable[Pcr->Irql] | Pcr->IDR;
    __outbyte(PIC1_DATA_PORT, PicMask.Master);
    __outbyte(PIC2_DATA_PORT, PicMask.Slave);
    
    /* Enable interrupts and exit */
    _enable();
    return TRUE;
}

/*
 * @implemented
 */
VOID
NTAPI
HalDisableSystemInterrupt(IN UCHAR Vector,
                          IN KIRQL Irql)
{
    ULONG IrqMask;
    PIC_MASK PicMask;

    /* Compute new combined IRQ mask */
    IrqMask = 1 << (Vector - PRIMARY_VECTOR_BASE);
    
    /* Disable interrupts */
    _disable();
    
    /* Update software IDR */
    KeGetPcr()->IDR |= IrqMask;
    
    /* Read current interrupt mask */
    PicMask.Master = __inbyte(PIC1_DATA_PORT);
    PicMask.Slave = __inbyte(PIC2_DATA_PORT);
    
    /* Add the new disabled interrupt */
    PicMask.Both |= IrqMask;
    
    /* Write new interrupt mask */
    __outbyte(PIC1_DATA_PORT, PicMask.Master);
    __outbyte(PIC2_DATA_PORT, PicMask.Slave);
    
    /* Bring interrupts back */
    _enable();
}

/*
 * @implemented
 */
BOOLEAN
NTAPI
HalBeginSystemInterrupt(IN KIRQL Irql,
                        IN UCHAR Vector,
                        OUT PKIRQL OldIrql)
{
    ULONG Irq;
    
    /* Get the IRQ and call the proper routine to handle it */
    Irq = Vector - PRIMARY_VECTOR_BASE;
    return HalpSpecialDismissTable[Irq](Irql, Irq, OldIrql);
}

/*
 * @implemented
 */
VOID
NTAPI
HalEndSystemInterrupt(IN KIRQL OldIrql,
                      IN UCHAR Vector)
{
    KIRQL PendingIrql;
    PKPCR Pcr = KeGetPcr();
    PIC_MASK Mask;

    /* Check if currentl IRQL affects hardware state */
    if (Pcr->Irql > DISPATCH_LEVEL)
    {        
        /* Set new PIC mask */
        Mask.Both = KiI8259MaskTable[OldIrql] | Pcr->IDR;
        __outbyte(PIC1_DATA_PORT, Mask.Master);
        __outbyte(PIC2_DATA_PORT, Mask.Slave);
    }

    /* Set old IRQL */
    Pcr->Irql = OldIrql;
    
    /* Check for pending software interrupts and compare with current IRQL */
    PendingIrql = SWInterruptLookUpTable[Pcr->IRR];
    if (PendingIrql > OldIrql) SWInterruptHandlerTable[PendingIrql]();
}

/* SOFTWARE INTERRUPT TRAPS ***************************************************/

VOID
FASTCALL
HalpApcInterruptHandler(IN PKTRAP_FRAME TrapFrame)
{
    KIRQL CurrentIrql;
    PKPCR Pcr = KeGetPcr();
    
    /* Set up a fake INT Stack */
    TrapFrame->EFlags = __readeflags();
    TrapFrame->SegCs = KGDT_R0_CODE;
    TrapFrame->Eip = TrapFrame->Eax;
    
    /* Build the trap frame */
    KiEnterInterruptTrap(TrapFrame);
    
    /* Save the current IRQL and update it */
    CurrentIrql = Pcr->Irql;
    Pcr->Irql = APC_LEVEL;
    
    /* Remove DPC from IRR */
    Pcr->IRR &= ~(1 << APC_LEVEL);
    
    /* Enable interrupts and call the kernel's APC interrupt handler */
    _enable();
    KiDeliverApc(((KiUserTrap(TrapFrame)) || (TrapFrame->EFlags & EFLAGS_V86_MASK)) ?
                 UserMode : KernelMode,
                 NULL,
                 TrapFrame);

    /* Disable interrupts and end the interrupt */
    _disable();
    HalpEndSoftwareInterrupt(CurrentIrql);
    
    /* Exit the interrupt */
    KiEoiHelper(TrapFrame);
}

VOID
FASTCALL
HalpDispatchInterruptHandler(IN PKTRAP_FRAME TrapFrame)
{
    KIRQL CurrentIrql;
    PKPCR Pcr = KeGetPcr();
    
    /* Set up a fake INT Stack */
    TrapFrame->EFlags = __readeflags();
    TrapFrame->SegCs = KGDT_R0_CODE;
    TrapFrame->Eip = TrapFrame->Eax;
    
    /* Build the trap frame */
    KiEnterInterruptTrap(TrapFrame);
    
    /* Save the current IRQL and update it */
    CurrentIrql = Pcr->Irql;
    Pcr->Irql = DISPATCH_LEVEL;
    
    /* Remove DPC from IRR */
    Pcr->IRR &= ~(1 << DISPATCH_LEVEL);
    
    /* Enable interrupts and call the kernel's DPC interrupt handler */
    _enable();
    KiDispatchInterrupt();
    
    /* Disable interrupts and end the interrupt */
    _disable();
    HalpEndSoftwareInterrupt(CurrentIrql);
    
    /* Exit the interrupt */
    KiEoiHelper(TrapFrame);
}
