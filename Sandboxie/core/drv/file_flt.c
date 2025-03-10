/*
 * Copyright 2004-2020 Sandboxie Holdings, LLC 
 * Copyright 2020 David Xanatos, xanasoft.com
 *
 * This program is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

//---------------------------------------------------------------------------
// File System:  Filesystem Minifilter for Windows Vista and later
//---------------------------------------------------------------------------


#include "my_fltkernel.h"
#include "thread.h"
#include "token.h"


//---------------------------------------------------------------------------
// Structures and Types
//---------------------------------------------------------------------------


typedef struct _PROTECTED_ROOT {

    LIST_ELEM list_elem;
    
    ULONG reg_root_len;
    WCHAR reg_root[MAX_REG_ROOT_LEN];
    ULONG file_root_len;
    WCHAR file_root[1];

} PROTECTED_ROOT;


//---------------------------------------------------------------------------
// Functions
//---------------------------------------------------------------------------


static BOOLEAN File_Init_Filter(void);

static void File_Unload_Filter(void);

static FLT_PREOP_CALLBACK_STATUS File_PreOperation(
    PFLT_CALLBACK_DATA Data,
    PCFLT_RELATED_OBJECTS FltObjects,
    void **CompletionContext);

static NTSTATUS File_CreateOperation(
    PROCESS *proc,
    FLT_IO_PARAMETER_BLOCK *Iopb,
    UNICODE_STRING *FileName);

static NTSTATUS File_RenameOperation(
    PROCESS *proc,
    FLT_IO_PARAMETER_BLOCK *Iopb,
    BOOLEAN LinkOp);

static NTSTATUS File_QueryTeardown(
    PCFLT_RELATED_OBJECTS FltObjects,
    FLT_INSTANCE_QUERY_TEARDOWN_FLAGS Flags);

static NTSTATUS File_CheckFileObject(
    PROCESS *proc, void *Object, UNICODE_STRING *NameString,
    ULONG Operation, ACCESS_MASK GrantedAccess);


//---------------------------------------------------------------------------
// Filter Manager Registration
//---------------------------------------------------------------------------


#ifdef ALLOC_DATA_PRAGMA
#pragma data_seg("INITDATA")
#pragma const_seg("INITDATA")
#endif


#define FILE_CALLBACK(irp) { irp, 0, File_PreOperation, NULL, NULL },


static const FLT_OPERATION_REGISTRATION File_Callbacks[] = {

    FILE_CALLBACK(IRP_MJ_CREATE)
    FILE_CALLBACK(IRP_MJ_CREATE_NAMED_PIPE)
    FILE_CALLBACK(IRP_MJ_CREATE_MAILSLOT)
    FILE_CALLBACK(IRP_MJ_SET_INFORMATION)

    /*
    FILE_CALLBACK(IRP_MJ_CLOSE)
    FILE_CALLBACK(IRP_MJ_READ)
    FILE_CALLBACK(IRP_MJ_WRITE)
    FILE_CALLBACK(IRP_MJ_QUERY_INFORMATION)
    FILE_CALLBACK(IRP_MJ_QUERY_EA)
    FILE_CALLBACK(IRP_MJ_SET_EA)
    FILE_CALLBACK(IRP_MJ_FLUSH_BUFFERS)
    FILE_CALLBACK(IRP_MJ_QUERY_VOLUME_INFORMATION)
    FILE_CALLBACK(IRP_MJ_SET_VOLUME_INFORMATION)
    FILE_CALLBACK(IRP_MJ_DIRECTORY_CONTROL)
    FILE_CALLBACK(IRP_MJ_FILE_SYSTEM_CONTROL)
    FILE_CALLBACK(IRP_MJ_DEVICE_CONTROL)
    FILE_CALLBACK(IRP_MJ_INTERNAL_DEVICE_CONTROL)
    FILE_CALLBACK(IRP_MJ_SHUTDOWN)
    FILE_CALLBACK(IRP_MJ_LOCK_CONTROL)
    FILE_CALLBACK(IRP_MJ_CLEANUP)
    FILE_CALLBACK(IRP_MJ_QUERY_SECURITY)
    FILE_CALLBACK(IRP_MJ_SET_SECURITY)
    FILE_CALLBACK(IRP_MJ_QUERY_QUOTA)
    FILE_CALLBACK(IRP_MJ_SET_QUOTA)
    FILE_CALLBACK(IRP_MJ_PNP)

    */
    FILE_CALLBACK(IRP_MJ_ACQUIRE_FOR_SECTION_SYNCHRONIZATION)
    /*
    FILE_CALLBACK(IRP_MJ_RELEASE_FOR_SECTION_SYNCHRONIZATION)
    FILE_CALLBACK(IRP_MJ_ACQUIRE_FOR_MOD_WRITE)
    FILE_CALLBACK(IRP_MJ_RELEASE_FOR_MOD_WRITE)
    FILE_CALLBACK(IRP_MJ_ACQUIRE_FOR_CC_FLUSH)
    FILE_CALLBACK(IRP_MJ_RELEASE_FOR_CC_FLUSH)

    FILE_CALLBACK(IRP_MJ_FAST_IO_CHECK_IF_POSSIBLE)
    FILE_CALLBACK(IRP_MJ_NETWORK_QUERY_OPEN)
    FILE_CALLBACK(IRP_MJ_MDL_READ)
    FILE_CALLBACK(IRP_MJ_MDL_READ_COMPLETE)
    FILE_CALLBACK(IRP_MJ_PREPARE_MDL_WRITE)
    FILE_CALLBACK(IRP_MJ_MDL_WRITE_COMPLETE)
    FILE_CALLBACK(IRP_MJ_VOLUME_MOUNT)
    FILE_CALLBACK(IRP_MJ_VOLUME_DISMOUNT)
    */

    { IRP_MJ_OPERATION_END }

};


static const FLT_CONTEXT_REGISTRATION File_Contexts[] = {

    { FLT_CONTEXT_END }
};


static const FLT_REGISTRATION File_Registration = {

    // Size, Version, Flags

    sizeof(FLT_REGISTRATION),

    FLT_REGISTRATION_VERSION_0202,

    FLTFL_REGISTRATION_DO_NOT_SUPPORT_SERVICE_STOP,

    // ContextRegistration, OperationRegistration

    File_Contexts,
    File_Callbacks,

    // Callbacks

    NULL,                                   //  FilterUnload
    NULL,                                   //  InstanceSetup
    File_QueryTeardown,                     //  InstanceQueryTeardown
    NULL,                                   //  InstanceTeardownStart
    NULL,                                   //  InstanceTeardownComplete
    NULL,                                   //  GenerateFileName
    NULL,                                   //  GenerateDestinationFileName
    NULL                                    //  NormalizeNameComponent

};


#ifdef ALLOC_DATA_PRAGMA
#pragma data_seg()
#pragma const_seg()
#endif


//---------------------------------------------------------------------------


#ifdef ALLOC_PRAGMA
#pragma alloc_text (INIT, File_Init_Filter)
#endif // ALLOC_PRAGMA


//---------------------------------------------------------------------------
// Variables
//---------------------------------------------------------------------------


PFLT_FILTER File_FilterCookie = NULL;
extern UCHAR Sbie_Token_SourceName[5];


static LIST File_ProtectedRoots;
static PERESOURCE File_ProtectedRootsLock;


//---------------------------------------------------------------------------
// File_Init_Filter
//---------------------------------------------------------------------------


_FX BOOLEAN File_Init_Filter(void)
{
    static const WCHAR *_MiniFilter = L"MiniFilter";
    NTSTATUS status;

    List_Init(&File_ProtectedRoots);
    if (! Mem_GetLockResource(&File_ProtectedRootsLock, TRUE))
        return FALSE;

    //
    // register as a minifilter driver
    //
    // note that minifilter supports only filtering of real files, so we
    // will also set handlers for file handle syscalls
    //
    // there is benefit in doing minifilter filtering because syscall
    // handlers intercept only after the the object was opened, and in
    // the case of files, this could leave persistent files outside the
    // sandbox.  also, file paths are complicated and may contain links,
    // which will be reparsed for us by the kernel IO subsystem
    //

    status = FltRegisterFilter(
                    Driver_Object, &File_Registration, &File_FilterCookie);

    if (! NT_SUCCESS(status)) {
        Log_Status_Ex(MSG_OBJ_HOOK_ANY_PROC, 0x81, status, _MiniFilter);
        return FALSE;
    }

    status = FltStartFiltering(File_FilterCookie);

    if (! NT_SUCCESS(status)) {
        Log_Status_Ex(MSG_OBJ_HOOK_ANY_PROC, 0x82, status, _MiniFilter);
        return FALSE;
    }

    //
    // set object open handlers for named pipe and mail slot file objects
    //

    if (! Syscall_Set2("CreateFile",            File_CheckFileObject))
        return FALSE;

    if (! Syscall_Set2("CreateMailslotFile",    File_CheckFileObject))
        return FALSE;

    if (! Syscall_Set2("CreateNamedPipeFile",   File_CheckFileObject))
        return FALSE;

    if (! Syscall_Set2("OpenFile",              File_CheckFileObject))
        return FALSE;

    //
    // set API functions
    //

    Api_SetFunction(API_PROTECT_ROOT,           File_Api_ProtectRoot);
    Api_SetFunction(API_UNPROTECT_ROOT,         File_Api_UnprotectRoot);

    //
    // successful initialization
    //

    return TRUE;
}


//---------------------------------------------------------------------------
// File_Unload_Filter
//---------------------------------------------------------------------------


_FX void File_Unload_Filter(void)
{
    if (File_FilterCookie) {
        FltUnregisterFilter(File_FilterCookie);
        File_FilterCookie = NULL;
    }

    if (File_ProtectedRootsLock) {
        Mem_FreeLockResource(&File_ProtectedRootsLock);
        File_ProtectedRootsLock = NULL;
    }
}


//---------------------------------------------------------------------------
// File_PreOperation
//---------------------------------------------------------------------------

#define SBIE_FILE_GENERIC_WRITE (\
    FILE_WRITE_DATA          |\
    FILE_WRITE_ATTRIBUTES    |\
    FILE_WRITE_EA            |\
    FILE_APPEND_DATA)


_FX FLT_PREOP_CALLBACK_STATUS File_PreOperation(
    PFLT_CALLBACK_DATA Data,
    PCFLT_RELATED_OBJECTS FltObjects,
    void **CompletionContext)
{
    NTSTATUS status;
    PROCESS *proc;
    FLT_IO_PARAMETER_BLOCK *Iopb;

    //
    // if caller is user mode, we only handle a few specific IRP operations
    //
    // if caller is kernel mode, we only handle IRP_MJ_CREATE for font access
    //

    status = STATUS_SUCCESS;
    
    Iopb = Data->Iopb;

    if (FLT_IS_FS_FILTER_OPERATION(Data) && Data->RequestorMode == UserMode && Iopb->MajorFunction == IRP_MJ_ACQUIRE_FOR_SECTION_SYNCHRONIZATION)
        goto check;

    if (! FLT_IS_IRP_OPERATION(Data))
        goto finish;

    if (Data->RequestorMode == KernelMode) {

        if (    Iopb->MajorFunction == IRP_MJ_CREATE
             && Data->Thread == PsGetCurrentThread()) {

            BOOLEAN bSetDirty = FALSE;

            PFILE_OBJECT FileObject = Iopb->TargetFileObject;

            File_ReplaceTokenIfFontRequest(
                Iopb->Parameters.Create.SecurityContext->AccessState,
                FileObject->DeviceObject, &FileObject->FileName, &bSetDirty);

            if (bSetDirty)
            {
                FltSetCallbackDataDirty(Data);
            }
        }

        goto finish;

    } else if (Iopb->MajorFunction == IRP_MJ_SET_INFORMATION) {
        // we allow IRP_MJ_SET_INFORMATION to pass except for these 3 
        if ((Iopb->Parameters.SetFileInformation.FileInformationClass != FileRenameInformation) &&
            (Iopb->Parameters.SetFileInformation.FileInformationClass != FileRenameInformationEx) &&

            (Iopb->Parameters.SetFileInformation.FileInformationClass != FileLinkInformation) &&
            (Iopb->Parameters.SetFileInformation.FileInformationClass != FileLinkInformationEx) &&

            (Iopb->Parameters.SetFileInformation.FileInformationClass != FileHardLinkInformation) &&
            (Iopb->Parameters.SetFileInformation.FileInformationClass != FileHardLinkFullIdInformation))

            goto finish;

    } else if (Iopb->MajorFunction != IRP_MJ_CREATE &&
               Iopb->MajorFunction != IRP_MJ_CREATE_NAMED_PIPE &&
               Iopb->MajorFunction != IRP_MJ_CREATE_MAILSLOT)
        goto finish;

    if (Data->Thread != PsGetCurrentThread())
        goto finish;

    // Block write operations from a system account process (spoolsv.exe) impersonated by sbox user token
    // if AllowSpoolerPrintToFile is not enabled, and this is the spooler process, we block all CreateFile calls that ask for write access
    // except in the cases where the spooler is writing to special folders (see below)

    if (Iopb->MajorFunction == IRP_MJ_CREATE &&
        Iopb->Parameters.Create.SecurityContext &&
        (Iopb->Parameters.Create.SecurityContext->DesiredAccess & SBIE_FILE_GENERIC_WRITE) &&
        MyIsProcessRunningAsSystemAccount(PsGetCurrentProcessId()))
    {
        void *nbuf = 0;
        ULONG nlen = 0;
        WCHAR *nptr = 0;

        Process_GetProcessName(Driver_Pool, (ULONG_PTR)PsGetCurrentProcessId(), &nbuf, &nlen, &nptr);

        if (nbuf)
        {
            if ((_wcsicmp(nptr, L"spoolsv.exe") == 0) &&        // is this the print spooler process?
                // Stupid hack: some printer drivers try to open a DOS-style port at the end of a directory name.  E.g. C:\Users\admin\Documents\XPSPort:
                // They must get invalid file error and not access denied, or they will error out.
                !UnicodeStringEndsWith(&Iopb->TargetFileObject->FileName, L":", TRUE) &&
                // another stupid hack to make spoolsv happy.  It tries to open this file, but never uses it.  So we again have to humor it or it will fail.
                !SearchUnicodeString(&Iopb->TargetFileObject->FileName, L"tpwinprn-stat.txt", TRUE) &&
                // The spooler needs to write to a network pipe and will fail if it can't.
                !SearchUnicodeString(&Iopb->TargetFileObject->FileName, L"\\pipe\\spoolss", TRUE))
            {
                ULONG ulOwnerPid = GetThreadTokenOwnerPid();
                if (ulOwnerPid)
                {
                    proc = Process_Find((HANDLE)ulOwnerPid, NULL);  // is this a sandboxed process?
                    if (proc && !proc->terminated && !proc->ipc_allowSpoolerPrintToFile)   // if process specifically allowed to use spooler print to file, we can skip everything below
                    {
                        FLT_FILE_NAME_INFORMATION   *pTargetFileNameInfo = NULL;
                        BOOLEAN     result = FALSE;

                        // Get normalized full path to target file.
                        // Occasionally, certain PDF apps will send in a short name (containing '~'). That will break all of our folder name checking below.
                        if (FltGetFileNameInformation(Data, FLT_FILE_NAME_NORMALIZED | FLT_FILE_NAME_QUERY_DEFAULT, &pTargetFileNameInfo) != STATUS_SUCCESS)
                        {
                            status = STATUS_ACCESS_DENIED;      // if we can't get the name, just disallow the call
                        }
                        else if (!UnicodeStringStartsWith(&pTargetFileNameInfo->Name, proc->box->spooler_directory, TRUE) ) // spooler work folder is allowed.
                        {
                            // See if the operation is allowed by normal Sbie rules.
                            // pTargetFileNameInfo has the device name at the start ("\device\harddisk2"). File_CreateOperation wants to add this itself deep in the code.
                            // So, make a new UNICODE_STRING that does not include the device (volume) name.
                            UNICODE_STRING      usTargetFile;

                            if (pTargetFileNameInfo->Volume.Length > pTargetFileNameInfo->Name.Length) {
                                status = STATUS_ACCESS_DENIED;
                            }
                            else {
                                usTargetFile.Buffer = pTargetFileNameInfo->Name.Buffer + (pTargetFileNameInfo->Volume.Length / 2);
                                usTargetFile.Length = pTargetFileNameInfo->Name.Length - pTargetFileNameInfo->Volume.Length;
                                usTargetFile.MaximumLength = pTargetFileNameInfo->Name.MaximumLength - pTargetFileNameInfo->Volume.Length;
                                status = File_CreateOperation(proc, Iopb, &usTargetFile);
                            }
                            if (status != STATUS_SUCCESS)
                            {
                                UCHAR *pStr, *pStr2;
                                int len;
                                WCHAR   wcPid[32];

                                status = STATUS_ACCESS_DENIED;  // disallow the call
                                RtlStringCbPrintfW(wcPid, sizeof(wcPid), L"[%d]", ulOwnerPid);

                                // create a string for the sandboxed proc name plus the blocked file name (plus a L", " plus NULL = 6)
                                len = proc->image_name_len + pTargetFileNameInfo->Name.Length + 6;

                                pStr = pStr2 = Mem_AllocEx(Driver_Pool, len, TRUE);
                                memcpy(pStr2, proc->image_name, proc->image_name_len);
                                pStr2 += proc->image_name_len - 2;  // subtract 2 bytes for the wchar NULL
                                wcscpy((WCHAR*)pStr2, L", ");
                                pStr2 += 4;             // bump past the L", "
                                memcpy(pStr2, pTargetFileNameInfo->Name.Buffer, pTargetFileNameInfo->Name.Length);
                                pStr2 += pTargetFileNameInfo->Name.Length;
                                memset(pStr2, 0, 2);    // add a wchar NULL

								Log_Msg_Process(MSG_1319, wcPid, (PWCHAR)pStr, proc->box->session_id, proc->pid);
                                Mem_Free(pStr, len);
                            }
                        }   // if (FltGetFileNameInformation)

                        if (pTargetFileNameInfo != NULL) {
                            FltReleaseFileNameInformation(pTargetFileNameInfo);
                        }
                    }   // if (proc)
                }   // if (ulOwnerPid)
            }   // is this the print spooler process?

            Mem_Free(nbuf, nlen);

            if (status == STATUS_ACCESS_DENIED)
                goto finish;
        }   // if (nbuf)
    }

check:

    proc = Process_Find(NULL, NULL);
    if (proc == PROCESS_TERMINATED) {
        status = STATUS_PROCESS_IS_TERMINATING;
        goto finish;
    }

    //
    // check if there are any protected root folders and restrict the access to them
    //

    if (Iopb->MajorFunction == IRP_MJ_CREATE && File_ProtectedRoots.count != 0 && Data->Iopb->TargetFileObject) {

        OBJECT_NAME_INFORMATION *Name;
        ULONG NameLength;

        status = Obj_GetParseName(Driver_Pool, Data->Iopb->TargetFileObject->DeviceObject, &Data->Iopb->TargetFileObject->FileName, &Name, &NameLength);
        if (NT_SUCCESS(status)) {

            //DbgPrint("IRP_MJ_CREATE: %S\n", Name->Name.Buffer);

            BOOLEAN protect_root = FALSE;

            KIRQL irql;
            KeRaiseIrql(APC_LEVEL, &irql);
            ExAcquireResourceExclusiveLite(File_ProtectedRootsLock, TRUE);

            PROTECTED_ROOT *root = List_Head(&File_ProtectedRoots);
            while (root) {

                if (Name->Name.Length / sizeof(WCHAR) >= root->file_root_len
                    && (Name->Name.Buffer[root->file_root_len] == L'\0' || Name->Name.Buffer[root->file_root_len] == L'\\')
                    && _wcsnicmp(Name->Name.Buffer, root->file_root, root->file_root_len) == 0
                    ) {

                    //DbgPrint("IRP_MJ_CREATE: %S\n", root->file_root);

                    protect_root = TRUE;
                    break;
                }

                root = List_Next(root);
            }

            ExReleaseResourceLite(File_ProtectedRootsLock);
            KeLowerIrql(irql);

            if (protect_root) {

                HANDLE cur_pid = PsGetCurrentProcessId();

                if (Util_IsSystemProcess(cur_pid, "csrss.exe") // csrss.exe needs access to binaries of starting up processes.
                    || cur_pid == Api_ServiceProcessId // always allow the service
                    || Session_GetLeadSession(cur_pid) != 0) // allow the session leader
                    protect_root = FALSE;
                else
                {
                    PROCESS *cur_proc = proc;
                    if (!cur_proc && ExGetPreviousMode() == KernelMode)
                        cur_proc = Process_Find(cur_pid, NULL);

                    if (cur_proc && !cur_proc->bHostInject) {
                        if (cur_proc->box->key_path_len / sizeof(WCHAR) == root->reg_root_len + 1 &&
                            _wcsnicmp(cur_proc->box->key_path, root->reg_root, root->reg_root_len) == 0) {
                            protect_root = FALSE; // its the allowed box
                        }
                    }
                }

                if (protect_root)
                {
                    status = STATUS_ACCESS_DENIED;

                    Session_MonitorPut(MONITOR_FILE | MONITOR_DENY, Name->Name.Buffer, cur_pid);

                    if (Conf_Get_Boolean(NULL, L"NotifyRootProtected", 0, FALSE)) {

                        void* nbuf = 0;
                        ULONG nlen = 0;
                        WCHAR* nptr = 0;
                        Process_GetProcessName(Driver_Pool, (ULONG_PTR)cur_pid, &nbuf, &nlen, &nptr);

                        Log_Msg_Process(MSG_1317, nptr, Name->Name.Buffer, -1, cur_pid);

                        if (nbuf) Mem_Free(nbuf, nlen);
                    }
                }
            }

            Mem_Free(Name, NameLength);

            if (!NT_SUCCESS(status))
                goto finish;
        }
    }

    //
    // check if the caller is sandboxed before proceeding
    //

    if (!proc || proc->bHostInject || proc->disable_file_flt)
        goto finish;

    //
    // process request
    //

    if (Iopb->MajorFunction == IRP_MJ_SET_INFORMATION) {
        // Do not allow hard links outside the sandbox
        if (Iopb->Parameters.SetFileInformation.FileInformationClass == FileLinkInformation 
         || Iopb->Parameters.SetFileInformation.FileInformationClass == FileLinkInformationEx) {

            /*
            // FILE_LINK_INFORMATION* FileInfo = (FILE_LINK_INFORMATION*)Iopb->Parameters.SetFileInformation.InfoBuffer;
            
            // For rename or link operations. If InfoBuffer->FileName contains a fully qualified file name, or if InfoBuffer->RootDirectory is non-NULL, 
            // this member is a file object pointer for the parent directory of the file that is the target of the operation. Otherwise it is NULL.
            //if (Iopb->Parameters.SetFileInformation.ParentOfTarget == NULL) {

                FLT_FILE_NAME_INFORMATION   *pTargetFileNameInfo = NULL;

                if (FltGetFileNameInformation(Data, FLT_FILE_NAME_NORMALIZED | FLT_FILE_NAME_QUERY_DEFAULT, &pTargetFileNameInfo) != STATUS_SUCCESS)
                {
                    status = STATUS_ACCESS_DENIED;      // if we can't get the name, just disallow the call
                }
                else
                {
                    // if the file is to be created in the same directory as the original file, we check if the original file is in a sandboxed location
                    if(!Box_IsBoxedPath(proc->box, file, &pTargetFileNameInfo->Name)) {
                        status = STATUS_ACCESS_DENIED;
                    }
                }
                       
                if (pTargetFileNameInfo != NULL) {
                    FltReleaseFileNameInformation(pTargetFileNameInfo);
                }
            //}
            //else if(!Box_IsBoxedPath(proc->box, file, &Iopb->Parameters.SetFileInformation.ParentOfTarget->FileName)) { // bug bug ParentOfTarget->FileName does not contain device path
            //    status = STATUS_ACCESS_DENIED;
            //}
            */

            status = File_RenameOperation(proc, Iopb, TRUE);
        }
        else if (Iopb->Parameters.SetFileInformation.FileInformationClass == FileRenameInformation 
         || Iopb->Parameters.SetFileInformation.FileInformationClass == FileRenameInformationEx) {
            status = File_RenameOperation(proc, Iopb, FALSE);
        }
        else
            status = STATUS_ACCESS_DENIED;
    }
    else if (Iopb->MajorFunction == IRP_MJ_ACQUIRE_FOR_SECTION_SYNCHRONIZATION) {

        if (!proc->image_from_box && proc->protect_host_images && Data->Iopb->TargetFileObject) {

            //
            // If host image protection is enabled, check if we are in process of creating a new process
            // in which case allow boxed images to be mapped by NtCreateUserProcess
            //

            THREAD *thrd = Thread_GetByThreadId(proc, NULL);

            if ((Data->Iopb->Parameters.AcquireForSectionSynchronization.PageProtection & PAGE_EXECUTE) && !(thrd && thrd->create_process_in_progress)) {

                OBJECT_NAME_INFORMATION *Name;
                ULONG NameLength;

                status = Obj_GetParseName(proc->pool, Data->Iopb->TargetFileObject->DeviceObject, &Data->Iopb->TargetFileObject->FileName, &Name, &NameLength);
                if (NT_SUCCESS(status)) {

                    if (Box_IsBoxedPath(proc->box, file, &Name->Name))  {

                        if(Conf_Get_Boolean(proc->box->name, L"NotifyImageLoadDenied", 0, TRUE))
				            Log_Msg_Process(MSG_1305, Name->Name.Buffer + (proc->box->file_path_len/sizeof(WCHAR) - 1), NULL, proc->box->session_id, proc->pid);

                        Log_Debug_Msg(MONITOR_IMAGE | MONITOR_DENY, NULL, Name->Name.Buffer);
                        status = STATUS_ACCESS_DENIED;
                    }

                    Mem_Free(Name, NameLength);
                }
            }
        }
    }
    else {
        // We have a problem that has started appearing in Win 10 1903. Sometimes a file rename will end up in the file pre-create callback (i.e. this code)
        // with only a file name and not the full path to the destination file (target). The Sandboxie file name parse routines all depend on a full path.
        // So instead of redesigning all this code, I (Curt) am going to build the full path here when required.
        // See MSDN entry for FltGetFileNameInformation. There is some information on "file name tunneling"

        UNICODE_STRING  usFileName = { 0 };
        ULONG   nTempFullPathLen = 0;
        UCHAR   *pTempFullPath = NULL;

        if (Data->Iopb->TargetFileObject)
        {
            if (!UnicodeStringStartsWith(&Data->Iopb->TargetFileObject->FileName, L"\\", TRUE))     // just a filename and not a full path?
            {
                // Get normalized path to target file.
                FLT_FILE_NAME_INFORMATION   *pTargetFileNameInfo = NULL;
                if (FltGetFileNameInformation(Data, FLT_FILE_NAME_NORMALIZED | FLT_FILE_NAME_QUERY_DEFAULT, &pTargetFileNameInfo) == STATUS_SUCCESS)
                {
                    if (pTargetFileNameInfo)
                    {
                        // The parse routines want a full path without the kernel volume name
                        nTempFullPathLen = pTargetFileNameInfo->Name.Length + Data->Iopb->TargetFileObject->FileName.Length - pTargetFileNameInfo->Volume.Length + 4;
                        UCHAR *pStr = pTempFullPath = Mem_AllocEx(Driver_Pool, nTempFullPathLen, TRUE);

                        // copy the path starting after the kernel volume name (e.g. \Device\HarddiskVolume2\)
                        memcpy(pStr,
                            pTargetFileNameInfo->Name.Buffer + pTargetFileNameInfo->Volume.Length / 2,  // ptr operations are all based on size of ptr type, so div by 2.
                            pTargetFileNameInfo->Name.Length - pTargetFileNameInfo->Volume.Length);

                        pStr += pTargetFileNameInfo->Name.Length - pTargetFileNameInfo->Volume.Length;
                        *pStr++ = '\\';     // add in a backslash
                        *pStr++ = 0;
                        // add in the file name
                        memcpy(pStr, Data->Iopb->TargetFileObject->FileName.Buffer, Data->Iopb->TargetFileObject->FileName.Length);
                        pStr += Data->Iopb->TargetFileObject->FileName.Length;
                        memset(pStr, 0, 2); // add a wchar NULL

                        RtlInitUnicodeString(&usFileName, (PCWSTR)pTempFullPath);
                    }
                    FltReleaseFileNameInformation(pTargetFileNameInfo);
                }
            }
        }

        status = File_CreateOperation(proc, Iopb, pTempFullPath ? &usFileName : NULL);
        if (pTempFullPath)
            Mem_Free(pTempFullPath, nTempFullPathLen);
    }

    //
    // finish
    //

finish:

    Data->IoStatus.Status = status;

    *CompletionContext = NULL;

    if (status == STATUS_SUCCESS)
        status = FLT_PREOP_SUCCESS_NO_CALLBACK;
    else
        status = FLT_PREOP_COMPLETE;

    return status;
}


//---------------------------------------------------------------------------
// File_CreateOperation
//---------------------------------------------------------------------------


_FX NTSTATUS File_CreateOperation(
    PROCESS *proc,
    FLT_IO_PARAMETER_BLOCK *Iopb,
    UNICODE_STRING *FileName)
{
    FLT_PARAMETERS *Parms;
    PFILE_OBJECT FileObject;
    PDEVICE_OBJECT DeviceObject;
    MY_CONTEXT MyContext;
    NTSTATUS status;

    Parms = &Iopb->Parameters;

    FileObject = Iopb->TargetFileObject;

    if (FileName == NULL)
        FileName = &FileObject->FileName;

    //
    // call File_Generic_MyParseProc
    //

    memzero(&MyContext, sizeof(MyContext));
    MyContext.HaveContext = TRUE;

    MyContext.CreateDisposition = ((Parms->Create.Options >> 24) & 0xFF);
    MyContext.CreateOptions = (Parms->Create.Options & 0x00FFFFFF);
    MyContext.OriginalDesiredAccess =
        Parms->Create.SecurityContext->AccessState->OriginalDesiredAccess;

    DeviceObject = FileObject->DeviceObject;

    status = File_Generic_MyParseProc(
        proc, DeviceObject, DeviceObject->DeviceType,
        FileName, &MyContext, TRUE);

    return status;
}


//---------------------------------------------------------------------------
// File_RenameOperation
//---------------------------------------------------------------------------


_FX NTSTATUS File_RenameOperation(
    PROCESS *proc,
    FLT_IO_PARAMETER_BLOCK *Iopb,
    BOOLEAN LinkOp)
{
    FLT_PARAMETERS *Parms;
    PFILE_OBJECT FileObject;
    UNICODE_STRING FileName;
    MY_CONTEXT MyContext;

    //
    // sandboxed programs always specify a root directory and file name
    //

    Parms = &Iopb->Parameters;

    if(LinkOp) {

        FILE_LINK_INFORMATION *infoL;

        infoL = (FILE_LINK_INFORMATION *)Parms->SetFileInformation.InfoBuffer;
    
        FileObject = Parms->SetFileInformation.ParentOfTarget;

        if ((! FileObject) || (! infoL) || (! infoL->FileNameLength))
            return STATUS_ACCESS_DENIED;

        FileName.Length = (USHORT)infoL->FileNameLength;
        FileName.MaximumLength = FileName.Length;
        FileName.Buffer = infoL->FileName;

    } else {

        FILE_RENAME_INFORMATION *infoR;

        infoR = (FILE_RENAME_INFORMATION *)Parms->SetFileInformation.InfoBuffer;

        FileObject = Parms->SetFileInformation.ParentOfTarget;

        if ((! FileObject) || (! infoR) || (! infoR->FileNameLength))
            return STATUS_ACCESS_DENIED;

        FileName.Length = (USHORT)infoR->FileNameLength;
        FileName.MaximumLength = FileName.Length;
        FileName.Buffer = infoR->FileName;
    }

    //
    // if the target directory specifies just a filename (no leading slash)
    // but the related file object pointer specifies a full path, then use
    // the full path instead of the original target file object
    //
    // this usually happens when renaming a file on a network drive
    //

    if (FileObject->FileName.Length >= 2 &&
            FileObject->FileName.Buffer &&
            FileObject->FileName.Buffer[0] != L'\\') {

        PFILE_OBJECT RelatedFileObject = FileObject->RelatedFileObject;
        if (RelatedFileObject &&
                RelatedFileObject->FileName.Length >= 2 &&
                RelatedFileObject->FileName.Buffer &&
                RelatedFileObject->FileName.Buffer[0] == L'\\') {

            FileObject = RelatedFileObject;
        }
    }

    //
    // call the generic parser function
    //

    memzero(&MyContext, sizeof(MyContext));
    MyContext.HaveContext = TRUE;

    MyContext.CreateDisposition = FILE_OPEN;
    MyContext.Options = IO_OPEN_TARGET_DIRECTORY;
    MyContext.OriginalDesiredAccess = FILE_GENERIC_WRITE;

    return File_Generic_MyParseProc(
                proc, FileObject, FileObject->DeviceObject->DeviceType,
                &FileName, &MyContext, FALSE);
}


//---------------------------------------------------------------------------
// File_QueryTeardown
//---------------------------------------------------------------------------


_FX NTSTATUS File_QueryTeardown(
    PCFLT_RELATED_OBJECTS FltObjects,
    FLT_INSTANCE_QUERY_TEARDOWN_FLAGS Flags)
{
    //
    // this callback must be implemented otherwise the filter manager
    // will not allow our driver to detach from mounted volumes
    //

    return STATUS_SUCCESS;
}


//---------------------------------------------------------------------------
// File_CheckFileObject
//---------------------------------------------------------------------------


_FX NTSTATUS File_CheckFileObject(
    PROCESS *proc, void *Object, UNICODE_STRING *NameString,
    ULONG Operation, ACCESS_MASK GrantedAccess)
{
    NTSTATUS status;
    FILE_OBJECT *FileObject;
    UNICODE_STRING FileName;
    OBJECT_NAME_INFORMATION *Name;
    ULONG NameLength;
    MY_CONTEXT MyContext;

    FileObject = (FILE_OBJECT *)Object;

    //
    // hack for Kaspersky 2014:  it queues an APC into Wow64 processes which
    // overwrites some NTAPI stubs, in particular NtSetInformationThread.
    // this gets in the way of the special NtSetInformationThread call from
    // Gui_ConnectToWindowStationAndDesktop in core/dll/gui.c
    //
    // if we block access to file \Device\swmon_dt_010011_kl1 then the APC
    // code gives up and does not overwrite the NTAPI stubs.  we return a
    // special status code that does not cancel the process, see also
    // function Syscall_OpenHandle in file syscall_open.c
    //

    // $Workaround$ - 3rd party fix
#ifdef _WIN64
    if (! proc->sbiedll_loaded) {
        WCHAR *Backslash = wcsrchr(NameString->Buffer, L'\\');
        WCHAR *Underscore = wcsrchr(NameString->Buffer, L'_');
        if (Backslash && Underscore &&
                    _wcsicmp(Underscore, L"_kl1") == 0&&
                    _wcsnicmp(Backslash, L"\\swmon_", 7) == 0)
            return STATUS_BAD_INITIAL_PC;
    }
#endif _WIN64

    //
    // continue with normal processing
    //

    if (FileObject->DeviceObject->DeviceType != FILE_DEVICE_NAMED_PIPE &&
        FileObject->DeviceObject->DeviceType != FILE_DEVICE_MAILSLOT) {

        //
        // we only care about device types that are not
        // intercepted by the minifilter
        //

        return STATUS_SUCCESS;
    }

    //
    // in some esoteric cases, Obj_GetName gets just the device name,
    // so we use the path from the FileObject structures
    //

    FileName.Length = 0;
    FileName.MaximumLength = 0;
    FileName.Buffer = L"";

    status = Obj_GetParseName(
        proc->pool, FileObject, &FileName, &Name, &NameLength);
    if (NT_SUCCESS(status) && (Name != &Obj_Unnamed)) {

        BOOLEAN OnlyDeviceName = FALSE;
        ULONG len = wcslen(Name->Name.Buffer);
        if (FileObject->DeviceObject->DeviceType == FILE_DEVICE_NAMED_PIPE
                && len == 17) /* \Device\NamedPipe */
            OnlyDeviceName = TRUE;
        else if (FileObject->DeviceObject->DeviceType == FILE_DEVICE_MAILSLOT
                && len == 16) /* \Device\MailSlot */
            OnlyDeviceName = TRUE;

        if (OnlyDeviceName) {

            memcpy(&FileName, &FileObject->FileName, sizeof(UNICODE_STRING));
            // DbgPrint("ONLY DEVICE NAME <%S>-<%S>\n", Name->Name.Buffer, FileName.Buffer);
        }

        Mem_Free(Name, NameLength);
    }

    //
    // initialize MyContext structure and check access
    //

    memzero(&MyContext, sizeof(MyContext));
    MyContext.HaveContext = TRUE;

    MyContext.CreateDisposition = FILE_OPEN;
    MyContext.Options = IO_OPEN_TARGET_DIRECTORY;
    MyContext.OriginalDesiredAccess = FILE_GENERIC_WRITE;

    return File_Generic_MyParseProc(
                proc, FileObject, FileObject->DeviceObject->DeviceType,
                &FileName, &MyContext, FALSE);
}


//---------------------------------------------------------------------------
// File_Api_ProtectRoot
//---------------------------------------------------------------------------


_FX NTSTATUS File_Api_ProtectRoot(PROCESS *proc, ULONG64 *parms)
{
    WCHAR* reg_root;
    WCHAR* file_root;
    KIRQL irql;

    if (proc)
        return STATUS_NOT_IMPLEMENTED;

    //
    // we expect to be called only by SbieSvc, or the session leader
    //

    if (PsGetCurrentProcessId() != Api_ServiceProcessId) {
        //if (Session_GetLeadSession(PsGetCurrentProcessId()) != 0)
        return STATUS_ACCESS_DENIED;
    }

    reg_root = (WCHAR *)parms[1];
    file_root = (WCHAR *)parms[2];

    ULONG path_len = wcslen(file_root);
    ULONG len = sizeof(PROTECTED_ROOT) + path_len * sizeof(WCHAR);
    PROTECTED_ROOT *root = Mem_Alloc(Driver_Pool, len);
    if (root) {
        
        root->file_root_len = path_len;
        wmemcpy(root->file_root, file_root, path_len);
        root->file_root[path_len] = L'\0';
        
        path_len = wcslen(reg_root);
        root->reg_root_len = path_len;
        wmemcpy(root->reg_root, reg_root, path_len);
        root->reg_root[path_len] = L'\0';

        KeRaiseIrql(APC_LEVEL, &irql);
        ExAcquireResourceExclusiveLite(File_ProtectedRootsLock, TRUE);
        
        List_Insert_After(&File_ProtectedRoots, NULL, root);
        
        ExReleaseResourceLite(File_ProtectedRootsLock);
        KeLowerIrql(irql);
    }

    return STATUS_SUCCESS;
}


//---------------------------------------------------------------------------
// File_Api_UnprotectRoot
//---------------------------------------------------------------------------


_FX NTSTATUS File_Api_UnprotectRoot(PROCESS* proc, ULONG64* parms)
{
    NTSTATUS status;
    KIRQL irql;
    ULONG len;
    WCHAR reg_root[MAX_REG_ROOT_LEN];
    ULONG reg_root_len;

    if (proc)
        return STATUS_NOT_IMPLEMENTED;

    //
    // we expect to be called only by SbieSvc, or the session leader
    //

    if (PsGetCurrentProcessId() != Api_ServiceProcessId) {
        //if (Session_GetLeadSession(PsGetCurrentProcessId()) != 0)
        return STATUS_ACCESS_DENIED;
    }

    reg_root_len = wcslen((WCHAR *)parms[1]);
    wmemcpy(reg_root, (WCHAR *)parms[1], reg_root_len);
    reg_root[reg_root_len] = L'\0';

    status = STATUS_OBJECT_NAME_NOT_FOUND;

    KeRaiseIrql(APC_LEVEL, &irql);
    ExAcquireResourceExclusiveLite(File_ProtectedRootsLock, TRUE);

    PROTECTED_ROOT *root = List_Head(&File_ProtectedRoots);
    while (root) {

        PROTECTED_ROOT *next_root = List_Next(root);

        if (root->reg_root_len == reg_root_len && _wcsicmp(root->reg_root, reg_root) == 0) {

            List_Remove(&File_ProtectedRoots, root);

            len = sizeof(PROTECTED_ROOT) + root->file_root_len * sizeof(WCHAR);
            Mem_Free(root, len);

            status = STATUS_SUCCESS; // don't break in case a root was added more than once
        }

        root = next_root;
    }

    ExReleaseResourceLite(File_ProtectedRootsLock);
    KeLowerIrql(irql);

    return status;
}
