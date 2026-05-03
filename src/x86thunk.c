/*
 * Real Mode Thunk Functions for IA32 and x64.
 *
 * Based on EDK2: MdePkg/Library/BaseLib/X86Thunk.c
 *
 * Which is:
 * Copyright (c) 2006 - 2018, Intel Corporation. All rights reserved.<BR>
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */

#include <libc.h>
#include <printf.h>
#include "csmwrap.h"

// FIXME: Are we going to implement it?
#define ASSERT(x)

extern const uint8_t   m16Start;
extern const uint16_t  m16Size;
extern const uint16_t  mThunk16Attr;
extern const uint16_t  m16Gdt;
extern const uint16_t  m16GdtrBase;
extern const uint16_t  mTransition;

/**
  Invokes 16-bit code in big real mode and returns the updated register set.

  This function transfers control to the 16-bit code specified by CS:EIP using
  the stack specified by SS:ESP in RegisterSet. The updated registers are saved
  on the real mode stack and the starting address of the save area is returned.

  @param  RegisterSet Values of registers before invocation of 16-bit code.
  @param  Transition  The pointer to the transition code under 1MB.

  @return The pointer to a IA32_REGISTER_SET structure containing the updated
          register values.

**/
__attribute__((__ms_abi__))
IA32_REGISTER_SET *InternalAsmThunk16(IA32_REGISTER_SET  *RegisterSet, void *Transition);

/**
  Retrieves the properties for 16-bit thunk functions.

  Computes the size of the buffer and stack below 1MB required to use the
  AsmPrepareThunk16(), AsmThunk16() and AsmPrepareAndThunk16() functions. This
  buffer size is returned in RealModeBufferSize, and the stack size is returned
  in ExtraStackSize. If parameters are passed to the 16-bit real mode code,
  then the actual minimum stack size is ExtraStackSize plus the maximum number
  of bytes that need to be passed to the 16-bit real mode code.

  If RealModeBufferSize is NULL, then ASSERT().
  If ExtraStackSize is NULL, then ASSERT().

  @param  RealModeBufferSize  A pointer to the size of the buffer below 1MB
                              required to use the 16-bit thunk functions.
  @param  ExtraStackSize      A pointer to the extra size of stack below 1MB
                              that the 16-bit thunk functions require for
                              temporary storage in the transition to and from
                              16-bit real mode.

**/
void AsmGetThunk16Properties (uint32_t *RealModeBufferSize, uint32_t  *ExtraStackSize)
{
  ASSERT (RealModeBufferSize != NULL);
  ASSERT (ExtraStackSize != NULL);

  *RealModeBufferSize = m16Size;

  //
  // Extra 4 bytes for return address, and another 4 bytes for mode transition
  //
  *ExtraStackSize = sizeof (IA32_DWORD_REGS) + 8;
}

/**
  Prepares all structures a code required to use AsmThunk16().

  Prepares all structures and code required to use AsmThunk16().

  This interface is limited to be used in either physical mode or virtual modes with paging enabled where the
  virtual to physical mappings for ThunkContext.RealModeBuffer is mapped 1:1.

  If ThunkContext is NULL, then ASSERT().

  @param  ThunkContext  A pointer to the context structure that describes the
                        16-bit real mode code to call.

**/
void AsmPrepareThunk16 (THUNK_CONTEXT *ThunkContext)
{
  IA32_SEGMENT_DESCRIPTOR  *RealModeGdt;

  ASSERT (ThunkContext != NULL);
  ASSERT ((uintptr_t)ThunkContext->RealModeBuffer < 0x100000);
  ASSERT (ThunkContext->RealModeBufferSize >= m16Size);
  ASSERT ((uintptr_t)ThunkContext->RealModeBuffer + m16Size <= 0x100000);

  memcpy (ThunkContext->RealModeBuffer, &m16Start, m16Size);

  //
  // Point RealModeGdt to the GDT to be used in transition
  //
  // RealModeGdt[0]: Reserved as NULL descriptor
  // RealModeGdt[1]: Code Segment
  // RealModeGdt[2]: Data Segment
  // RealModeGdt[3]: Call Gate
  //
  RealModeGdt = (IA32_SEGMENT_DESCRIPTOR *)(
                                            (uintptr_t)ThunkContext->RealModeBuffer + m16Gdt);

  //
  // Update Code & Data Segment Descriptor
  //
  RealModeGdt[1].Bits.BaseLow =
    (uint32_t)(uintptr_t)ThunkContext->RealModeBuffer & ~0xf;
  RealModeGdt[1].Bits.BaseMid =
    (uint32_t)(uintptr_t)ThunkContext->RealModeBuffer >> 16;

  //
  // Update transition code entry point offset
  //
  *(uint32_t *)((uintptr_t)ThunkContext->RealModeBuffer + mTransition) +=
    (uint32_t)(uintptr_t)ThunkContext->RealModeBuffer & 0xf;

  //
  // Update Segment Limits for both Code and Data Segment Descriptors
  //
  if ((ThunkContext->ThunkAttributes & THUNK_ATTRIBUTE_BIG_REAL_MODE) == 0) {
    //
    // Set segment limits to 64KB
    //
    RealModeGdt[1].Bits.LimitHigh = 0;
    RealModeGdt[1].Bits.G         = 0;
    RealModeGdt[2].Bits.LimitHigh = 0;
    RealModeGdt[2].Bits.G         = 0;
  }

  //
  // Update GDTBASE for this thunk context
  //
  *(void **)((uintptr_t)ThunkContext->RealModeBuffer + m16GdtrBase) = RealModeGdt;

  //
  // Update Thunk Attributes
  //
  *(uint32_t *)((uintptr_t)ThunkContext->RealModeBuffer + mThunk16Attr) =
    ThunkContext->ThunkAttributes;
}

/**
  Transfers control to a 16-bit real mode entry point and returns the results.

  Transfers control to a 16-bit real mode entry point and returns the results.
  AsmPrepareThunk16() must be called with ThunkContext before this function is used.
  This function must be called with interrupts disabled.

  The register state from the RealModeState field of ThunkContext is restored just prior
  to calling the 16-bit real mode entry point.  This includes the EFLAGS field of RealModeState,
  which is used to set the interrupt state when a 16-bit real mode entry point is called.
  Control is transferred to the 16-bit real mode entry point specified by the CS and Eip fields of RealModeState.
  The stack is initialized to the SS and ESP fields of RealModeState.  Any parameters passed to
  the 16-bit real mode code must be populated by the caller at SS:ESP prior to calling this function.
  The 16-bit real mode entry point is invoked with a 16-bit CALL FAR instruction,
  so when accessing stack contents, the 16-bit real mode code must account for the 16-bit segment
  and 16-bit offset of the return address that were pushed onto the stack. The 16-bit real mode entry
  point must exit with a RETF instruction. The register state is captured into RealModeState immediately
  after the RETF instruction is executed.

  If EFLAGS specifies interrupts enabled, or any of the 16-bit real mode code enables interrupts,
  or any of the 16-bit real mode code makes a SW interrupt, then the caller is responsible for making sure
  the IDT at address 0 is initialized to handle any HW or SW interrupts that may occur while in 16-bit real mode.

  If EFLAGS specifies interrupts enabled, or any of the 16-bit real mode code enables interrupts,
  then the caller is responsible for making sure the 8259 PIC is in a state compatible with 16-bit real mode.
  This includes the base vectors, the interrupt masks, and the edge/level trigger mode.

  If THUNK_ATTRIBUTE_BIG_REAL_MODE is set in the ThunkAttributes field of ThunkContext, then the user code
  is invoked in big real mode.  Otherwise, the user code is invoked in 16-bit real mode with 64KB segment limits.

  If neither THUNK_ATTRIBUTE_DISABLE_A20_MASK_INT_15 nor THUNK_ATTRIBUTE_DISABLE_A20_MASK_KBD_CTRL are set in
  ThunkAttributes, then it is assumed that the user code did not enable the A20 mask, and no attempt is made to
  disable the A20 mask.

  If THUNK_ATTRIBUTE_DISABLE_A20_MASK_INT_15 is set and THUNK_ATTRIBUTE_DISABLE_A20_MASK_KBD_CTRL is clear in
  ThunkAttributes, then attempt to use the INT 15 service to disable the A20 mask.  If this INT 15 call fails,
  then attempt to disable the A20 mask by directly accessing the 8042 keyboard controller I/O ports.

  If THUNK_ATTRIBUTE_DISABLE_A20_MASK_INT_15 is clear and THUNK_ATTRIBUTE_DISABLE_A20_MASK_KBD_CTRL is set in
  ThunkAttributes, then attempt to disable the A20 mask by directly accessing the 8042 keyboard controller I/O ports.

  If ThunkContext is NULL, then ASSERT().
  If AsmPrepareThunk16() was not previously called with ThunkContext, then ASSERT().
  If both THUNK_ATTRIBUTE_DISABLE_A20_MASK_INT_15 and THUNK_ATTRIBUTE_DISABLE_A20_MASK_KBD_CTRL are set in
  ThunkAttributes, then ASSERT().

  This interface is limited to be used in either physical mode or virtual modes with paging enabled where the
  virtual to physical mappings for ThunkContext.RealModeBuffer is mapped 1:1.

  @param  ThunkContext  A pointer to the context structure that describes the
                        16-bit real mode code to call.

**/
void AsmThunk16 (THUNK_CONTEXT  *ThunkContext)
{
  IA32_REGISTER_SET  *UpdatedRegs;

  ASSERT (ThunkContext != NULL);
  ASSERT ((uintptr_t)ThunkContext->RealModeBuffer < 0x100000);
  ASSERT (ThunkContext->RealModeBufferSize >= m16Size);
  ASSERT ((uintptr_t)ThunkContext->RealModeBuffer + m16Size <= 0x100000);
  ASSERT (
    ((ThunkContext->ThunkAttributes & (THUNK_ATTRIBUTE_DISABLE_A20_MASK_INT_15 | THUNK_ATTRIBUTE_DISABLE_A20_MASK_KBD_CTRL)) != \
     (THUNK_ATTRIBUTE_DISABLE_A20_MASK_INT_15 | THUNK_ATTRIBUTE_DISABLE_A20_MASK_KBD_CTRL))
    );

  UpdatedRegs = InternalAsmThunk16 (
                  ThunkContext->RealModeState,
                  ThunkContext->RealModeBuffer
                  );

  memcpy(ThunkContext->RealModeState, UpdatedRegs, sizeof (*UpdatedRegs));
}

/**
  Prepares all structures and code for a 16-bit real mode thunk, transfers
  control to a 16-bit real mode entry point, and returns the results.

  Prepares all structures and code for a 16-bit real mode thunk, transfers
  control to a 16-bit real mode entry point, and returns the results. If the
  caller only need to perform a single 16-bit real mode thunk, then this
  service should be used. If the caller intends to make more than one 16-bit
  real mode thunk, then it is more efficient if AsmPrepareThunk16() is called
  once and AsmThunk16() can be called for each 16-bit real mode thunk.

  This interface is limited to be used in either physical mode or virtual modes with paging enabled where the
  virtual to physical mappings for ThunkContext.RealModeBuffer is mapped 1:1.

  See AsmPrepareThunk16() and AsmThunk16() for the detailed description and ASSERT() conditions.

  @param  ThunkContext  A pointer to the context structure that describes the
                        16-bit real mode code to call.

**/
void AsmPrepareAndThunk16 (THUNK_CONTEXT *ThunkContext)
{
  AsmPrepareThunk16(ThunkContext);
  AsmThunk16(ThunkContext);
}

THUNK_CONTEXT  mThunkContext;

bool InternalLegacyBiosFarCall (uint16_t Segment, uint16_t Offset, EFI_IA32_REGISTER_SET *Regs, void *Stack, uintptr_t StackSize)
{
//  uintptr_t                 Status;
  uint16_t                *Stack16;
//  EFI_TPL               OriginalTpl;
  IA32_REGISTER_SET     ThunkRegSet;
//  bool               InterruptState;
//  uint64_t                TimerPeriod;

  memset(&ThunkRegSet, 0, sizeof (ThunkRegSet));
  ThunkRegSet.X.DI = Regs->X.DI;
  ThunkRegSet.X.SI = Regs->X.SI;
  ThunkRegSet.X.BP = Regs->X.BP;
  ThunkRegSet.X.BX = Regs->X.BX;
  ThunkRegSet.X.DX = Regs->X.DX;
  //
  // Sometimes, ECX is used to pass in 32 bit data. For example, INT 1Ah, AX = B10Dh is
  // "PCI BIOS v2.0c + Write Configuration DWORD" and ECX has the dword to write.
  //
  ThunkRegSet.E.ECX = Regs->E.ECX;
  ThunkRegSet.X.AX  = Regs->X.AX;
  ThunkRegSet.E.DS  = Regs->X.DS;
  ThunkRegSet.E.ES  = Regs->X.ES;

  memcpy (&(ThunkRegSet.E.EFLAGS.UintN), &(Regs->X.Flags), sizeof (Regs->X.Flags));

  //
  // Clear the error flag; thunk code may set it. Stack16 should be the high address
  // Make Statk16 address the low 16 bit must be not zero.
  //
  Stack16 = (uint16_t *)((uint8_t *)mThunkContext.RealModeBuffer + mThunkContext.RealModeBufferSize - sizeof (uint16_t));

  //
  // Save current rate of DXE Timer
  //
  // Private->Timer->GetTimerPeriod (Private->Timer, &TimerPeriod);

  //
  // Disable DXE Timer while executing in real mode
  //
  // Private->Timer->SetTimerPeriod (Private->Timer, 0);

  //
  // Save and disable interrupt of debug timer
  //
  // InterruptState = SaveAndSetDebugTimerInterrupt (FALSE);

  //
  // The call to Legacy16 is a critical section to EFI
  //
  // OriginalTpl = ggBS->RaiseTPL (TPL_HIGH_LEVEL);

  //
  // Check to see if there is more than one HW interrupt registers with the CPU AP.
  // If there is, then ASSERT() since that is not compatible with the CSM because
  // interupts other than the Timer interrupt that was disabled above can not be
  // handled properly from real mode.
  //
  #if 0
  DEBUG_CODE_BEGIN ();
  uintptr_t  Vector;
  uintptr_t  Count;

  for (Vector = 0x20, Count = 0; Vector < 0x100; Vector++) {
    Status = Private->Cpu->RegisterInterruptHandler (Private->Cpu, Vector, LegacyBiosNullInterruptHandler);
    if (Status == EFI_ALREADY_STARTED) {
      Count++;
    }

    if (Status == EFI_SUCCESS) {
      Private->Cpu->RegisterInterruptHandler (Private->Cpu, Vector, NULL);
    }
  }

  if (Count >= 2) {
    printf("ERROR: More than one HW interrupt active with CSM enabled\n");
  }

  ASSERT (Count < 2);
  DEBUG_CODE_END ();
#endif

  //
  // If the Timer AP has enabled the 8254 timer IRQ and the current 8254 timer
  // period is less than the CSM required rate of 54.9254, then force the 8254
  // PIT counter to 0, which is the CSM required rate of 54.9254 ms
  //
  // if (Private->TimerUses8254 && (TimerPeriod < 549254)) {
  //  SetPitCount (0);
  // }

  if ((Stack != NULL) && (StackSize != 0)) {
    //
    // Copy Stack to low memory stack
    //
    Stack16 -= StackSize / sizeof (uint16_t);
    memcpy (Stack16, Stack, StackSize);
  }

  ThunkRegSet.E.SS  = (uint16_t)(((uintptr_t)Stack16 >> 16) << 12);
  ThunkRegSet.E.ESP = (uint16_t)(uintptr_t)Stack16;
  ThunkRegSet.E.CS  = Segment;
  ThunkRegSet.E.Eip = Offset;

  mThunkContext.RealModeState = &ThunkRegSet;

  //
  // Set Legacy16 state. 0x08, 0x70 is legacy 8259 vector bases.
  //
  /* FIXME: DO we need to care 8259? */
  // Status = Private->Legacy8259->SetMode (Private->Legacy8259, Efi8259LegacyMode, NULL, NULL);
  // ASSERT_EFI_ERROR (Status);

  AsmThunk16 (&mThunkContext);

  if ((Stack != NULL) && (StackSize != 0)) {
    //
    // Copy low memory stack to Stack
    //
    memcpy (Stack, Stack16, StackSize);
  }

  //
  // Restore protected mode interrupt state
  //
  /* FIXME: DO we need to care 8259? */
  // Status = Private->Legacy8259->SetMode (Private->Legacy8259, Efi8259ProtectedMode, NULL, NULL);
  // ASSERT_EFI_ERROR (Status);

  mThunkContext.RealModeState = NULL;

  //
  // Enable and restore rate of DXE Timer
  //
  // Private->Timer->SetTimerPeriod (Private->Timer, TimerPeriod);

  //
  // End critical section
  //
  // ggBS->RestoreTPL (OriginalTpl);

  //
  // OPROM may allocate EBDA range by itself and change EBDA base and EBDA size.
  // Get the current EBDA base address, and compared with pre-allocate minimum
  // EBDA base address, if the current EBDA base address is smaller, it indicates
  // PcdEbdaReservedMemorySize should be adjusted to larger for more OPROMs.
  //
#if 0
  DEBUG_CODE_BEGIN ();
  {
    uintptr_t  EbdaBaseAddress;
    uintptr_t  ReservedEbdaBaseAddress;

    ACCESS_PAGE0_CODE (
      EbdaBaseAddress         = (*(uint16_t *)(uintptr_t)0x40E) << 4;
      ReservedEbdaBaseAddress = CONVENTIONAL_MEMORY_TOP
                                - PcdGet32 (PcdEbdaReservedMemorySize);
      ASSERT (ReservedEbdaBaseAddress <= EbdaBaseAddress);
      );
  }
  DEBUG_CODE_END ();
#endif

  //
  // Restore interrupt of debug timer
  //
  // SaveAndSetDebugTimerInterrupt (InterruptState);

  Regs->E.EDI = ThunkRegSet.E.EDI;
  Regs->E.ESI = ThunkRegSet.E.ESI;
  Regs->E.EBP = ThunkRegSet.E.EBP;
  Regs->E.EBX = ThunkRegSet.E.EBX;
  Regs->E.EDX = ThunkRegSet.E.EDX;
  Regs->E.ECX = ThunkRegSet.E.ECX;
  Regs->E.EAX = ThunkRegSet.E.EAX;
  Regs->X.SS  = ThunkRegSet.E.SS;
  Regs->X.CS  = ThunkRegSet.E.CS;
  Regs->X.DS  = ThunkRegSet.E.DS;
  Regs->X.ES  = ThunkRegSet.E.ES;

  memcpy (&(Regs->X.Flags), &(ThunkRegSet.E.EFLAGS.UintN), sizeof (Regs->X.Flags));

  return (bool)(Regs->X.Flags.CF == 1);
}

// Return final pointer
uintptr_t LegacyBiosInitializeThunkAndTable(uintptr_t MemoryAddress, size_t data_size) {
  uintptr_t data_pages = (data_size + EFI_PAGE_SIZE - 1) / EFI_PAGE_SIZE;

  mThunkContext.RealModeBuffer     = (void *)(uintptr_t)(MemoryAddress + (data_pages * EFI_PAGE_SIZE));
  mThunkContext.RealModeBufferSize = EFI_PAGE_SIZE;
  mThunkContext.ThunkAttributes    = THUNK_ATTRIBUTE_BIG_REAL_MODE | THUNK_ATTRIBUTE_DISABLE_A20_MASK_INT_15;

  memset(mThunkContext.RealModeBuffer, 0, mThunkContext.RealModeBufferSize);

  printf("RealmodeBuffer %lx\n", (uintptr_t)mThunkContext.RealModeBuffer);

  AsmPrepareThunk16 (&mThunkContext);

  return (uintptr_t)mThunkContext.RealModeBuffer + mThunkContext.RealModeBufferSize + EFI_PAGE_SIZE;
}

bool LegacyBiosInt86(uint8_t BiosInt, EFI_IA32_REGISTER_SET *Regs)
{
  uint16_t Segment;
  uint16_t Offset;

  /* FIXME: Not working!!!!!!!!!!!! */

  Regs->X.Flags.Reserved1 = 1;
  Regs->X.Flags.Reserved2 = 0;
  Regs->X.Flags.Reserved3 = 0;
  Regs->X.Flags.Reserved4 = 0;
  Regs->X.Flags.IOPL      = 3;
  Regs->X.Flags.NT        = 0;
  Regs->X.Flags.IF        = 0;
  Regs->X.Flags.TF        = 0;
  Regs->X.Flags.CF        = 0;


  //
  // The base address of legacy interrupt vector table is 0.
  // We use this base address to get the legacy interrupt handler.
  //
  ACCESS_PAGE0_CODE (
    Segment = (UINT16)(((UINT32 *)0)[BiosInt] >> 16);
    Offset  = (UINT16)((UINT32 *)0)[BiosInt];
    );

  return InternalLegacyBiosFarCall (
           Segment,
           Offset,
           Regs,
           &Regs->X.Flags,
           sizeof (Regs->X.Flags)
           );
}

/**
  Thunk to 16-bit real mode and call Segment:Offset. Regs will contain the
  16-bit register context on entry and exit. Arguments can be passed on
  the Stack argument

  @param  This                   Protocol instance pointer.
  @param  Segment                Segemnt of 16-bit mode call
  @param  Offset                 Offset of 16-bit mdoe call
  @param  Regs                   Register contexted passed into (and returned) from
                                 thunk to  16-bit mode
  @param  Stack                  Caller allocated stack used to pass arguments
  @param  StackSize              Size of Stack in bytes

  @retval FALSE                  Thunk completed, and there were no BIOS errors in
                                 the target code. See Regs for status.
  @retval TRUE                   There was a BIOS erro in the target code.

**/
bool LegacyBiosFarCall86 (uint16_t Segment, uint16_t Offset, EFI_IA32_REGISTER_SET *Regs, void *Stack, uintptr_t StackSize)
{
  Regs->X.Flags.Reserved1 = 1;
  Regs->X.Flags.Reserved2 = 0;
  Regs->X.Flags.Reserved3 = 0;
  Regs->X.Flags.Reserved4 = 0;
  Regs->X.Flags.IOPL      = 3;
  Regs->X.Flags.NT        = 0;
  Regs->X.Flags.IF        = 1;
  Regs->X.Flags.TF        = 0;
  Regs->X.Flags.CF        = 0;

  return InternalLegacyBiosFarCall (Segment, Offset, Regs, Stack, StackSize);
}
