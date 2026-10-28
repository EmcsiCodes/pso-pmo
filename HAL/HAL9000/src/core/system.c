#include "HAL9000.h"
#include "bootinfo.h"
#include "efi_runtime.h"
#include "system.h"
#include "idt.h"
#include "iomu.h"
#include "os_info.h"
#include "cpumu.h"
#include "smp.h"
#include "print.h"
#include "synch.h"
#include "mmu.h"
#include "thread_internal.h"
#include "gdtmu.h"
#include "lapic_system.h"
#include "idt_handlers.h"
#include "serial_comm.h"
#include "core.h"
#include "acpi_interface.h"
#include "network_stack.h"
#include "dmp_common.h"
#include "ex_system.h"
#include "process_internal.h"
#include "boot_module.h"
#include "display.h"
#include "debug_database.h"

#include "ex_timer.h"

#define NO_OF_TSS_STACKS             7
STATIC_ASSERT(NO_OF_TSS_STACKS <= NO_OF_IST);

typedef struct _SYSTEM_DATA
{
    BYTE        NumberOfTssStacks;
} SYSTEM_DATA, *PSYSTEM_DATA;

static SYSTEM_DATA m_systemData;

QWORD gVirtualToPhysicalOffset;
QWORD gAcpiRsdpAddress;

void
SystemPreinit(
    IN  HAL_BOOT_INFORMATION*     BootInformation
    )
{
    DISPLAY_INFORMATION displayInformation;
    BOOLEAN isSpinlock;
    PLOCK pLock;
    PRW_SPINLOCK pRwSpinlock;
    PMUTEX pMutex;
    PLIST_ENTRY pList;
    PLIST_ENTRY pAnotherList;

    memzero(&m_systemData, sizeof(SYSTEM_DATA));

    displayInformation.FrameBufferAddress = BootInformation->VirtualDisplayAddress;
    memcpy(&(displayInformation.FrameBufferPitch), 
           &(BootInformation->Framebuffer.Pitch),
           sizeof(DISPLAY_INFORMATION) - sizeof(QWORD));
    EFI_RUNTIME_SERVICES *EfiRuntimeServices = 
           (EFI_RUNTIME_SERVICES *) BootInformation->EfiRuntimeServices;

    m_systemData.NumberOfTssStacks = NO_OF_TSS_STACKS;

    BootModulesPreinit();
    DumpPreinit();
    ExEventSystemPreinit();

    //NEW (Timer system for Threads to wait on)
    ExTimerSystemPreinit();

    MutexSystemPreinit();
    ThreadSystemPreinit();
    printSystemPreinit(&displayInformation);
    LogSystemPreinit();
    OsInfoPreinit();
    MmuPreinitSystem();
    EfiRuntimePreinit(EfiRuntimeServices);
    IomuPreinitSystem();
    AcpiInterfacePreinit();
    SmpPreinit();
    PciSystemPreinit();
    CorePreinit();
    NetworkStackPreinit();
    ProcessSystemPreinit();
    
    DebugDatabasePreinit();
    MutexSystemGetMutexList(&pLock, &pList);
    DebugDatabaseSetMutexList(pLock, pList);
    ThreadSystemGetThreadList(&pLock, &pList);
    DebugDatabaseSetThreadList(pLock, pList);
    ProcessSystemGetProcessList(&pMutex, &pList);
    DebugDatabaseSetProcessList(pMutex, pList);
    LockSystemGetLockList(&isSpinlock, &pLock, &pList);
    RwSpinlockSystemGetLockList(&pRwSpinlock, &pAnotherList);
    DebugDatabaseSetLockLists(isSpinlock, pLock, pList, pRwSpinlock, pAnotherList);
    IoFilesSystemGetFileList(&pLock, &pList);
    DebugSystemSetFilesList(pLock, pList);
    ExEventSystemGetEventList(&pLock, &pList);
    DebugSystemSetEventList(pLock, pList);
}

STATUS
SystemInit(
    IN  HAL_BOOT_INFORMATION*     BootInformation
    )
{
    STATUS status;
    PCPU* pCpu;
    DWORD BootModuleCount;
    PHYSICAL_ADDRESS BootModules;
    PVOID kernelBaseAddress;
    DWORD kernelSize;

    status = STATUS_SUCCESS;
    pCpu = NULL;
    BootModuleCount = BootInformation->BootModuleCount;
    BootModules = (PHYSICAL_ADDRESS) ((QWORD) BootInformation->BootModules);
    kernelBaseAddress = (PVOID) ((QWORD) BootInformation->KernelBaseAddress);
    kernelSize = BootInformation->KernelSize;

    LogSystemInit(LogLevelInfo,
                  LogComponentInterrupt | LogComponentIo | LogComponentAcpi | LogComponentUserMode,
                  TRUE
                  );

    // if validation fails => the system will HALT
    CpuMuValidateConfiguration();

    HalInitialize();

    // install new GDT table
    status = GdtMuInit();
    if (!SUCCEEDED(status))
    {
        LOG_FUNC_ERROR("GdtMuInit", status);
        return status;
    }

    // initialize serial communication
    status = SerialCommunicationInitialize(BootInformation->SerialPorts, BIOS_MAX_NO_OF_SERIAL_PORTS);
    if (!SUCCEEDED(status))
    {
        LOG_FUNC_ERROR("SerialCommunicationInitialize", status);
        return status;
    }

    LOG("Serial communications initialized\n");
    LOG("Running HAL9000 %s version %s built on %s\n",
        OsGetBuildType(),
        OsGetVersion(),
        OsGetBuildDate()
        );

    status = OsInfoInit();
    if (!SUCCEEDED(status))
    {
        LOG_FUNC_ERROR("OsInfoInit", status);
        return status;
    }

    LOGL("OsInfoInit succeeded\n");

    LOGL("EFI Runtime Services Virtual Address is %X\n", BootInformation->EfiRuntimeVirtualAddress);
    LOGL("EFI Runtime Services Descriptor Count is %X\n", BootInformation->EfiRuntimeDescriptorCount);
    LOGL("EFI Runtime Services Pointer is %X\n", BootInformation->EfiRuntimeServices);

    status = CpuMuActivateFpuFeatures();
    if (!SUCCEEDED(status))
    {
        LOG_FUNC_ERROR("CpuMuActivateFpuFeatures", status);
        return status;
    }

    LOGL("CpuMuActivateFpuFeatures succeeded\n");
     
    // IDT handlers need to be initialized before
    // MmuInitSystem is called because the VMM
    // needs page fault handling to allocate memory
    status = InitIdtHandlers(GdtMuGetCS64Supervisor(), 0);
    if (!SUCCEEDED(status))
    {
        LOG_FUNC_ERROR("InitIdtHandlers", status);
        return status;
    }

    LOGL("InitIdtHandlers succeeded\n");
    
    status = MmuInitSystem((PVOID) BootInformation);
    if (!SUCCEEDED(status))
    {
        LOG_FUNC_ERROR("MmuInitSystem", status);
        return status;
    }

    LOGL("MmuInitSystem succeeded\n");

    status = EfiRuntimeInit(); 
    if (!SUCCEEDED(status))
    {
        LOG_FUNC_ERROR("EfiRuntimeInitSystem", status);
        return status;
    }

    LOGL("EfiRuntimeInitSystem succeeded\n");

    // BootInformation contents not valid from here, it is not mapped
    if (BootModuleCount > 0)
    {
        LOG("We have %d boot modules at %X\n", BootModuleCount, BootModules);
        status = BootModulesInit(BootModules,
                                 BootModuleCount);
        if (!SUCCEEDED(status))
        {
            LOG_FUNC_ERROR("BootModulesMap", status);
            return status;
        }
    }

    status = IomuInitSystemDriver();
    if (!SUCCEEDED(status))
    {
        LOG_FUNC_ERROR("IomuInitSystemDriver", status);
        return status;
    }
    LOGL("IomuInitSystemDriver suceeded\n");

    // initialize ACPI interface
    status = AcpiInterfaceInit();
    if (!SUCCEEDED(status))
    {
        LOG_FUNC_ERROR("AcpiInterfaceInit", status);
        return status;
    }
    LOGL("AcpiInterfaceInit suceeded\n");

    status = LapicSystemInit();
    if (!SUCCEEDED(status))
    {
        LOG_FUNC_ERROR("LapicSystemInit", status);
        return status;
    }
    LOGL("LapicSystemInit suceeded\n");

    status = SmpInit();
    if (!SUCCEEDED(status))
    {
        LOG_FUNC_ERROR("SmpInit", status);
        return status;
    }

    LOGL("SmpInit succeded\n");

    // allocate PCPU structure for the BSP
    // this needs to be before the call to IomuInitSystem because
    // by the time we enable interrupts we want our TSS descriptor to be installed
    status = CpuMuAllocAndInitCpu(&pCpu,
    // C28039: The type of actual parameter 'CpuGetApicId()' should exactly match the type 'APIC_ID'
MSVC_WARNING_SUPPRESS(28039)
                                  CpuGetApicId(),
                                  STACK_DEFAULT_SIZE,
                                  m_systemData.NumberOfTssStacks
                                  );
    if (!SUCCEEDED(status))
    {
        LOG_FUNC_ERROR("CpuMuAllocAndInitCpu", status);
        return status;
    }
    LOGL("CpuMuAllocAndInitCpu succeeded\n");

    // initialize IO system
    // this also initializes the IDT
    status = IomuInitSystem(GdtMuGetCS64Supervisor(),m_systemData.NumberOfTssStacks );
    if (!SUCCEEDED(status))
    {
        LOG_FUNC_ERROR("IomuInitSystem", status);
        return status;
    }

    LOGL("IomuInitSystem succeeded\n");

    status = CoreInit();
    if (!SUCCEEDED(status))
    {
        LOG_FUNC_ERROR("CoreInit", status);
        return status;
    }

    LOGL("CoreInit succeeded\n");

    status = SmpSetupLowerMemory(m_systemData.NumberOfTssStacks);
    if (!SUCCEEDED(status))
    {
        LOG_FUNC_ERROR("SmpSetupLowerMemory", status);
        return status;
    }

    LOGL("SmpSetupLowerMemory succeded\n");

    status = ProcessSystemInitSystemProcess();
    if (!SUCCEEDED(status))
    {
        LOG_FUNC_ERROR("ProcessSystemInitSystemProcess", status);
        return status;
    }

    LOGL("Successfully intiialized system process!\n");

    status = ThreadSystemInitIdleForCurrentCPU();
    if (!SUCCEEDED(status))
    {
        LOG_FUNC_ERROR("ThreadSystemInitIdleForCurrentCPU", status);
        return status;
    }

    LOGL("ThreadSystemInitIdleForCurrentCPU succeeded\n");

    status = AcpiInterfaceLateInit();
    if (!SUCCEEDED(status))
    {
        LOG_FUNC_ERROR("AcpiInterfaceLateInit", status);
        return status;
    }
    LOGL("AcpiInterfaceLateInit succeeded\n");

    SmpWakeupAps();
    LOGL("SmpWakeupAps completed\n");

    // finish IOMU initialization
    status = IomuInitSystemAfterApWakeup();
    if (!SUCCEEDED(status))
    {
        LOG_FUNC_ERROR("IomuInitSystemAfterApWakeup", status);
        return status;
    }
    LOGL("IomuInitSystemAfterApWakeup succeeded\n");

    // we no longer need the lower memory mappings
    SmpCleanupLowerMemory();

    LOGL("SmpCleanupLowerMemory completed\n");

    // After the APs have woken up we no longer need the 1:1 VA->PA mappings
    MmuDiscardIdentityMappings(kernelBaseAddress, kernelSize);

    LOGL("MmuDiscardIdentityMappings completed\n");

    status = MmuInitThreadingSystem();
    if (!SUCCEEDED(status))
    {
        LOG_FUNC_ERROR("MmuInitThreadingSystem", status );
        return status;
    }

    LOGL("MmuInitThreadingSystem succeded\n");

    // IOMU late initialization: drivers + system partition determination
    status = IomuLateInit();
    if (!SUCCEEDED(status))
    {
        LOG_FUNC_ERROR("IomuLateInit", status);
        return status;
    }

    LOGL("IOMU late initialization successfully completed\n");

    // FIXME: Commented because the networking devices causes a kernel panic when a breakpoint is added in this function
    // status = NetworkStackInit(TRUE);
    // if (!SUCCEEDED(status))
    // {
    //     LOG_FUNC_ERROR("NetworkStackInit", status);
    //     return status;
    // }

    // LOGL("Network stack successfully initialized\n");

    EFI_TIME Time;
    EfiRuntimeGetTime(&Time, NULL);
    LOG("Year: %d\n", Time.Year);

    return status;
}

void
SystemUninit(
    void
    )
{
    LOGL("Finished command execution\n");

    LOGL("%s terminating!\n", OsInfoGetName());

    // disable interrupts
    CpuIntrDisable();
}