#include "debugger_commands.h"
#include "debugger.h"
#include "console.h"
#include "value.h"
#include "thread.h"
#include "memory.h"
#include "threading.h"
#include "variable.h"
#include "argument.h"
#include "plugin_loader.h"
#include "simplescript.h"
#include "symbolinfo.h"

static bool bScyllaLoaded = false;

CMDRESULT cbDebugInit(int argc, char* argv[])
{
    if(DbgIsDebugging())
    {
        dputs("already debugging!");
        return STATUS_ERROR;
    }

    static char arg1[deflen] = "";
    if(!argget(*argv, arg1, 0, false))
        return STATUS_ERROR;
    if(!FileExists(arg1))
    {
        dputs("file does not exist!");
        return STATUS_ERROR;
    }
    HANDLE hFile = CreateFileA(arg1, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
    if(hFile == INVALID_HANDLE_VALUE)
    {
        dputs("could not open file!");
        return STATUS_ERROR;
    }
    GetFileNameFromHandle(hFile, arg1); //get full path of the file
    CloseHandle(hFile);

    //do some basic checks
    switch(GetFileArchitecture(arg1))
    {
    case invalid:
        dputs("invalid PE file!");
        return STATUS_ERROR;
#ifdef _WIN64
    case x32:
        dputs("use x32_dbg to debug this file!");
#else //x86
    case x64:
        dputs("use x64_dbg to debug this file!");
#endif //_WIN64
        return STATUS_ERROR;
    default:
        break;
    }

    static char arg2[deflen] = "";
    argget(*argv, arg2, 1, true);
    char* commandline = 0;
    if(strlen(arg2))
        commandline = arg2;

    char arg3[deflen] = "";
    argget(*argv, arg3, 2, true);

    static char currentfolder[deflen] = "";
    strcpy(currentfolder, arg1);
    int len = (int)strlen(currentfolder);
    while(currentfolder[len] != '\\' and len != 0)
        len--;
    currentfolder[len] = 0;
    if(DirExists(arg3))
        strcpy(currentfolder, arg3);
    INIT_STRUCT* init = (INIT_STRUCT*)emalloc(sizeof(INIT_STRUCT), "cbDebugInit:init");
    memset(init, 0, sizeof(INIT_STRUCT));
    init->exe = arg1;
    init->commandline = commandline;
    if(*currentfolder)
        init->currentfolder = currentfolder;
    //initialize
    wait(WAITID_STOP); //wait for the debugger to stop
    waitclear(); //clear waiting flags NOTE: thread-unsafe
    CloseHandle(CreateThread(0, 0, threadDebugLoop, init, 0, 0));
    return STATUS_CONTINUE;
}

CMDRESULT cbStopDebug(int argc, char* argv[])
{
    scriptreset(); //reset the currently-loaded script
    StopDebug();
    unlock(WAITID_RUN);
    wait(WAITID_STOP);
    return STATUS_CONTINUE;
}

CMDRESULT cbDebugRun(int argc, char* argv[])
{
    if(!waitislocked(WAITID_RUN))
    {
        dputs("program is already running");
        return STATUS_ERROR;
    }
    GuiSetDebugState(running);
    unlock(WAITID_RUN);
    PLUG_CB_RESUMEDEBUG callbackInfo;
    callbackInfo.reserved = 0;
    plugincbcall(CB_RESUMEDEBUG, &callbackInfo);
    return STATUS_CONTINUE;
}

CMDRESULT cbDebugErun(int argc, char* argv[])
{
    if(waitislocked(WAITID_RUN))
        dbgsetskipexceptions(true);
    return cbDebugRun(argc, argv);
}

CMDRESULT cbDebugSetBPXOptions(int argc, char* argv[])
{
    char argtype[deflen] = "";
    DWORD type = 0;
    if(!argget(*argv, argtype, 0, false))
        return STATUS_ERROR;
    const char* a = 0;
    uint setting_type;
    if(strstr(argtype, "long"))
    {
        setting_type = 1; //break_int3long
        a = "TYPE_LONG_INT3";
        type = UE_BREAKPOINT_LONG_INT3;
    }
    else if(strstr(argtype, "ud2"))
    {
        setting_type = 2; //break_ud2
        a = "TYPE_UD2";
        type = UE_BREAKPOINT_UD2;
    }
    else if(strstr(argtype, "short"))
    {
        setting_type = 0; //break_int3short
        a = "TYPE_INT3";
        type = UE_BREAKPOINT_INT3;
    }
    else
    {
        dputs("invalid type specified!");
        return STATUS_ERROR;
    }
    SetBPXOptions(type);
    BridgeSettingSetUint("Engine", "BreakpointType", setting_type);
    dprintf("default breakpoint type set to: %s\n", a);
    return STATUS_CONTINUE;
}

CMDRESULT cbDebugSetBPX(int argc, char* argv[]) //bp addr [,name [,type]]
{
    char argaddr[deflen] = "";
    if(!argget(*argv, argaddr, 0, false))
        return STATUS_ERROR;
    char argname[deflen] = "";
    argget(*argv, argname, 1, true);
    char argtype[deflen] = "";
    bool has_arg2 = argget(*argv, argtype, 2, true);
    if(!has_arg2 and (scmp(argname, "ss") or scmp(argname, "long") or scmp(argname, "ud2")))
    {
        strcpy(argtype, argname);
        *argname = 0;
    }
    _strlwr(argtype);
    uint addr = 0;
    if(!valfromstring(argaddr, &addr))
    {
        dprintf("invalid addr: \"%s\"\n", argaddr);
        return STATUS_ERROR;
    }
    int type = 0;
    bool singleshoot = false;
    if(strstr(argtype, "ss"))
    {
        type |= UE_SINGLESHOOT;
        singleshoot = true;
    }
    else
        type |= UE_BREAKPOINT;
    if(strstr(argtype, "long"))
        type |= UE_BREAKPOINT_TYPE_LONG_INT3;
    else if(strstr(argtype, "ud2"))
        type |= UE_BREAKPOINT_TYPE_UD2;
    else if(strstr(argtype, "short"))
        type |= UE_BREAKPOINT_TYPE_INT3;
    short oldbytes;
    const char* bpname = 0;
    if(*argname)
        bpname = argname;
    if(bpget(addr, BPNORMAL, bpname, 0))
    {
        dputs("breakpoint already set!");
        return STATUS_CONTINUE;
    }
    if(IsBPXEnabled(addr))
    {
        dprintf("error setting breakpoint at "fhex"!\n (IsBPXEnabled)", addr);
        return STATUS_ERROR;
    }
    else if(!memread(fdProcessInfo->hProcess, (void*)addr, &oldbytes, sizeof(short), 0))
    {
        dprintf("error setting breakpoint at "fhex"!\n (memread)", addr);
        return STATUS_ERROR;
    }
    else if(!bpnew(addr, true, singleshoot, oldbytes, BPNORMAL, type, bpname))
    {
        dprintf("error setting breakpoint at "fhex"!\n (bpnew)", addr);
        return STATUS_ERROR;
    }
    else if(!SetBPX(addr, type, (void*)cbUserBreakpoint))
    {
        dprintf("error setting breakpoint at "fhex"! (SetBPX)\n", addr);
        return STATUS_ERROR;
    }
    dprintf("breakpoint at "fhex" set!\n", addr);
    GuiUpdateAllViews();
    return STATUS_CONTINUE;
}

CMDRESULT cbDebugDeleteBPX(int argc, char* argv[])
{
    char arg1[deflen] = "";
    if(!argget(*argv, arg1, 0, true)) //delete all breakpoints
    {
        if(!bpgetcount(BPNORMAL))
        {
            dputs("no breakpoints to delete!");
            return STATUS_CONTINUE;
        }
        if(!bpenumall(cbDeleteAllBreakpoints)) //at least one deletion failed
            return STATUS_ERROR;
        dputs("all breakpoints deleted!");
        GuiUpdateAllViews();
        return STATUS_CONTINUE;
    }
    BREAKPOINT found;
    if(bpget(0, BPNORMAL, arg1, &found)) //found a breakpoint with name
    {
        if(!bpdel(found.addr, BPNORMAL))
        {
            dprintf("delete breakpoint failed (bpdel): "fhex"\n", found.addr);
            return STATUS_ERROR;
        }
        else if(found.enabled && !DeleteBPX(found.addr))
        {
            dprintf("delete breakpoint failed (DeleteBPX): "fhex"\n", found.addr);
            GuiUpdateAllViews();
            return STATUS_ERROR;
        }
        return STATUS_CONTINUE;
    }
    uint addr = 0;
    if(!valfromstring(arg1, &addr) or !bpget(addr, BPNORMAL, 0, &found)) //invalid breakpoint
    {
        dprintf("no such breakpoint \"%s\"\n", arg1);
        return STATUS_ERROR;
    }
    if(!bpdel(found.addr, BPNORMAL))
    {
        dprintf("delete breakpoint failed (bpdel): "fhex"\n", found.addr);
        return STATUS_ERROR;
    }
    else if(found.enabled && !DeleteBPX(found.addr))
    {
        dprintf("delete breakpoint failed (DeleteBPX): "fhex"\n", found.addr);
        GuiUpdateAllViews();
        return STATUS_ERROR;
    }
    dputs("breakpoint deleted!");
    GuiUpdateAllViews();
    return STATUS_CONTINUE;
}

CMDRESULT cbDebugEnableBPX(int argc, char* argv[])
{
    char arg1[deflen] = "";
    if(!argget(*argv, arg1, 0, true)) //enable all breakpoints
    {
        if(!bpgetcount(BPNORMAL))
        {
            dputs("no breakpoints to enable!");
            return STATUS_CONTINUE;
        }
        if(!bpenumall(cbEnableAllBreakpoints)) //at least one enable failed
            return STATUS_ERROR;
        dputs("all breakpoints enabled!");
        GuiUpdateAllViews();
        return STATUS_CONTINUE;
    }
    BREAKPOINT found;
    if(bpget(0, BPNORMAL, arg1, &found)) //found a breakpoint with name
    {
        if(!bpenable(found.addr, BPNORMAL, true) or !SetBPX(found.addr, found.titantype, (void*)cbUserBreakpoint))
        {
            dprintf("could not enable breakpoint "fhex"\n", found.addr);
            return STATUS_ERROR;
        }
        GuiUpdateAllViews();
        return STATUS_CONTINUE;
    }
    uint addr = 0;
    if(!valfromstring(arg1, &addr) or !bpget(addr, BPNORMAL, 0, &found)) //invalid breakpoint
    {
        dprintf("no such breakpoint \"%s\"\n", arg1);
        return STATUS_ERROR;
    }
    if(found.enabled)
    {
        dputs("breakpoint already enabled!");
        GuiUpdateAllViews();
        return STATUS_CONTINUE;
    }
    if(!bpenable(found.addr, BPNORMAL, true) or !SetBPX(found.addr, found.titantype, (void*)cbUserBreakpoint))
    {
        dprintf("could not enable breakpoint "fhex"\n", found.addr);
        return STATUS_ERROR;
    }
    dputs("breakpoint enabled!");
    GuiUpdateAllViews();
    return STATUS_CONTINUE;
}

CMDRESULT cbDebugDisableBPX(int argc, char* argv[])
{
    char arg1[deflen] = "";
    if(!argget(*argv, arg1, 0, true)) //delete all breakpoints
    {
        if(!bpgetcount(BPNORMAL))
        {
            dputs("no breakpoints to disable!");
            return STATUS_CONTINUE;
        }
        if(!bpenumall(cbDisableAllBreakpoints)) //at least one deletion failed
            return STATUS_ERROR;
        dputs("all breakpoints disabled!");
        GuiUpdateAllViews();
        return STATUS_CONTINUE;
    }
    BREAKPOINT found;
    if(bpget(0, BPNORMAL, arg1, &found)) //found a breakpoint with name
    {
        if(!bpenable(found.addr, BPNORMAL, false) or !DeleteBPX(found.addr))
        {
            dprintf("could not disable breakpoint "fhex"\n", found.addr);
            return STATUS_ERROR;
        }
        GuiUpdateAllViews();
        return STATUS_CONTINUE;
    }
    uint addr = 0;
    if(!valfromstring(arg1, &addr) or !bpget(addr, BPNORMAL, 0, &found)) //invalid breakpoint
    {
        dprintf("no such breakpoint \"%s\"\n", arg1);
        return STATUS_ERROR;
    }
    if(!found.enabled)
    {
        dputs("breakpoint already disabled!");
        return STATUS_CONTINUE;
    }
    if(!bpenable(found.addr, BPNORMAL, false) or !DeleteBPX(found.addr))
    {
        dprintf("could not disable breakpoint "fhex"\n", found.addr);
        return STATUS_ERROR;
    }
    dputs("breakpoint disabled!");
    GuiUpdateAllViews();
    return STATUS_CONTINUE;
}

CMDRESULT cbDebugBplist(int argc, char* argv[])
{
    bpenumall(cbBreakpointList);
    return STATUS_CONTINUE;
}

CMDRESULT cbDebugStepInto(int argc, char* argv[])
{
    StepInto((void*)cbStep);
    dbgsetstepping(true);
    return cbDebugRun(argc, argv);
}

CMDRESULT cbDebugeStepInto(int argc, char* argv[])
{
    dbgsetskipexceptions(true);
    return cbDebugStepInto(argc, argv);
}

CMDRESULT cbDebugStepOver(int argc, char* argv[])
{
    StepOver((void*)cbStep);
    dbgsetstepping(true);
    return cbDebugRun(argc, argv);
}

CMDRESULT cbDebugeStepOver(int argc, char* argv[])
{
    dbgsetskipexceptions(true);
    return cbDebugStepOver(argc, argv);
}

CMDRESULT cbDebugSingleStep(int argc, char* argv[])
{
    char arg1[deflen] = "";
    uint stepcount = 1;
    if(argget(*argv, arg1, 0, true))
    {
        if(!valfromstring(arg1, &stepcount))
            stepcount = 1;
    }
    SingleStep((DWORD)stepcount, (void*)cbStep);
    dbgsetstepping(true);
    return cbDebugRun(argc, argv);
}

CMDRESULT cbDebugeSingleStep(int argc, char* argv[])
{
    dbgsetskipexceptions(true);
    return cbDebugSingleStep(argc, argv);
}

CMDRESULT cbDebugHide(int argc, char* argv[])
{
    if(HideDebugger(fdProcessInfo->hProcess, UE_HIDE_PEBONLY))
        dputs("debugger hidden");
    else
        dputs("something went wrong");
    return STATUS_CONTINUE;
}

CMDRESULT cbDebugDisasm(int argc, char* argv[])
{
    char arg1[deflen] = "";
    uint addr = GetContextDataEx(hActiveThread, UE_CIP);
    if(argget(*argv, arg1, 0, true))
        if(!valfromstring(arg1, &addr))
            addr = GetContextDataEx(hActiveThread, UE_CIP);
    if(!memisvalidreadptr(fdProcessInfo->hProcess, addr))
        return STATUS_CONTINUE;
    DebugUpdateGui(addr, false);
    return STATUS_CONTINUE;
}

CMDRESULT cbDebugSetMemoryBpx(int argc, char* argv[])
{
    char arg1[deflen] = ""; //addr
    if(!argget(*argv, arg1, 0, false))
        return STATUS_ERROR;
    uint addr;
    if(!valfromstring(arg1, &addr))
        return STATUS_ERROR;
    bool restore = false;
    char arg2[deflen] = ""; //restore
    char arg3[deflen] = ""; //type
    argget(*argv, arg3, 2, true);
    if(argget(*argv, arg2, 1, true))
    {
        if(*arg2 == '1')
            restore = true;
        else if(*arg2 == '0')
            restore = false;
        else
            strcpy(arg3, arg2);
    }
    DWORD type = UE_MEMORY;
    if(*arg3)
    {
        switch(*arg3)
        {
        case 'r':
            type = UE_MEMORY_READ;
            break;
        case 'w':
            type = UE_MEMORY_WRITE;
            break;
        case 'x':
            type = UE_MEMORY_EXECUTE; //EXECUTE
            break;
        default:
            dputs("invalid type (argument ignored)");
            break;
        }
    }
    uint size = 0;
    uint base = memfindbaseaddr(addr, &size, true);
    bool singleshoot = false;
    if(!restore)
        singleshoot = true;
    if(bpget(base, BPMEMORY, 0, 0))
    {
        dputs("hardware breakpoint already set!");
        return STATUS_CONTINUE;
    }
    if(!bpnew(base, true, singleshoot, 0, BPMEMORY, type, 0) or !SetMemoryBPXEx(base, size, type, restore, (void*)cbMemoryBreakpoint))
    {
        dputs("error setting memory breakpoint!");
        return STATUS_ERROR;
    }
    dprintf("memory breakpoint at "fhex" set!\n", addr);
    GuiUpdateAllViews();
    return STATUS_CONTINUE;
}

CMDRESULT cbDebugDeleteMemoryBreakpoint(int argc, char* argv[])
{
    char arg1[deflen] = "";
    if(!argget(*argv, arg1, 0, true)) //delete all breakpoints
    {
        if(!bpgetcount(BPMEMORY))
        {
            dputs("no memory breakpoints to delete!");
            return STATUS_CONTINUE;
        }
        if(!bpenumall(cbDeleteAllMemoryBreakpoints)) //at least one deletion failed
            return STATUS_ERROR;
        dputs("all memory breakpoints deleted!");
        GuiUpdateAllViews();
        return STATUS_CONTINUE;
    }
    BREAKPOINT found;
    if(bpget(0, BPMEMORY, arg1, &found)) //found a breakpoint with name
    {
        uint size;
        memfindbaseaddr(found.addr, &size);
        if(!bpdel(found.addr, BPMEMORY) or !RemoveMemoryBPX(found.addr, size))
        {
            dprintf("delete memory breakpoint failed: "fhex"\n", found.addr);
            return STATUS_ERROR;
        }
        return STATUS_CONTINUE;
    }
    uint addr = 0;
    if(!valfromstring(arg1, &addr) or !bpget(addr, BPMEMORY, 0, &found)) //invalid breakpoint
    {
        dprintf("no such memory breakpoint \"%s\"\n", arg1);
        return STATUS_ERROR;
    }
    uint size;
    memfindbaseaddr(found.addr, &size);
    if(!bpdel(found.addr, BPMEMORY) or !RemoveMemoryBPX(found.addr, size))
    {
        dprintf("delete memory breakpoint failed: "fhex"\n", found.addr);
        return STATUS_ERROR;
    }
    dputs("memory breakpoint deleted!");
    GuiUpdateAllViews();
    return STATUS_CONTINUE;
}

CMDRESULT cbDebugRtr(int argc, char* argv[])
{
    StepOver((void*)cbRtrStep);
    cbDebugRun(argc, argv);
    return STATUS_CONTINUE;
}

CMDRESULT cbDebugeRtr(int argc, char* argv[])
{
    dbgsetskipexceptions(true);
    return cbDebugRtr(argc, argv);
}

CMDRESULT cbDebugSetHardwareBreakpoint(int argc, char* argv[])
{
    char arg1[deflen] = ""; //addr
    if(!argget(*argv, arg1, 0, false))
        return STATUS_ERROR;
    uint addr;
    if(!valfromstring(arg1, &addr))
        return STATUS_ERROR;
    DWORD type = UE_HARDWARE_EXECUTE;
    char arg2[deflen] = ""; //type
    if(argget(*argv, arg2, 1, true))
    {
        switch(*arg2)
        {
        case 'r':
            type = UE_HARDWARE_READWRITE;
            break;
        case 'w':
            type = UE_HARDWARE_WRITE;
            break;
        case 'x':
            break;
        default:
            dputs("invalid type, assuming 'x'");
            break;
        }
    }
    char arg3[deflen] = ""; //size
    uint size = UE_HARDWARE_SIZE_1;
    if(argget(*argv, arg3, 2, true))
    {
        if(!valfromstring(arg3, &size))
            return STATUS_ERROR;
        switch(size)
        {
        case 2:
            size = UE_HARDWARE_SIZE_2;
            break;
        case 4:
            size = UE_HARDWARE_SIZE_4;
            break;
#ifdef _WIN64
        case 8:
            size = UE_HARDWARE_SIZE_8;
            break;
#endif // _WIN64
        default:
            dputs("invalid size, using 1");
            break;
        }
        if((addr % size) != 0)
        {
            dprintf("address not aligned to %d\n", size);
            return STATUS_ERROR;
        }
    }
    DWORD drx = 0;
    if(!GetUnusedHardwareBreakPointRegister(&drx))
    {
        dputs("you can only set 4 hardware breakpoints");
        return STATUS_ERROR;
    }
    int titantype = 0;
    TITANSETDRX(titantype, drx);
    TITANSETTYPE(titantype, type);
    TITANSETSIZE(titantype, size);
    //TODO: hwbp in multiple threads TEST
    if(bpget(addr, BPHARDWARE, 0, 0))
    {
        dputs("hardware breakpoint already set!");
        return STATUS_CONTINUE;
    }
    if(!bpnew(addr, true, false, 0, BPHARDWARE, titantype, 0) or !SetHardwareBreakPoint(addr, drx, type, (DWORD)size, (void*)cbHardwareBreakpoint))
    {
        dputs("error setting hardware breakpoint!");
        return STATUS_ERROR;
    }
    dprintf("hardware breakpoint at "fhex" set!\n", addr);
    GuiUpdateAllViews();
    return STATUS_CONTINUE;
}

CMDRESULT cbDebugDeleteHardwareBreakpoint(int argc, char* argv[])
{
    char arg1[deflen] = "";
    if(!argget(*argv, arg1, 0, true)) //delete all breakpoints
    {
        if(!bpgetcount(BPHARDWARE))
        {
            dputs("no hardware breakpoints to delete!");
            return STATUS_CONTINUE;
        }
        if(!bpenumall(cbDeleteAllHardwareBreakpoints)) //at least one deletion failed
            return STATUS_ERROR;
        dputs("all hardware breakpoints deleted!");
        GuiUpdateAllViews();
        return STATUS_CONTINUE;
    }
    BREAKPOINT found;
    if(bpget(0, BPHARDWARE, arg1, &found)) //found a breakpoint with name
    {
        if(!bpdel(found.addr, BPHARDWARE) or !DeleteHardwareBreakPoint(TITANGETDRX(found.titantype)))
        {
            dprintf("delete hardware breakpoint failed: "fhex"\n", found.addr);
            return STATUS_ERROR;
        }
        return STATUS_CONTINUE;
    }
    uint addr = 0;
    if(!valfromstring(arg1, &addr) or !bpget(addr, BPHARDWARE, 0, &found)) //invalid breakpoint
    {
        dprintf("no such hardware breakpoint \"%s\"\n", arg1);
        return STATUS_ERROR;
    }
    if(!bpdel(found.addr, BPHARDWARE) or !DeleteHardwareBreakPoint(TITANGETDRX(found.titantype)))
    {
        dprintf("delete hardware breakpoint failed: "fhex"\n", found.addr);
        return STATUS_ERROR;
    }
    dputs("hardware breakpoint deleted!");
    GuiUpdateAllViews();
    return STATUS_CONTINUE;
}

CMDRESULT cbDebugAlloc(int argc, char* argv[])
{
    char arg1[deflen] = ""; //size
    uint size = 0x1000;
    if(argget(*argv, arg1, 0, true))
        if(!valfromstring(arg1, &size, false))
            return STATUS_ERROR;
    uint mem = (uint)memalloc(fdProcessInfo->hProcess, 0, size, PAGE_EXECUTE_READWRITE);
    if(!mem)
        dputs("VirtualAllocEx failed");
    else
        dprintf(fhex"\n", mem);
    if(mem)
        varset("$lastalloc", mem, true);
    dbggetprivateusage(fdProcessInfo->hProcess, true);
    memupdatemap(fdProcessInfo->hProcess);
    varset("$res", mem, false);
    return STATUS_CONTINUE;
}

CMDRESULT cbDebugFree(int argc, char* argv[])
{
    uint lastalloc;
    varget("$lastalloc", &lastalloc, 0, 0);
    char arg1[deflen] = ""; //addr
    uint addr = lastalloc;
    if(argget(*argv, arg1, 0, true))
    {
        if(!valfromstring(arg1, &addr, false))
            return STATUS_ERROR;
    }
    else if(!lastalloc)
    {
        dputs("lastalloc is zero, provide a page address");
        return STATUS_ERROR;
    }
    if(addr == lastalloc)
        varset("$lastalloc", (uint)0, true);
    bool ok = !!VirtualFreeEx(fdProcessInfo->hProcess, (void*)addr, 0, MEM_RELEASE);
    if(!ok)
        dputs("VirtualFreeEx failed");
    dbggetprivateusage(fdProcessInfo->hProcess, true);
    memupdatemap(fdProcessInfo->hProcess);
    varset("$res", ok, false);
    return STATUS_CONTINUE;
}

CMDRESULT cbDebugMemset(int argc, char* argv[])
{
    char arg3[deflen] = ""; //size
    uint addr;
    uint value;
    uint size;
    if(argc < 3)
    {
        dputs("not enough arguments");
        return STATUS_ERROR;
    }
    if(!valfromstring(argv[1], &addr, false) or !valfromstring(argv[2], &value, false))
        return STATUS_ERROR;
    if(argget(*argv, arg3, 2, true))
    {
        if(!valfromstring(arg3, &size, false))
            return STATUS_ERROR;
    }
    else
    {
        uint base = memfindbaseaddr(addr, &size, true);
        if(!base)
        {
            dputs("invalid address specified");
            return STATUS_ERROR;
        }
        uint diff = addr - base;
        addr = base + diff;
        size -= diff;
    }
    BYTE fi = value & 0xFF;
    if(!Fill((void*)addr, size & 0xFFFFFFFF, &fi))
        dputs("memset failed");
    else
        dprintf("memory "fhex" (size: %.8X) set to %.2X\n", addr, size & 0xFFFFFFFF, value & 0xFF);
    return STATUS_CONTINUE;
}

CMDRESULT cbDebugBenchmark(int argc, char* argv[])
{
    uint addr = memfindbaseaddr(GetContextDataEx(hActiveThread, UE_CIP), 0);
    DWORD ticks = GetTickCount();
    char comment[MAX_COMMENT_SIZE] = "";
    for(uint i = addr; i < addr + 100000; i++)
    {
        commentset(i, "test", false);
        labelset(i, "test", false);
        bookmarkset(i, false);
        functionadd(i, i, false);
    }
    dprintf("%ums\n", GetTickCount() - ticks);
    return STATUS_CONTINUE;
}

CMDRESULT cbDebugPause(int argc, char* argv[])
{
    if(waitislocked(WAITID_RUN))
    {
        dputs("program is not running");
        return STATUS_ERROR;
    }
    dbgsetispausedbyuser(true);
    DebugBreakProcess(fdProcessInfo->hProcess);
    return STATUS_CONTINUE;
}

static DWORD WINAPI scyllaThread(void* lpParam)
{
    typedef INT (WINAPI * SCYLLASTARTGUI)(DWORD pid, HINSTANCE mod);
    SCYLLASTARTGUI ScyllaStartGui = 0;
    HINSTANCE hScylla = LoadLibraryA("Scylla.dll");
    if(!hScylla)
    {
        dputs("error loading Scylla.dll!");
        bScyllaLoaded = false;
        return 0;
    }
    ScyllaStartGui = (SCYLLASTARTGUI)GetProcAddress(hScylla, "ScyllaStartGui");
    if(!ScyllaStartGui)
    {
        dputs("could not find export 'ScyllaStartGui' inside Scylla.dll");
        bScyllaLoaded = false;
        return 0;
    }
    if(dbgisdll())
        ScyllaStartGui(fdProcessInfo->dwProcessId, (HINSTANCE)dbgdebuggedbase());
    else
        ScyllaStartGui(fdProcessInfo->dwProcessId, 0);
    FreeLibrary(hScylla);
    bScyllaLoaded = false;
    return 0;
}

CMDRESULT cbDebugStartScylla(int argc, char* argv[])
{
    if(bScyllaLoaded)
    {
        dputs("Scylla is already loaded");
        return STATUS_ERROR;
    }
    bScyllaLoaded = true;
    CloseHandle(CreateThread(0, 0, scyllaThread, 0, 0, 0));
    return STATUS_CONTINUE;
}

CMDRESULT cbDebugAttach(int argc, char* argv[])
{
    if(argc < 2)
    {
        dputs("not enough arguments!");
        return STATUS_ERROR;
    }
    uint pid = 0;
    if(!valfromstring(argv[1], &pid))
    {
        dprintf("invalid expression \"%s\"!\n", argv[1]);
        return STATUS_ERROR;
    }
    if(DbgIsDebugging())
    {
        //TODO: do stuff
        dputs("terminate the current session!");
        return STATUS_ERROR;
    }
    HANDLE hProcess = TitanOpenProcess(PROCESS_ALL_ACCESS, false, (DWORD)pid);
    if(!hProcess)
    {
        dprintf("could not open process %X!\n", pid);
        return STATUS_ERROR;
    }
    BOOL wow64 = false, mewow64 = false;
    if(!IsWow64Process(hProcess, &wow64) or !IsWow64Process(GetCurrentProcess(), &mewow64))
    {
        dputs("IsWow64Process failed!");
        CloseHandle(hProcess);
        return STATUS_ERROR;
    }
    if((mewow64 and !wow64) or (!mewow64 and wow64))
    {
#ifdef _WIN64
        dputs("Use x32_dbg to debug this process!");
#else
        dputs("Use x64_dbg to debug this process!");
#endif // _WIN64
        CloseHandle(hProcess);
        return STATUS_ERROR;
    }
    if(!GetModuleFileNameExA(hProcess, 0, szFileName, sizeof(szFileName)))
    {
        dprintf("could not get module filename %X!\n", pid);
        CloseHandle(hProcess);
        return STATUS_ERROR;
    }
    CloseHandle(hProcess);
    CloseHandle(CreateThread(0, 0, threadAttachLoop, (void*)pid, 0, 0));
    return STATUS_CONTINUE;
}

CMDRESULT cbDebugDetach(int argc, char* argv[])
{
    unlock(WAITID_RUN); //run
    dbgsetisdetachedbyuser(true); //detach when paused
    StepInto((void*)cbDetach);
    DebugBreakProcess(fdProcessInfo->hProcess);
    return STATUS_CONTINUE;
}

CMDRESULT cbDebugDump(int argc, char* argv[])
{
    if(argc < 2)
    {
        dputs("not enough arguments!");
        return STATUS_ERROR;
    }
    duint addr = 0;
    if(!valfromstring(argv[1], &addr))
    {
        dprintf("invalid address \"%s\"!\n", argv[1]);
        return STATUS_ERROR;
    }
    GuiDumpAt(addr);
    return STATUS_CONTINUE;
}

CMDRESULT cbDebugStackDump(int argc, char* argv[])
{
    duint addr = 0;
    if(argc < 2)
        addr = GetContextDataEx(hActiveThread, UE_CSP);
    else if(!valfromstring(argv[1], &addr))
    {
        dprintf("invalid address \"%s\"!\n", argv[1]);
        return STATUS_ERROR;
    }
    duint csp = GetContextDataEx(hActiveThread, UE_CSP);
    duint size = 0;
    duint base = memfindbaseaddr(csp, &size);
    if(base && addr >= base && addr < (base + size))
        GuiStackDumpAt(addr, csp);
    else
        dputs("invalid stack address!");
    return STATUS_CONTINUE;
}

CMDRESULT cbDebugContinue(int argc, char* argv[])
{
    if(argc < 2)
    {
        SetNextDbgContinueStatus(DBG_CONTINUE);
        dputs("exception will be swallowed");
    }
    else
    {
        SetNextDbgContinueStatus(DBG_EXCEPTION_NOT_HANDLED);
        dputs("exception will be thrown in the program");
    }
    return STATUS_CONTINUE;
}

CMDRESULT cbDebugBpDll(int argc, char* argv[])
{
    if(argc < 2)
    {
        dputs("not enough arguments!");
        return STATUS_ERROR;
    }
    DWORD type = UE_ON_LIB_ALL;
    if(argc > 2)
    {
        switch(*argv[2])
        {
        case 'l':
            type = UE_ON_LIB_LOAD;
            break;
        case 'u':
            type = UE_ON_LIB_UNLOAD;
            break;
        }
    }
    bool singleshoot = true;
    if(argc > 3)
        singleshoot = false;
    LibrarianSetBreakPoint(argv[1], type, singleshoot, (void*)cbLibrarianBreakpoint);
    dprintf("dll breakpoint set on \"%s\"!\n", argv[1]);
    return STATUS_CONTINUE;
}

CMDRESULT cbDebugBcDll(int argc, char* argv[])
{
    if(argc < 2)
    {
        dputs("not enough arguments");
        return STATUS_ERROR;
    }
    if(!LibrarianRemoveBreakPoint(argv[1], UE_ON_LIB_ALL))
    {
        dputs("failed to remove dll breakpoint...");
        return STATUS_ERROR;
    }
    dputs("dll breakpoint removed!");
    return STATUS_CONTINUE;
}

CMDRESULT cbDebugSwitchthread(int argc, char* argv[])
{
    uint threadid = fdProcessInfo->dwThreadId; //main thread
    if(argc > 1)
        if(!valfromstring(argv[1], &threadid, false))
            return STATUS_ERROR;
    if(!threadisvalid((DWORD)threadid)) //check if the thread is valid
    {
        dprintf("invalid thread %X\n", threadid);
        return STATUS_ERROR;
    }
    //switch thread
    hActiveThread = threadgethandle((DWORD)threadid);
    DebugUpdateGui(GetContextDataEx(hActiveThread, UE_CIP), true);
    dputs("thread switched!");
    return STATUS_CONTINUE;
}

CMDRESULT cbDebugSuspendthread(int argc, char* argv[])
{
    uint threadid = fdProcessInfo->dwThreadId;
    if(argc > 1)
        if(!valfromstring(argv[1], &threadid, false))
            return STATUS_ERROR;
    if(!threadisvalid((DWORD)threadid)) //check if the thread is valid
    {
        dprintf("invalid thread %X\n", threadid);
        return STATUS_ERROR;
    }
    //suspend thread
    if(SuspendThread(threadgethandle((DWORD)threadid)) == -1)
    {
        dputs("error suspending thread");
        return STATUS_ERROR;
    }
    dputs("thread suspended");
    GuiUpdateAllViews();
    return STATUS_CONTINUE;
}

CMDRESULT cbDebugResumethread(int argc, char* argv[])
{
    uint threadid = fdProcessInfo->dwThreadId;
    if(argc > 1)
        if(!valfromstring(argv[1], &threadid, false))
            return STATUS_ERROR;
    if(!threadisvalid((DWORD)threadid)) //check if the thread is valid
    {
        dprintf("invalid thread %X\n", threadid);
        return STATUS_ERROR;
    }
    //resume thread
    if(ResumeThread(threadgethandle((DWORD)threadid)) == -1)
    {
        dputs("error resuming thread");
        return STATUS_ERROR;
    }
    dputs("thread resumed!");
    GuiUpdateAllViews();
    return STATUS_CONTINUE;
}

CMDRESULT cbDebugKillthread(int argc, char* argv[])
{
    uint threadid = fdProcessInfo->dwThreadId;
    if(argc > 1)
        if(!valfromstring(argv[1], &threadid, false))
            return STATUS_ERROR;
    uint exitcode = 0;
    if(argc > 2)
        if(!valfromstring(argv[2], &exitcode, false))
            return STATUS_ERROR;
    if(!threadisvalid((DWORD)threadid)) //check if the thread is valid
    {
        dprintf("invalid thread %X\n", threadid);
        return STATUS_ERROR;
    }
    //terminate thread
    if(TerminateThread(threadgethandle((DWORD)threadid), (DWORD)exitcode) != 0)
    {
        GuiUpdateAllViews();
        dputs("thread terminated");
        return STATUS_CONTINUE;
    }
    dputs("error terminating thread!");
    return STATUS_ERROR;
}

CMDRESULT cbDebugSetPriority(int argc, char* argv[])
{
    if(argc < 3)
    {
        dputs("not enough arguments!");
        return STATUS_ERROR;
    }
    uint threadid;
    if(!valfromstring(argv[1], &threadid, false))
        return STATUS_ERROR;
    uint priority;
    if(!valfromstring(argv[2], &priority))
    {
        if(_strcmpi(argv[2], "Normal") == 0)
            priority = THREAD_PRIORITY_NORMAL;
        else if(_strcmpi(argv[2], "AboveNormal") == 0)
            priority = THREAD_PRIORITY_ABOVE_NORMAL;
        else if(_strcmpi(argv[2], "TimeCritical") == 0)
            priority = THREAD_PRIORITY_TIME_CRITICAL;
        else if(_strcmpi(argv[2], "Idle") == 0)
            priority = THREAD_PRIORITY_IDLE;
        else if(_strcmpi(argv[2], "BelowNormal") == 0)
            priority = THREAD_PRIORITY_BELOW_NORMAL;
        else if(_strcmpi(argv[2], "Highest") == 0)
            priority = THREAD_PRIORITY_HIGHEST;
        else if(_strcmpi(argv[2], "Lowest") == 0)
            priority = THREAD_PRIORITY_LOWEST;
        else
        {
            dputs("unknown priority value, read the help!");
            return STATUS_ERROR;
        }
    }
    else
    {
        switch(priority) //check if the priority value is valid
        {
        case THREAD_PRIORITY_NORMAL:
        case THREAD_PRIORITY_ABOVE_NORMAL:
        case THREAD_PRIORITY_TIME_CRITICAL:
        case THREAD_PRIORITY_IDLE:
        case THREAD_PRIORITY_BELOW_NORMAL:
        case THREAD_PRIORITY_HIGHEST:
        case THREAD_PRIORITY_LOWEST:
            break;
        default:
            dputs("unknown priority value, read the help!");
            return STATUS_ERROR;
        }
    }
    if(!threadisvalid((DWORD)threadid)) //check if the thread is valid
    {
        dprintf("invalid thread %X\n", threadid);
        return STATUS_ERROR;
    }
    //set thread priority
    if(SetThreadPriority(threadgethandle((DWORD)threadid), (int)priority) == 0)
    {
        dputs("error setting thread priority");
        return STATUS_ERROR;
    }
    dputs("thread priority changed!");
    GuiUpdateAllViews();
    return STATUS_CONTINUE;
}

CMDRESULT cbDebugEnableHardwareBreakpoint(int argc, char* argv[])
{
    char arg1[deflen] = "";
    DWORD drx = 0;
    if(!GetUnusedHardwareBreakPointRegister(&drx))
    {
        dputs("you can only set 4 hardware breakpoints");
        return STATUS_ERROR;
    }
    if(!argget(*argv, arg1, 0, true)) //enable all hardware breakpoints
    {
        if(!bpgetcount(BPHARDWARE))
        {
            dputs("no hardware breakpoints to enable!");
            return STATUS_CONTINUE;
        }
        if(!bpenumall(cbEnableAllHardwareBreakpoints)) //at least one enable failed
            return STATUS_ERROR;
        dputs("all hardware breakpoints enabled!");
        GuiUpdateAllViews();
        return STATUS_CONTINUE;
    }
    BREAKPOINT found;
    uint addr = 0;
    if(!valfromstring(arg1, &addr) or !bpget(addr, BPHARDWARE, 0, &found)) //invalid hardware breakpoint
    {
        dprintf("no such hardware breakpoint \"%s\"\n", arg1);
        return STATUS_ERROR;
    }
    if(found.enabled)
    {
        dputs("hardware breakpoint already enabled!");
        GuiUpdateAllViews();
        return STATUS_CONTINUE;
    }
    TITANSETDRX(found.titantype, drx);
    bpsettitantype(found.addr, BPHARDWARE, found.titantype);
    if(!bpenable(found.addr, BPHARDWARE, true) or !SetHardwareBreakPoint(found.addr, drx, TITANGETTYPE(found.titantype), TITANGETSIZE(found.titantype), (void*)cbHardwareBreakpoint))
    {
        dprintf("could not enable hardware breakpoint "fhex"\n", found.addr);
        return STATUS_ERROR;
    }
    dputs("hardware breakpoint enabled!");
    GuiUpdateAllViews();
    return STATUS_CONTINUE;
}

CMDRESULT cbDebugDisableHardwareBreakpoint(int argc, char* argv[])
{
    char arg1[deflen] = "";
    if(!argget(*argv, arg1, 0, true)) //delete all hardware breakpoints
    {
        if(!bpgetcount(BPHARDWARE))
        {
            dputs("no hardware breakpoints to disable!");
            return STATUS_CONTINUE;
        }
        if(!bpenumall(cbDisableAllHardwareBreakpoints)) //at least one deletion failed
            return STATUS_ERROR;
        dputs("all hardware breakpoints disabled!");
        GuiUpdateAllViews();
        return STATUS_CONTINUE;
    }
    BREAKPOINT found;
    uint addr = 0;
    if(!valfromstring(arg1, &addr) or !bpget(addr, BPHARDWARE, 0, &found)) //invalid hardware breakpoint
    {
        dprintf("no such hardware breakpoint \"%s\"\n", arg1);
        return STATUS_ERROR;
    }
    if(!found.enabled)
    {
        dputs("hardware breakpoint already disabled!");
        return STATUS_CONTINUE;
    }
    if(!bpenable(found.addr, BPHARDWARE, false) or !DeleteHardwareBreakPoint(TITANGETDRX(found.titantype)))
    {
        dprintf("could not disable hardware breakpoint "fhex"\n", found.addr);
        return STATUS_ERROR;
    }
    dputs("hardware breakpoint disabled!");
    GuiUpdateAllViews();
    return STATUS_CONTINUE;
}

CMDRESULT cbDebugEnableMemoryBreakpoint(int argc, char* argv[])
{
    char arg1[deflen] = "";
    DWORD drx = 0;
    if(!GetUnusedHardwareBreakPointRegister(0))
    {
        dputs("you can only set 4 hardware breakpoints");
        return STATUS_ERROR;
    }
    if(!argget(*argv, arg1, 0, true)) //enable all memory breakpoints
    {
        if(!bpgetcount(BPMEMORY))
        {
            dputs("no hardware breakpoints to enable!");
            return STATUS_CONTINUE;
        }
        if(!bpenumall(cbEnableAllHardwareBreakpoints)) //at least one enable failed
            return STATUS_ERROR;
        dputs("all memory breakpoints enabled!");
        GuiUpdateAllViews();
        return STATUS_CONTINUE;
    }
    BREAKPOINT found;
    uint addr = 0;
    if(!valfromstring(arg1, &addr) or !bpget(addr, BPMEMORY, 0, &found)) //invalid memory breakpoint
    {
        dprintf("no such memory breakpoint \"%s\"\n", arg1);
        return STATUS_ERROR;
    }
    if(found.enabled)
    {
        dputs("hardware memory already enabled!");
        GuiUpdateAllViews();
        return STATUS_CONTINUE;
    }
    uint size = 0;
    memfindbaseaddr(found.addr, &size);
    if(!bpenable(found.addr, BPMEMORY, true) or !SetMemoryBPXEx(found.addr, size, found.titantype, !found.singleshoot, (void*)cbMemoryBreakpoint))
    {
        dprintf("could not enable memory breakpoint "fhex"\n", found.addr);
        return STATUS_ERROR;
    }
    dputs("memory breakpoint enabled!");
    GuiUpdateAllViews();
    return STATUS_CONTINUE;
}

CMDRESULT cbDebugDisableMemoryBreakpoint(int argc, char* argv[])
{
    char arg1[deflen] = "";
    if(!argget(*argv, arg1, 0, true)) //delete all memory breakpoints
    {
        if(!bpgetcount(BPMEMORY))
        {
            dputs("no memory breakpoints to disable!");
            return STATUS_CONTINUE;
        }
        if(!bpenumall(cbDisableAllMemoryBreakpoints)) //at least one deletion failed
            return STATUS_ERROR;
        dputs("all memory breakpoints disabled!");
        GuiUpdateAllViews();
        return STATUS_CONTINUE;
    }
    BREAKPOINT found;
    uint addr = 0;
    if(!valfromstring(arg1, &addr) or !bpget(addr, BPMEMORY, 0, &found)) //invalid memory breakpoint
    {
        dprintf("no such memory breakpoint \"%s\"\n", arg1);
        return STATUS_ERROR;
    }
    if(!found.enabled)
    {
        dputs("memory breakpoint already disabled!");
        return STATUS_CONTINUE;
    }
    uint size = 0;
    memfindbaseaddr(found.addr, &size);
    if(!bpenable(found.addr, BPMEMORY, false) or !RemoveMemoryBPX(found.addr, size))
    {
        dprintf("could not disable memory breakpoint "fhex"\n", found.addr);
        return STATUS_ERROR;
    }
    dputs("memory breakpoint disabled!");
    GuiUpdateAllViews();
    return STATUS_CONTINUE;
}

CMDRESULT cbDebugDownloadSymbol(int argc, char* argv[])
{
    char szDefaultStore[MAX_PATH] = "";
    const char* szSymbolStore = szDefaultStore;
    if(!BridgeSettingGet("Symbols", "DefaultStore", szDefaultStore)) //get default symbol store from settings
    {
        strcpy(szDefaultStore, "http://msdl.microsoft.com/download/symbols");
        BridgeSettingSet("Symbols", "DefaultStore", szDefaultStore);
    }
    if(argc < 2) //no arguments
    {
        symdownloadallsymbols(szSymbolStore); //download symbols for all modules
        GuiSymbolRefreshCurrent();
        dputs("done! See symbol log for more information");
        return STATUS_CONTINUE;
    }
    //get some module information
    uint modbase = modbasefromname(argv[1]);
    if(!modbase)
    {
        dprintf("invalid module \"%s\"!\n", argv[1]);
        return STATUS_ERROR;
    }
    char szModulePath[MAX_PATH] = "";
    if(!GetModuleFileNameExA(fdProcessInfo->hProcess, (HMODULE)modbase, szModulePath, MAX_PATH))
    {
        dputs("GetModuleFileNameExA failed!");
        return STATUS_ERROR;
    }
    char szOldSearchPath[MAX_PATH] = "";
    if(!SymGetSearchPath(fdProcessInfo->hProcess, szOldSearchPath, MAX_PATH)) //backup current search path
    {
        dputs("SymGetSearchPath failed!");
        return STATUS_ERROR;
    }
    char szServerSearchPath[MAX_PATH * 2] = "";
    if(argc > 2)
        szSymbolStore = argv[2];
    sprintf_s(szServerSearchPath, "SRV*%s*%s", szSymbolCachePath, szSymbolStore);
    if(!SymSetSearchPath(fdProcessInfo->hProcess, szServerSearchPath)) //set new search path
    {
        dputs("SymSetSearchPath (1) failed!");
        return STATUS_ERROR;
    }
    if(!SymUnloadModule64(fdProcessInfo->hProcess, (DWORD64)modbase)) //unload module
    {
        SymSetSearchPath(fdProcessInfo->hProcess, szOldSearchPath);
        dputs("SymUnloadModule64 failed!");
        return STATUS_ERROR;
    }
    if(!SymLoadModuleEx(fdProcessInfo->hProcess, 0, szModulePath, 0, (DWORD64)modbase, 0, 0, 0)) //load module
    {
        dputs("SymLoadModuleEx failed!");
        SymSetSearchPath(fdProcessInfo->hProcess, szOldSearchPath);
        return STATUS_ERROR;
    }
    if(!SymSetSearchPath(fdProcessInfo->hProcess, szOldSearchPath))
    {
        dputs("SymSetSearchPath (2) failed!");
        return STATUS_ERROR;
    }
    GuiSymbolRefreshCurrent();
    dputs("done! See symbol log for more information");
    return STATUS_CONTINUE;
}

CMDRESULT cbDebugSetJIT(int argc, char* argv[])
{
    arch actual_arch;
    char * jit_debugger_cmd;
    if(argc < 2)
    {
        char path[JIT_ENTRY_DEF_SIZE];
        dbggetdefjit(path);

        jit_debugger_cmd = path;
        if (!dbgsetjit( jit_debugger_cmd, notfound, & actual_arch ))
        {
            dprintf( "Error setting JIT %s\n", (actual_arch == x64) ? "x64" : "x32" );
            return STATUS_ERROR;
        }
    }
    else if ( argc == 2 )
    {
        jit_debugger_cmd = argv[1];
        if (!dbgsetjit( jit_debugger_cmd, notfound, & actual_arch ))
        {
            dprintf( "Error setting JIT %s\n", (actual_arch == x64) ? "x64" : "x32" );
            return STATUS_ERROR;
        }
    }
    else if ( argc == 3 )
    {
        actual_arch = x64;

        if ( _strcmpi( argv[1], "x64" ) == 0 )
        {
            if (!IsWow64())
            {
                dprintf( "Error using x64 arg the debugger is not a WOW64 process", (actual_arch == x64) ? "x64" : "x32" );
                return STATUS_ERROR;
            }
        }
        else if ( _strcmpi( argv[1], "x32" ) == 0 )
            actual_arch = x32;
        else
        {
            dputs( "Unkown jit entry type use x64 or x32 parameter");
            return STATUS_ERROR;
        }
        
        jit_debugger_cmd = argv[2];
        if (!dbgsetjit( jit_debugger_cmd, actual_arch, NULL))
        {
            dprintf( "Error getting JIT %s\n", (actual_arch == x64) ? "x64" : "x32" );
            return STATUS_ERROR;
        }
    }
    else
    {
        dputs( "Error unkown parameters use x86 or x64, cmdline" );
        return STATUS_ERROR;
    }

    dprintf( " New JIT %s: %s\n", (actual_arch == x64) ? "x64" : "x32", jit_debugger_cmd );

    return STATUS_CONTINUE;
}

CMDRESULT cbDebugGetJIT(int argc, char* argv[])
{
    char * get_entry = NULL;
    arch actual_arch;

    if(argc < 2)
    {
        if (!dbggetjit( & get_entry, notfound, & actual_arch ))
        {
            dprintf( "Error getting JIT %s\n", (actual_arch == x64) ? "x64" : "x32" );
            return STATUS_ERROR;
        }
    }
    else
    {
        if ( _strcmpi( argv[1], "x64" ) == 0 )
        {
            actual_arch = x64;

            if (!IsWow64())
            {
                dprintf( "Error using x64 arg the debugger is not a WOW64 process", (actual_arch == x64) ? "x64" : "x32" );
                return STATUS_ERROR;
            }
        }
        else if ( _strcmpi( argv[1], "x32" ) == 0 )
            actual_arch = x32;
        else
        {
            dputs( "Unkown jit entry type use x64 or x32 parameter");
            return STATUS_ERROR;
        }

        if (!dbggetjit( & get_entry, actual_arch, NULL ))
        {
            dprintf( "Error getting JIT %s\n", argv[1] );
            return STATUS_ERROR;
        }
    }

    dprintf( " JIT %s: %s\n", (actual_arch == x64) ? "x64" : "x32", get_entry );
    if ( get_entry != NULL )
        efree(get_entry);
    
    return STATUS_CONTINUE;
}