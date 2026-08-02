#ifdef CPU_TRACE

static const char* g_psx_cpu_a_kcall_symtable[] = {
    "open(filename=%08x,accessmode=%08x)",
    "lseek(fd=%08x,offset=%08x,seektype=%08x)",
    "read(fd=%08x,dst=%08x,length=%08x)",
    "write(fd=%08x,src=%08x,length=%08x)",
    "close(fd=%08x)",
    "ioctl(fd=%08x,cmd=%08x,arg=%08x)",
    "exit(exitcode=%08x)",
    "isatty(fd=%08x)",
    "getc(fd=%08x)",
    "putc(char=%08x,fd=%08x)",
    "todigit(char=%08x)",
    "atof(src=%08x)",
    "strtoul(src=%08x,src_end=%08x,base=%08x)",
    "strtol(src=%08x,src_end=%08x,base=%08x)",
    "abs(val=%08x)",
    "labs(val=%08x)",
    "atoi(src=%08x)",
    "atol(src=%08x)",
    "atob(src=%08x,num_dst=%08x)",
    "setjmp(buf=%08x)",
    "longjmp(buf=%08x,param=%08x)",
    "strcat(dst=%08x,src=%08x)",
    "strncat(dst=%08x,src=%08x,maxlen=%08x)",
    "strcmp(str1=%08x,str2=%08x)",
    "strncmp(str1=%08x,str2=%08x,maxlen=%08x)",
    "strcpy(dst=%08x,src=%08x)",
    "strncpy(dst=%08x,src=%08x,maxlen=%08x)",
    "strlen(src=%08x)",
    "index(src=%08x,char=%08x)",
    "rindex(src=%08x,char=%08x)",
    "strchr(src=%08x,char=%08x)",
    "strrchr(src=%08x,char=%08x)",
    "strpbrk(src=%08x,list=%08x)",
    "strspn(src=%08x,list=%08x)",
    "strcspn(src=%08x,list=%08x)",
    "strtok(src=%08x,list=%08x)",
    "strstr(str=%08x,substr=%08x)",
    "toupper(char=%08x)",
    "tolower(char=%08x)",
    "bcopy(src=%08x,dst=%08x,len=%08x)",
    "bzero(dst=%08x,len=%08x)",
    "bcmp(ptr1=%08x,ptr2=%08x,len=%08x)",
    "memcpy(dst=%08x,src=%08x,len=%08x)",
    "memset(dst=%08x,fillbyte=%08x,len=%08x)",
    "memmove(dst=%08x,src=%08x,len=%08x)",
    "memcmp(src1=%08x,src2=%08x,len=%08x)",
    "memchr(src=%08x,scanbyte=%08x,len=%08x)",
    "rand()",
    "srand(seed=%08x)",
    "qsort(base=%08x,nel=%08x,width=%08x,callback=%08x)",
    "strtod(src=%08x,src_end=%08x)",
    "malloc(size=%08x)",
    "free(buf=%08x)",
    "lsearch(key=%08x,base=%08x,nel=%08x,width=%08x,callback=%08x)",
    "bsearch(key=%08x,base=%08x,nel=%08x,width=%08x,callback=%08x)",
    "calloc(sizx=%08x,sizy=%08x)",
    "realloc(old_buf=%08x,new_siz=%08x)",
    "InitHeap(addr=%08x,size=%08x)",
    "_exit(exitcode=%08x)",
    "getchar()",
    "putchar(char=%08x)",
    "gets(dst=%08x)",
    "puts(src=%08x)",
    "printf(txt=%08x,param1=%08x,param2=%08x,etc.=%08x)",
    "SystemErrorUnresolvedException()",
    "LoadTest(filename=%08x,headerbuf=%08x)",
    "Load(filename=%08x,headerbuf=%08x)",
    "Exec(headerbuf=%08x,param1=%08x,param2=%08x)",
    "FlushCache()",
    "init_a0_b0_c0_vectors()",
    "GPU_dw(Xdst=%08x,Ydst=%08x,Xsiz=%08x,Ysiz=%08x,src=%08x)",
    "gpu_send_dma(Xdst=%08x,Ydst=%08x,Xsiz=%08x,Ysiz=%08x,src=%08x)",
    "SendGP1Command(gp1cmd=%08x)",
    "GPU_cw(gp0cmd=%08x)",
    "GPU_cwp(src=%08x,num=%08x)",
    "send_gpu_linked_list(src=%08x)",
    "gpu_abort_dma()",
    "GetGPUStatus()",
    "gpu_sync()",
    "SystemError()",
    "SystemError()",
    "LoadExec(filename=%08x,stackbase=%08x,stackoffset=%08x)",
    "GetSysSp()",
    "SystemError()",
    "_96_init()",
    "_bu_init()",
    "_96_remove()",
    "return 0",
    "return 0",
    "return 0",
    "return 0",
    "dev_tty_init()",
    "dev_tty_open(fcb=%08x, (unused)path=%08x,accessmode=%08x)",
    "dev_tty_in_out(fcb=%08x,cmd=%08x)",
    "dev_tty_ioctl(fcb=%08x,cmd=%08x,arg=%08x)",
    "dev_cd_open(fcb=%08x,path=%08x,accessmode=%08x)",
    "dev_cd_read(fcb=%08x,dst=%08x,len=%08x)",
    "dev_cd_close(fcb=%08x)",
    "dev_cd_firstfile(fcb=%08x,path=%08x,direntry=%08x)",
    "dev_cd_nextfile(fcb=%08x,direntry=%08x)",
    "dev_cd_chdir(fcb=%08x,path=%08x)",
    "dev_card_open(fcb=%08x,path=%08x,accessmode=%08x)",
    "dev_card_read(fcb=%08x,dst=%08x,len=%08x)",
    "dev_card_write(fcb=%08x,src=%08x,len=%08x)",
    "dev_card_close(fcb=%08x)",
    "dev_card_firstfile(fcb=%08x,path=%08x,direntry=%08x)",
    "dev_card_nextfile(fcb=%08x,direntry=%08x)",
    "dev_card_erase(fcb=%08x,path=%08x)",
    "dev_card_undelete(fcb=%08x,path=%08x)",
    "dev_card_format(fcb=%08x)",
    "dev_card_rename(fcb1=%08x,path=%08x)",
    "card_clear_error(fcb=%08x) (?)",
    "_bu_init()",
    "_96_init()",
    "_96_remove()",
    "return 0",
    "return 0",
    "return 0",
    "return 0",
    "return 0",
    "CdAsyncSeekL(src=%08x)",
    "return 0",
    "return 0",
    "return 0",
    "CdAsyncGetStatus(dst=%08x)",
    "return 0",
    "CdAsyncReadSector(count=%08x,dst=%08x,mode=%08x)",
    "return 0",
    "return 0",
    "CdAsyncSetMode(mode=%08x)",
    "return 0",
    "return 0",
    "return 0",
    "return 0",
    "return 0",
    "return 0",
    "return 0",
    "return 0",
    "return 0",
    "return 0",
    "return 0",
    "return 0",
    "return 0",
    "return 0",
    "CdromIoIrqFunc1()",
    "CdromDmaIrqFunc1()",
    "CdromIoIrqFunc2()",
    "CdromDmaIrqFunc2()",
    "CdromGetInt5errCode(dst1=%08x,dst2=%08x)",
    "CdInitSubFunc()",
    "AddCDROMDevice()",
    "AddMemCardDevice()",
    "AddDuartTtyDevice()",
    "add_nullcon_driver()",
    "SystemError()",
    "SystemError()",
    "SetConf(num_EvCB=%08x,num_TCB=%08x,stacktop=%08x)",
    "GetConf(num_EvCB_dst=%08x,num_TCB_dst=%08x,stacktop_dst=%08x)",
    "SetCdromIrqAutoAbort(type=%08x,flag=%08x)",
    "SetMem(megabytes=%08x)",
    "_boot()",
    "SystemError(type=%08x,errorcode=%08x)",
    "EnqueueCdIntr()",
    "DequeueCdIntr()",
    "CdGetLbn(filename=%08x)",
    "CdReadSector(count=%08x,sector=%08x,buffer=%08x)",
    "CdGetStatus()",
    "bufs_cb_0()",
    "bufs_cb_1()",
    "bufs_cb_2()",
    "bufs_cb_3()",
    "_card_info(port=%08x)",
    "_card_load(port=%08x)",
    "_card_auto(flag=%08x)",
    "bufs_cb_4()",
    "card_write_test(port=%08x)",
    "return 0",
    "return 0",
    "ioabort_raw(param=%08x)",
    "return 0",
    "GetSystemInfo(index=%08x)"
};

static const char* g_psx_cpu_b_kcall_symtable[] = {
    "alloc_kernel_memory(size=%08x)",
    "free_kernel_memory(buf=%08x)",
    "init_timer(t=%08x,reload=%08x,flags=%08x)",
    "get_timer(t=%08x)",
    "enable_timer_irq(t=%08x)",
    "disable_timer_irq(t=%08x)",
    "restart_timer(t=%08x)",
    "DeliverEvent(class=%08x, spec=%08x)",
    "OpenEvent(class=%08x,spec=%08x,mode=%08x,func=%08x)",
    "CloseEvent(event=%08x)",
    "WaitEvent(event=%08x)",
    "TestEvent(event=%08x)",
    "EnableEvent(event=%08x)",
    "DisableEvent(event=%08x)",
    "OpenTh(reg_PC=%08x,reg_SP_FP=%08x,reg_GP=%08x)",
    "CloseTh(handle=%08x)",
    "ChangeTh(handle=%08x)",
    "jump_to_00000000h()",
    "InitPAD2(buf1=%08x,siz1=%08x,buf2=%08x,siz2=%08x)",
    "StartPAD2()",
    "StopPAD2()",
    "PAD_init2(type=%08x,button_dest=%08x,unused=%08x,unused=%08x)",
    "PAD_dr()",
    "ReturnFromException()",
    "ResetEntryInt()",
    "HookEntryInt(addr=%08x)",
    "SystemError()",
    "SystemError()",
    "SystemError()",
    "SystemError()",
    "SystemError()",
    "SystemError()",
    "UnDeliverEvent(class=%08x,spec=%08x)",
    "SystemError()",
    "SystemError()",
    "SystemError()",
    "jump_to_00000000h()",
    "jump_to_00000000h()",
    "jump_to_00000000h()",
    "jump_to_00000000h()",
    "jump_to_00000000h()",
    "jump_to_00000000h()",
    "SystemError()",
    "SystemError()",
    "jump_to_00000000h()",
    "jump_to_00000000h()",
    "jump_to_00000000h()",
    "jump_to_00000000h()",
    "jump_to_00000000h()",
    "jump_to_00000000h()",
    "open(filename=%08x,accessmode=%08x)",
    "lseek(fd=%08x,offset=%08x,seektype=%08x)",
    "read(fd=%08x,dst=%08x,length=%08x)",
    "write(fd=%08x,src=%08x,length=%08x)",
    "close(fd=%08x)",
    "ioctl(fd=%08x,cmd=%08x,arg=%08x)",
    "exit(exitcode=%08x)",
    "isatty(fd=%08x)",
    "getc(fd=%08x)",
    "putc(char=%08x,fd=%08x)",
    "getchar()",
    "putchar(char=%08x)",
    "gets(dst=%08x)",
    "puts(src=%08x)",
    "cd(name=%08x)",
    "format(devicename=%08x)",
    "firstfile2(filename=%08x,direntry=%08x)",
    "nextfile(direntry=%08x)",
    "rename(old_filename=%08x,new_filename=%08x)",
    "erase(filename=%08x)",
    "undelete(filename=%08x)",
    "AddDrv(device_info=%08x)",
    "DelDrv(device_name_lowercase=%08x)",
    "PrintInstalledDevices()",
    "InitCARD2(pad_enable=%08x)",
    "StartCARD2()",
    "StopCARD2()",
    "_card_info_subfunc(port=%08x)",
    "_card_write(port=%08x,sector=%08x,src=%08x)",
    "_card_read(port=%08x,sector=%08x,dst=%08x)",
    "_new_card()",
    "Krom2RawAdd(shiftjis_code=%08x)",
    "SystemError()",
    "Krom2Offset(shiftjis_code=%08x)",
    "_get_errno()",
    "_get_error(fd=%08x)",
    "GetC0Table()",
    "GetB0Table()",
    "_card_chan()",
    "testdevice(devicename=%08x)",
    "SystemError()",
    "ChangeClearPAD(int=%08x)",
    "_card_status(slot=%08x)",
    "_card_wait(slot=%08x)"
};

static const char* g_psx_cpu_c_kcall_symtable[] = {
    "EnqueueTimerAndVblankIrqs(priority=%08x)",
    "EnqueueSyscallHandler(priority=%08x)",
    "SysEnqIntRP(priority=%08x,struc=%08x)",
    "SysDeqIntRP(priority=%08x,struc=%08x)",
    "get_free_EvCB_slot()",
    "get_free_TCB_slot()",
    "ExceptionHandler()",
    "InstallExceptionHandlers()",
    "SysInitMemory(addr=%08x,size=%08x)",
    "SysInitKernelVariables()",
    "ChangeClearRCnt(t=%08x,flag=%08x)",
    "SystemError()",
    "InitDefInt(priority=%08x)",
    "SetIrqAutoAck(irq=%08x,flag=%08x)",
    "return 0",
    "return 0",
    "return 0",
    "return 0",
    "InstallDevices(ttyflag=%08x)",
    "FlushStdInOutPut()",
    "return 0",
    "_cdevinput(circ=%08x,char=%08x)",
    "_cdevscan()",
    "_circgetc(circ=%08x)",
    "_circputc(char=%08x,circ=%08x)",
    "_ioabort(txt1=%08x,txt2=%08x)",
    "set_card_find_mode(mode=%08x)",
    "KernelRedirect(ttyflag=%08x)",
    "AdjustA0Table()",
    "get_card_find_mode()"
};

#define TRACE_M(m) \
    log_trace("%08x: %-7s $%s, %+i($%s)", cpu->pc-8, m, g_mips_cc_register_names[T], IMM16S, g_mips_cc_register_names[S])

#define TRACE_I16S(m) \
    log_trace("%08x: %-7s $%s, 0x%04x", cpu->pc-8, m, g_mips_cc_register_names[T], IMM16)

#define TRACE_I16D(m) \
    log_trace("%08x: %-7s $%s, $%s, 0x%04x", cpu->pc-8, m, g_mips_cc_register_names[T], g_mips_cc_register_names[S], IMM16)

#define TRACE_I5D(m) \
    log_trace("%08x: %-7s $%s, $%s, %u", cpu->pc-8, m, g_mips_cc_register_names[D], g_mips_cc_register_names[T], IMM5)

#define TRACE_I26(m) \
    log_trace("%08x: %-7s 0x%07x", cpu->pc-8, m, ((cpu->pc & 0xf0000000) | (IMM26 << 2)))

#define TRACE_RT(m) \
    log_trace("%08x: %-7s $%s, $%s, $%s", cpu->pc-8, m, g_mips_cc_register_names[D], g_mips_cc_register_names[S], g_mips_cc_register_names[T])

#define TRACE_C0M(m) \
    log_trace("%08x: %-7s $%s, $%s", cpu->pc-8, m, g_mips_cc_register_names[T], g_mips_cop0_register_names[D])

#define TRACE_C2M(m) \
    log_trace("%08x: %-7s $%s, $cop2_r%u", cpu->pc-8, m, g_mips_cc_register_names[T], D)

#define TRACE_C2MC(m) \
    log_trace("%08x: %-7s $%s, $cop2_r%u", cpu->pc-8, m, g_mips_cc_register_names[T], D + 32)

#define TRACE_B(m) \
    log_trace("%08x: %-7s $%s, $%s, %-i", cpu->pc-8, m, g_mips_cc_register_names[S], g_mips_cc_register_names[T], IMM16S << 2)

#define TRACE_RS(m) \
    log_trace("%08x: %-7s $%s", cpu->pc-8, m, g_mips_cc_register_names[S])

#define TRACE_MTF(m) \
    log_trace("%08x: %-7s $%s", cpu->pc-8, m, g_mips_cc_register_names[D])

#define TRACE_RD(m) \
    log_trace("%08x: %-7s $%s, $%s", cpu->pc-8, m, g_mips_cc_register_names[D], g_mips_cc_register_names[S])

#define TRACE_MD(m) \
    log_trace("%08x: %-7s $%s, $%s", cpu->pc-8, m, g_mips_cc_register_names[S], g_mips_cc_register_names[T]);

#define TRACE_I20(m) \
    log_trace("%08x: %-7s 0x%05x", cpu->pc-8, m, CMT);

#define TRACE_N(m) \
    log_trace("%08x: %-7s", cpu->pc-8, m);

#define DEBUG_ALL \
    log_fatal("r0=%08x at=%08x v0=%08x v1=%08x", cpu->r[0] , cpu->r[1] , cpu->r[2] , cpu->r[3] ); \
    log_fatal("a0=%08x a1=%08x a2=%08x a3=%08x", cpu->r[4] , cpu->r[5] , cpu->r[6] , cpu->r[7] ); \
    log_fatal("t0=%08x t1=%08x t2=%08x t3=%08x", cpu->r[8] , cpu->r[9] , cpu->r[10], cpu->r[11]); \
    log_fatal("t4=%08x t5=%08x t6=%08x t7=%08x", cpu->r[12], cpu->r[13], cpu->r[14], cpu->r[15]); \
    log_fatal("s0=%08x s1=%08x s2=%08x s3=%08x", cpu->r[16], cpu->r[17], cpu->r[18], cpu->r[19]); \
    log_fatal("s4=%08x s5=%08x s6=%08x s7=%08x", cpu->r[20], cpu->r[21], cpu->r[22], cpu->r[23]); \
    log_fatal("t8=%08x t9=%08x k0=%08x k1=%08x", cpu->r[24], cpu->r[25], cpu->r[26], cpu->r[27]); \
    log_fatal("gp=%08x sp=%08x fp=%08x ra=%08x", cpu->r[28], cpu->r[29], cpu->r[30], cpu->r[31]); \
    log_fatal("pc=%08x hi=%08x lo=%08x l:%s=%08x", cpu->pc, cpu->hi, cpu->lo, g_mips_cc_register_names[cpu->load_d], cpu->load_v); \
    exit(1)

const char* g_mips_cop0_register_names[] = {
    "cop0_r0",
    "cop0_r1",
    "cop0_r2",
    "cop0_bpc",
    "cop0_r4",
    "cop0_bda",
    "cop0_jumpdest",
    "cop0_dcic",
    "cop0_badvaddr",
    "cop0_bdam",
    "cop0_r10",
    "cop0_bpcm",
    "cop0_sr",
    "cop0_cause",
    "cop0_epc",
    "cop0_prid"
};

static const char* g_mips_cc_register_names[] = {
    "r0", "at", "v0", "v1", "a0", "a1", "a2", "a3",
    "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
    "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
    "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"
};

static const char* g_psx_cpu_syscall_function_symbol_table[] = {
    "NoFunction",
    "EnterCriticalSection",
    "ExitCriticalSection",
    "ChangeThreadSubFunction"
    // DeliverEvent (invalid)
};

#else
#define TRACE_M(m)
#define TRACE_I16S(m)
#define TRACE_I16D(m)
#define TRACE_I5D(m)
#define TRACE_I26(m)
#define TRACE_RT(m)
#define TRACE_C0M(m)
#define TRACE_C2M(m)
#define TRACE_C2MC(m)
#define TRACE_B(m)
#define TRACE_RS(m)
#define TRACE_MTF(m)
#define TRACE_RD(m)
#define TRACE_MD(m)
#define TRACE_I20(m)
#define TRACE_N(m)
#endif