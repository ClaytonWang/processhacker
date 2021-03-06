/*
 * Process Hacker Plugins -
 *   Online Checks Plugin
 *
 * Copyright (C) 2016-2018 dmex
 *
 * This file is part of Process Hacker.
 *
 * Process Hacker is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Process Hacker is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Process Hacker.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "onlnchk.h"

PPH_OBJECT_TYPE UploadContextType = NULL;
PH_INITONCE UploadContextTypeInitOnce = PH_INITONCE_INIT;
SERVICE_INFO UploadServiceInfo[] =
{ 
    { MENUITEM_HYBRIDANALYSIS_UPLOAD, L"www.hybrid-analysis.com", L"/api/submit", L"file" },
    { MENUITEM_HYBRIDANALYSIS_UPLOAD_SERVICE, L"www.hybrid-analysis.com", L"/api/submit", L"file" },
    { MENUITEM_VIRUSTOTAL_UPLOAD, L"www.virustotal.com", L"???", L"file" },
    { MENUITEM_VIRUSTOTAL_UPLOAD_SERVICE, L"www.virustotal.com", L"???", L"file" },
    { MENUITEM_JOTTI_UPLOAD, L"virusscan.jotti.org", L"/en-US/submit-file?isAjax=true", L"sample-file[]" },
    { MENUITEM_JOTTI_UPLOAD_SERVICE, L"virusscan.jotti.org", L"/en-US/submit-file?isAjax=true", L"sample-file[]" },
};

VOID RaiseUploadError(
    _In_ PUPLOAD_CONTEXT Context,
    _In_ PWSTR Error,
    _In_ ULONG ErrorCode
    )
{
    PPH_STRING message;

    if (!Context->DialogHandle)
        return;

    if (message = PhHttpSocketGetErrorMessage(ErrorCode))
    {
        PhMoveReference(&Context->ErrorString, PhFormatString(
            L"[%lu] %s",
            ErrorCode,
            PhGetString(message)
            ));

        PhDereferenceObject(message);
    }
    else
    {
        PhMoveReference(&Context->ErrorString, PhFormatString(
            L"[%lu] %s",
            ErrorCode,
            Error
            ));
    }

    PostMessage(Context->DialogHandle, UM_ERROR, 0, 0);
}

PSERVICE_INFO GetUploadServiceInfo(
    _In_ ULONG Id
    )
{
    ULONG i;

    for (i = 0; i < ARRAYSIZE(UploadServiceInfo); i++)
    {
        if (UploadServiceInfo[i].Id == Id)
            return &UploadServiceInfo[i];
    }

    return NULL;
}

VOID UploadContextDeleteProcedure(
    _In_ PVOID Object,
    _In_ ULONG Flags
    )
{
    PUPLOAD_CONTEXT context = Object;

    if (context->TaskbarListClass)
    {
        ITaskbarList3_Release(context->TaskbarListClass);
        context->TaskbarListClass = NULL;
    }

    if (context->UploadThreadHandle)
    {
        NtClose(context->UploadThreadHandle);
        context->UploadThreadHandle = NULL;
    }

    PhClearReference(&context->ErrorString);
    PhClearReference(&context->FileName);
    PhClearReference(&context->BaseFileName);
    PhClearReference(&context->WindowFileName);
    PhClearReference(&context->LaunchCommand);
    PhClearReference(&context->Detected);
    PhClearReference(&context->MaxDetected);
    PhClearReference(&context->UploadUrl);
    PhClearReference(&context->ReAnalyseUrl);
    PhClearReference(&context->FirstAnalysisDate);
    PhClearReference(&context->LastAnalysisDate);
    PhClearReference(&context->LastAnalysisUrl);
    PhClearReference(&context->LastAnalysisAgo);
}

VOID TaskDialogFreeContext(
    _In_ PUPLOAD_CONTEXT Context
    )
{
    // Reset Taskbar progress state(s)
    if (Context->TaskbarListClass)
        ITaskbarList3_SetProgressState(Context->TaskbarListClass, PhMainWndHandle, TBPF_NOPROGRESS);

    if (Context->TaskbarListClass)
        ITaskbarList3_SetProgressState(Context->TaskbarListClass, Context->DialogHandle, TBPF_NOPROGRESS);

    PhDereferenceObject(Context);
}

VOID TaskDialogCreateIcons(
    _In_ PUPLOAD_CONTEXT Context
    )
{
    Context->IconLargeHandle = PH_LOAD_SHARED_ICON_LARGE(PhInstanceHandle, MAKEINTRESOURCE(PHAPP_IDI_PROCESSHACKER));
    Context->IconSmallHandle = PH_LOAD_SHARED_ICON_SMALL(PhInstanceHandle, MAKEINTRESOURCE(PHAPP_IDI_PROCESSHACKER));

    SendMessage(Context->DialogHandle, WM_SETICON, ICON_SMALL, (LPARAM)Context->IconSmallHandle);
    SendMessage(Context->DialogHandle, WM_SETICON, ICON_BIG, (LPARAM)Context->IconLargeHandle);
}

PPH_STRING UpdateVersionString(
    VOID
    )
{
    ULONG majorVersion;
    ULONG minorVersion;
    ULONG revisionVersion;
    PPH_STRING currentVersion;
    PPH_STRING versionHeader = NULL;

    PhGetPhVersionNumbers(&majorVersion, &minorVersion, NULL, &revisionVersion);

    if (currentVersion = PhFormatString(L"%lu.%lu.%lu", majorVersion, minorVersion, revisionVersion))
    {
        versionHeader = PhConcatStrings2(L"ProcessHacker-Build: ", currentVersion->Buffer);
        PhDereferenceObject(currentVersion);
    }

    return versionHeader;
}

NTSTATUS HashFileAndResetPosition(
    _In_ HANDLE FileHandle,
    _In_ PLARGE_INTEGER FileSize,
    _In_ PH_HASH_ALGORITHM Algorithm,
    _Out_ PPH_STRING *HashString
    )
{
    NTSTATUS status;
    IO_STATUS_BLOCK iosb;
    PH_HASH_CONTEXT hashContext;
    PPH_STRING hashString = NULL;
    ULONG64 bytesRemaining;
    FILE_POSITION_INFORMATION positionInfo;
    LONG priority;
    LONG newpriority;
    IO_PRIORITY_HINT ioPriority;
    IO_PRIORITY_HINT newioPriority;
    UCHAR buffer[PAGE_SIZE];
    
    bytesRemaining = FileSize->QuadPart;

    newpriority = THREAD_PRIORITY_LOWEST;
    newioPriority = IoPriorityVeryLow;
    NtQueryInformationThread(NtCurrentThread(), ThreadBasePriority, &priority, sizeof(LONG), NULL);
    NtQueryInformationThread(NtCurrentThread(), ThreadIoPriority, &ioPriority, sizeof(IO_PRIORITY_HINT), NULL);
    NtSetInformationThread(NtCurrentThread(), ThreadBasePriority, &newpriority, sizeof(LONG));
    NtSetInformationThread(NtCurrentThread(), ThreadIoPriority, &newioPriority, sizeof(IO_PRIORITY_HINT));

    PhInitializeHash(&hashContext, Algorithm);

    while (bytesRemaining)
    {
        status = NtReadFile(
            FileHandle,
            NULL,
            NULL,
            NULL,
            &iosb,
            buffer,
            sizeof(buffer),
            NULL,
            NULL
            );

        if (!NT_SUCCESS(status))
            break;

        PhUpdateHash(&hashContext, buffer, (ULONG)iosb.Information);
        bytesRemaining -= (ULONG)iosb.Information;
    }

    if (status == STATUS_END_OF_FILE)
        status = STATUS_SUCCESS;

    if (NT_SUCCESS(status))
    {
        UCHAR hash[32];

        switch (Algorithm)
        {
        case Md5HashAlgorithm:
            PhFinalHash(&hashContext, hash, 16, NULL);
            *HashString = PhBufferToHexString(hash, 16);
            break;
        case Sha1HashAlgorithm:
            PhFinalHash(&hashContext, hash, 20, NULL);
            *HashString = PhBufferToHexString(hash, 20);
            break;
        case Sha256HashAlgorithm:
            PhFinalHash(&hashContext, hash, 32, NULL);
            *HashString = PhBufferToHexString(hash, 32);
            break;
        }

        positionInfo.CurrentByteOffset.QuadPart = 0;
        status = NtSetInformationFile(
            FileHandle,
            &iosb,
            &positionInfo,
            sizeof(FILE_POSITION_INFORMATION),
            FilePositionInformation
            );
    }

    PhSetThreadBasePriority(NtCurrentThread(), priority);
    PhSetThreadIoPriority(NtCurrentThread(), ioPriority);

    return status;
}

PPH_BYTES PerformSubRequest(
    _In_ PUPLOAD_CONTEXT Context,
    _In_ PWSTR HostName,
    _In_ PWSTR ObjectName
    )
{
    PPH_BYTES result = NULL;
    PPH_HTTP_CONTEXT httpContext = NULL;
    PPH_STRING phVersion;
    PPH_STRING userAgent;

    phVersion = PhGetPhVersion();
    userAgent = PhConcatStrings2(L"ProcessHacker_", phVersion->Buffer);

    if (!PhHttpSocketCreate(&httpContext, userAgent->Buffer))
    {
        RaiseUploadError(Context, L"Unable to create the http socket.", GetLastError());
        goto CleanupExit;
    }

    if (!PhHttpSocketConnect(httpContext, HostName, PH_HTTP_DEFAULT_HTTPS_PORT))
    {
        RaiseUploadError(Context, L"Unable to connect to the service.", GetLastError());
        goto CleanupExit;
    }

    if (!PhHttpSocketBeginRequest(
        httpContext,
        NULL,
        ObjectName,
        PH_HTTP_FLAG_REFRESH | PH_HTTP_FLAG_SECURE
        ))
    {
        RaiseUploadError(Context, L"Unable to create the request.", GetLastError());
        goto CleanupExit;
    }

    if (!PhHttpSocketSendRequest(httpContext, NULL, 0))
    {
        RaiseUploadError(Context, L"Unable to send the request.", GetLastError());
        goto CleanupExit;
    }

    if (!PhHttpSocketEndRequest(httpContext))
    {
        RaiseUploadError(Context, L"Unable to receive the request.", GetLastError());
        goto CleanupExit;
    }

    if (!(result = PhHttpSocketDownloadString(httpContext, FALSE)))
    {
        RaiseUploadError(Context, L"Unable to download the response.", GetLastError());
        goto CleanupExit;
    }

CleanupExit:

    if (httpContext)
        PhHttpSocketDestroy(httpContext);

    PhClearReference(&phVersion);
    PhClearReference(&userAgent);

    return result;
}

BOOLEAN UploadGetFileArchitecture(
    _In_ HANDLE FileHandle, 
    _Out_ PUSHORT FileArchitecture
    )
{
    NTSTATUS status;
    PH_MAPPED_IMAGE mappedImage;

    if (!NT_SUCCESS(status = PhLoadMappedImage(NULL, FileHandle, TRUE, &mappedImage)))
        return FALSE;

    *FileArchitecture = mappedImage.NtHeaders->FileHeader.Machine;

    PhUnloadMappedImage(&mappedImage);

    return TRUE;
}

NTSTATUS UploadFileThreadStart(
    _In_ PVOID Parameter
    )
{
    NTSTATUS status = STATUS_SUCCESS;
    ULONG httpStatus = 0;
    ULONG httpPostSeed = 0;
    ULONG totalUploadLength = 0;
    ULONG totalUploadedLength = 0;
    ULONG totalPostHeaderWritten = 0;
    ULONG totalPostFooterWritten = 0;
    ULONG totalWriteLength = 0;
    LARGE_INTEGER timeNow;
    LARGE_INTEGER timeStart;
    ULONG64 timeTicks = 0;
    ULONG64 timeBitsPerSecond = 0;
    HANDLE fileHandle = NULL;
    IO_STATUS_BLOCK isb;
    PPH_HTTP_CONTEXT httpContext = NULL;
    PPH_STRING phVersion = NULL;
    PPH_STRING userAgent = NULL;
    PPH_STRING httpHostName = NULL;
    PPH_STRING httpHostPath = NULL;
    USHORT httpHostPort = 0;
    PSERVICE_INFO serviceInfo = NULL;
    PPH_STRING postBoundary = NULL;
    PPH_BYTES asciiPostData = NULL;
    PPH_BYTES asciiFooterData = NULL;
    PH_STRING_BUILDER httpRequestHeaders = { 0 };
    PH_STRING_BUILDER httpPostHeader = { 0 };
    PH_STRING_BUILDER httpPostFooter = { 0 };
    PUPLOAD_CONTEXT context = (PUPLOAD_CONTEXT)Parameter;

    serviceInfo = GetUploadServiceInfo(context->Service);

    if (!NT_SUCCESS(status = PhCreateFileWin32(
        &fileHandle,
        PhGetString(context->FileName),
        FILE_GENERIC_READ,
        0,
        FILE_SHARE_READ | FILE_SHARE_DELETE,
        FILE_OPEN,
        FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT
        )))
    {
        RaiseUploadError(context, L"Unable to open the file", RtlNtStatusToDosError(status));
        goto CleanupExit;
    }

    // Create a user agent string.
    phVersion = PhGetPhVersion();
    userAgent = PhConcatStrings2(L"ProcessHacker_", phVersion->Buffer);

    if (!PhHttpSocketCreate(&httpContext, userAgent->Buffer))
    {
        PhClearReference(&phVersion);
        PhClearReference(&userAgent);
        goto CleanupExit;
    }

    PhClearReference(&phVersion);
    PhClearReference(&userAgent);

    if (!PhHttpSocketParseUrl(
        context->UploadUrl,
        &httpHostName,
        &httpHostPath,
        &httpHostPort
        ))
    {
        context->ErrorCode = GetLastError();
        goto CleanupExit;
    }

    if (!PhHttpSocketConnect(
        httpContext,
        PhGetString(httpHostName),
        httpHostPort
        ))
    {
        RaiseUploadError(context, L"Unable to connect to the service", GetLastError());
        goto CleanupExit;
    }

    if (!PhHttpSocketBeginRequest(
        httpContext,
        L"POST",
        PhGetString(httpHostPath),
        PH_HTTP_FLAG_REFRESH | (httpHostPort == PH_HTTP_DEFAULT_HTTPS_PORT ? PH_HTTP_FLAG_SECURE : 0)
        ))
    {
        RaiseUploadError(context, L"Unable to create the request", GetLastError());
        goto CleanupExit;
    }

    PhInitializeStringBuilder(&httpRequestHeaders, DOS_MAX_PATH_LENGTH);
    PhInitializeStringBuilder(&httpPostHeader, DOS_MAX_PATH_LENGTH);
    PhInitializeStringBuilder(&httpPostFooter, DOS_MAX_PATH_LENGTH);

    // HTTP request boundary string.
    postBoundary = PhFormatString(
        L"--%I64u",
        (ULONG64)RtlRandomEx(&httpPostSeed) | ((ULONG64)RtlRandomEx(&httpPostSeed) << 31)
        );

    // HTTP request header string.
    PhAppendFormatStringBuilder(
        &httpRequestHeaders,
        L"Content-Type: multipart/form-data; boundary=%s\r\n",
        postBoundary->Buffer
        );

    if (context->Service == MENUITEM_HYBRIDANALYSIS_UPLOAD || context->Service == MENUITEM_HYBRIDANALYSIS_UPLOAD_SERVICE)
    {
        USHORT machineType;
        USHORT environmentId;

        if (!UploadGetFileArchitecture(fileHandle, &machineType))
        {
            RaiseUploadError(context, L"Unable to create the request.", GetLastError());
            goto CleanupExit;
        }

        switch (machineType)
        {
        case IMAGE_FILE_MACHINE_I386:
            environmentId = 100;
            break;
        case IMAGE_FILE_MACHINE_AMD64:
            environmentId = 120;
            break;
        default:
            {
                RaiseUploadError(context, L"File architecture not supported.", GetLastError());
                goto CleanupExit;
            }
        }

        {
            PPH_BYTES serviceHash;
            PPH_BYTES networkHash;
            PPH_STRING resourceNameString;
            PPH_STRING resourceHashString;

            serviceHash = VirusTotalGetCachedDbHash(&ServiceObjectDbHash);
            networkHash = VirusTotalGetCachedDbHash(&NetworkObjectDbHash);

            resourceNameString = PhZeroExtendToUtf16(serviceHash->Buffer);
            resourceHashString = PhZeroExtendToUtf16(networkHash->Buffer);
            PhHttpSocketSetCredentials(httpContext, PhGetString(resourceHashString), PhGetString(resourceNameString));
            PhClearReference(&resourceHashString);
            PhClearReference(&resourceNameString);
            PhClearReference(&networkHash);
            PhClearReference(&serviceHash);
        }

        // POST boundary header.
        PhAppendFormatStringBuilder(
            &httpPostHeader,
            L"--%s\r\nContent-Disposition: form-data; name=\"environmentId\"\r\n\r\n%hu\r\n",
            postBoundary->Buffer,
            environmentId
            );
        PhAppendFormatStringBuilder(
            &httpPostHeader,
            L"--%s\r\nContent-Disposition: form-data; name=\"nosharevt\"\r\n\r\n1\r\n",
            postBoundary->Buffer
            );
        PhAppendFormatStringBuilder(
            &httpPostHeader,
            L"--%s\r\nContent-Disposition: form-data; name=\"file\"; filename=\"%s\"\r\n\r\n",
            postBoundary->Buffer,
            PhGetStringOrEmpty(context->BaseFileName)
            );

        // POST boundary footer.
        PhAppendFormatStringBuilder(
            &httpPostFooter,
            L"\r\n--%s--\r\n",
            postBoundary->Buffer
            );
    }
    else if (context->Service == MENUITEM_JOTTI_UPLOAD)
    {
        // POST boundary header.
        PhAppendFormatStringBuilder(
            &httpPostHeader,
            L"\r\n--%s\r\n",
            postBoundary->Buffer
            );
        PhAppendFormatStringBuilder(
            &httpPostHeader,
            L"Content-Disposition: form-data; name=\"MAX_FILE_SIZE\"\r\n\r\n268435456\r\n"
            );
        PhAppendFormatStringBuilder(
            &httpPostHeader,
            L"--%s\r\n",
            postBoundary->Buffer
            );
        PhAppendFormatStringBuilder(
            &httpPostHeader,
            L"Content-Disposition: form-data; name=\"%s\"; filename=\"%s\"\r\n",
            serviceInfo->FileNameFieldName,
            PhGetStringOrEmpty(context->BaseFileName)
            );
        PhAppendFormatStringBuilder(
            &httpPostHeader,
            L"Content-Type: application/x-msdownload\r\n\r\n"
            );

        // POST boundary footer.
        PhAppendFormatStringBuilder(
            &httpPostFooter,
            L"\r\n--%s--\r\n",
            postBoundary->Buffer
            );
    }
    else
    {
        // POST boundary header
        PhAppendFormatStringBuilder(
            &httpPostHeader,
            L"--%s\r\n",
            postBoundary->Buffer
            );
        PhAppendFormatStringBuilder(
            &httpPostHeader,
            L"Content-Disposition: form-data; name=\"%s\"; filename=\"%s\"\r\n",
            serviceInfo->FileNameFieldName,
            PhGetStringOrEmpty(context->BaseFileName)
            );
        PhAppendFormatStringBuilder(
            &httpPostHeader,
            L"Content-Type: application/octet-stream\r\n\r\n"
            );

        // POST boundary footer
        PhAppendFormatStringBuilder(
            &httpPostFooter,
            L"\r\n--%s--\r\n\r\n",
            postBoundary->Buffer
            );
    }

    // add headers
    if (!PhHttpSocketAddRequestHeaders(
        httpContext,
        httpRequestHeaders.String->Buffer,
        (ULONG)httpRequestHeaders.String->Length / sizeof(WCHAR)
        ))
    {
        RaiseUploadError(context, L"Unable to add request headers", GetLastError());
        goto CleanupExit;
    }

    if (context->Service == MENUITEM_JOTTI_UPLOAD)
    {
        PPH_STRING ajaxHeader = PhCreateString(L"X-Requested-With: XMLHttpRequest");

        PhHttpSocketAddRequestHeaders(
            httpContext,
            ajaxHeader->Buffer,
            (ULONG)ajaxHeader->Length / sizeof(WCHAR)
            );

        PhDereferenceObject(ajaxHeader);
    }

    // Calculate the total request length.
    totalUploadLength = (ULONG)httpPostHeader.String->Length / sizeof(WCHAR) + context->TotalFileLength + (ULONG)httpPostFooter.String->Length / sizeof(WCHAR);

    // Send the request.
    if (!PhHttpSocketSendRequest(httpContext, NULL, totalUploadLength))
    {
        RaiseUploadError(context, L"Unable to send the request", GetLastError());
        goto CleanupExit;
    }

    // Convert to ASCII
    asciiPostData = PhConvertUtf16ToAscii(httpPostHeader.String->Buffer, '-');
    asciiFooterData = PhConvertUtf16ToAscii(httpPostFooter.String->Buffer, '-');

    // Start the clock.
    PhQuerySystemTime(&timeStart);

    // Write the header
    if (!PhHttpSocketWriteData(
        httpContext,
        asciiPostData->Buffer,
        (ULONG)asciiPostData->Length,
        &totalPostHeaderWritten
        ))
    {
        RaiseUploadError(context, L"Unable to write the post header", GetLastError());
        goto CleanupExit;
    }

    PPH_STRING msg = PhFormatString(L"Uploading %s...", PhGetStringOrEmpty(context->BaseFileName));
    SendMessage(context->DialogHandle, TDM_SET_MARQUEE_PROGRESS_BAR, FALSE, 0);
    SendMessage(context->DialogHandle, TDM_UPDATE_ELEMENT_TEXT, TDE_MAIN_INSTRUCTION, (LPARAM)PhGetString(msg));
    PhDereferenceObject(msg);

    if (context->TaskbarListClass)
    {
        ITaskbarList3_SetProgressState(
            context->TaskbarListClass,
            PhMainWndHandle,
            TBPF_NORMAL
            );
    }

    while (TRUE)
    {
        BYTE buffer[PAGE_SIZE];

        if (!context->UploadThreadHandle)
            goto CleanupExit;

        if (!NT_SUCCESS(status = NtReadFile(
            fileHandle,
            NULL,
            NULL,
            NULL,
            &isb,
            buffer,
            PAGE_SIZE,
            NULL,
            NULL
            )))
        {
            break;
        }

        if (!PhHttpSocketWriteData(httpContext, buffer, (ULONG)isb.Information, &totalWriteLength))
        {
            RaiseUploadError(context, L"Unable to upload the file data", GetLastError());
            goto CleanupExit;
        }

        PhQuerySystemTime(&timeNow);

        totalUploadedLength += totalWriteLength;
        timeTicks = (timeNow.QuadPart - timeStart.QuadPart) / PH_TICKS_PER_SEC;
        timeBitsPerSecond = totalUploadedLength / __max(timeTicks, 1);

        {
            FLOAT percent = ((FLOAT)totalUploadedLength / context->TotalFileLength * 100);
            PPH_STRING totalLength = PhFormatSize(context->TotalFileLength, -1);
            PPH_STRING totalUploaded = PhFormatSize(totalUploadedLength, -1);
            PPH_STRING totalSpeed = PhFormatSize(timeBitsPerSecond, -1);
            PPH_STRING statusMessage = PhFormatString(
                L"Uploaded: %s of %s (%.0f%%)\r\nSpeed: %s/s",
                PhGetStringOrEmpty(totalUploaded),
                PhGetStringOrEmpty(totalLength),
                percent,
                PhGetStringOrEmpty(totalSpeed)
                );

            SendMessage(context->DialogHandle, TDM_UPDATE_ELEMENT_TEXT, TDE_CONTENT, (LPARAM)statusMessage->Buffer);
            SendMessage(context->DialogHandle, TDM_SET_PROGRESS_BAR_POS, (WPARAM)percent, 0);

            if (context->TaskbarListClass)
            {
                ITaskbarList3_SetProgressValue(
                    context->TaskbarListClass, 
                    context->DialogHandle, 
                    totalUploadedLength, 
                    context->TotalFileLength
                    );
            }

            PhDereferenceObject(statusMessage);
            PhDereferenceObject(totalSpeed);
            PhDereferenceObject(totalUploaded);
            PhDereferenceObject(totalLength);
        }
    }

    // Write the footer bytes
    if (!PhHttpSocketWriteData(
        httpContext,
        asciiFooterData->Buffer,
        (ULONG)asciiFooterData->Length,
        &totalPostFooterWritten
        ))
    {
        RaiseUploadError(context, L"Unable to write the post footer", GetLastError());
        goto CleanupExit;
    }

    if (!PhHttpSocketEndRequest(httpContext))
    {
        RaiseUploadError(context, L"Unable to receive the response", GetLastError());
        goto CleanupExit;
    }

    if (!PhHttpSocketQueryHeaderUlong(
        httpContext,
        PH_HTTP_QUERY_STATUS_CODE,
        &httpStatus
        ))
    {
        RaiseUploadError(context, L"Unable to query http headers", GetLastError());
        goto CleanupExit;
    }

    if (httpStatus == PH_HTTP_STATUS_OK || httpStatus == PH_HTTP_STATUS_REDIRECT_METHOD || httpStatus == PH_HTTP_STATUS_REDIRECT)
    {
        switch (context->Service)
        {
        case MENUITEM_HYBRIDANALYSIS_UPLOAD:
        case MENUITEM_HYBRIDANALYSIS_UPLOAD_SERVICE:
            {
                PPH_BYTES jsonString;
                PVOID jsonRootObject;

                if (!(jsonString = PhHttpSocketDownloadString(httpContext, FALSE)))
                {
                    RaiseUploadError(context, L"Unable to download the response.", GetLastError());
                    goto CleanupExit;
                }

                if (jsonRootObject = PhCreateJsonParser(jsonString->Buffer))
                {
                    INT64 errorCode = PhGetJsonValueAsLong64(jsonRootObject, "response_code");

                    if (errorCode == 0)
                    {
                        PVOID jsonResponse;
                        PPH_STRING jsonHashString = NULL;
 
                        if (jsonResponse = PhGetJsonObject(jsonRootObject, "response"))
                            jsonHashString = PhGetJsonValueAsString(jsonResponse, "sha256");

                        if (jsonHashString)
                        {
                            PhMoveReference(&context->LaunchCommand, PhFormatString(
                                L"https://www.hybrid-analysis.com/sample/%s",
                                context->FileHash ? PhGetString(context->FileHash) : PhGetString(jsonHashString)
                                ));

                            PhDereferenceObject(jsonHashString);
                        }
                    }
                    else
                    {
                        RaiseUploadError(context, L"Hybrid Analysis API error.", (ULONG)errorCode);
                        PhDereferenceObject(jsonString);
                        goto CleanupExit;
                    }

                    PhFreeJsonParser(jsonRootObject);
                }
                else
                {
                    RaiseUploadError(context, L"Unable to complete the request.", RtlNtStatusToDosError(STATUS_FAIL_CHECK));
                    goto CleanupExit;
                }

                PhDereferenceObject(jsonString);
            }
            break;
        case MENUITEM_VIRUSTOTAL_UPLOAD:
        case MENUITEM_VIRUSTOTAL_UPLOAD_SERVICE:
            {
                PPH_BYTES jsonString;
                PVOID jsonRootObject;

                if (!(jsonString = PhHttpSocketDownloadString(httpContext, FALSE)))
                {
                    RaiseUploadError(context, L"Unable to complete the request", GetLastError());
                    goto CleanupExit;
                }

                if (jsonRootObject = PhCreateJsonParser(jsonString->Buffer))
                {
                    INT64 errorCode = PhGetJsonValueAsLong64(jsonRootObject, "response_code");
                    //PhGetJsonValueAsString(jsonRootObject, "scan_id");
                    //PhGetJsonValueAsString(jsonRootObject, "verbose_msg");

                    if (errorCode != 1)
                    {
                        RaiseUploadError(context, L"VirusTotal API error", (ULONG)errorCode);
                        PhDereferenceObject(jsonString);
                        goto CleanupExit;
                    }
                    else
                    {
                        PhMoveReference(&context->LaunchCommand, PhGetJsonValueAsString(jsonRootObject, "permalink"));
                    }

                    PhFreeJsonParser(jsonRootObject);
                }
                else
                {
                    RaiseUploadError(context, L"Unable to complete the request", RtlNtStatusToDosError(STATUS_FAIL_CHECK));
                    goto CleanupExit;
                }

                PhDereferenceObject(jsonString);
            }
            break;
        case MENUITEM_JOTTI_UPLOAD:
        case MENUITEM_JOTTI_UPLOAD_SERVICE:
            {
                PPH_BYTES jsonString;
                PVOID jsonRootObject;

                if (!(jsonString = PhHttpSocketDownloadString(httpContext, FALSE)))
                {
                    RaiseUploadError(context, L"Unable to complete the request", GetLastError());
                    goto CleanupExit;
                }

                if (jsonRootObject = PhCreateJsonParser(jsonString->Buffer))
                {
                    PPH_STRING redirectUrl;

                    if (redirectUrl = PhGetJsonValueAsString(jsonRootObject, "redirecturl"))
                    {
                        PhMoveReference(&context->LaunchCommand, PhFormatString(L"http://virusscan.jotti.org%s", redirectUrl->Buffer));
                        PhDereferenceObject(redirectUrl);
                    }

                    PhFreeJsonParser(jsonRootObject);
                }

                PhDereferenceObject(jsonString);
            }
            break;
        }
    }
    else
    {
        RaiseUploadError(context, L"Unable to complete the request", STATUS_FVE_PARTIAL_METADATA);
        goto CleanupExit;
    }

    if (!context->UploadThreadHandle)
        goto CleanupExit;

    if (!PhIsNullOrEmptyString(context->LaunchCommand))
    {
        PostMessage(context->DialogHandle, UM_LAUNCH, 0, 0);
    }
    else
    {
        RaiseUploadError(context, L"Unable to complete the Launch request (please try again after a few minutes)", ERROR_INVALID_DATA);
    }

CleanupExit:

    if (httpContext)
        PhHttpSocketDestroy(httpContext);

    // Reset Taskbar progress state(s)
    if (context->TaskbarListClass)
    {
        ITaskbarList3_SetProgressState(context->TaskbarListClass, PhMainWndHandle, TBPF_NOPROGRESS);
        ITaskbarList3_SetProgressState(context->TaskbarListClass, context->DialogHandle, TBPF_NOPROGRESS);
    }

    if (postBoundary)
        PhDereferenceObject(postBoundary);

    if (asciiFooterData)
        PhDereferenceObject(asciiFooterData);

    if (asciiPostData)
        PhDereferenceObject(asciiPostData);

    if (httpPostFooter.String)
        PhDeleteStringBuilder(&httpPostFooter);

    if (httpPostHeader.String)
        PhDeleteStringBuilder(&httpPostHeader);

    if (httpRequestHeaders.String)
        PhDeleteStringBuilder(&httpRequestHeaders);

    if (fileHandle)
        NtClose(fileHandle);

    PhDereferenceObject(context);

    return status;
}

NTSTATUS UploadCheckThreadStart(
    _In_ PVOID Parameter
    )
{
    NTSTATUS status = STATUS_SUCCESS;
    BOOLEAN fileExists = FALSE;
    LARGE_INTEGER fileSize64;
    PPH_BYTES subRequestBuffer = NULL;
    PSERVICE_INFO serviceInfo = NULL;
    PPH_STRING subObjectName = NULL;
    HANDLE fileHandle = NULL;
    PUPLOAD_CONTEXT context = (PUPLOAD_CONTEXT)Parameter;

    //context->Extension = VirusTotalGetCachedResult(context->FileName);
    serviceInfo = GetUploadServiceInfo(context->Service);

    if (!NT_SUCCESS(status = PhCreateFileWin32(
        &fileHandle,
        context->FileName->Buffer,
        FILE_GENERIC_READ,
        0,
        FILE_SHARE_READ | FILE_SHARE_DELETE,
        FILE_OPEN,
        FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT
        )))
    {
        RaiseUploadError(context, L"Unable to open the file", RtlNtStatusToDosError(status));
        goto CleanupExit;
    }

    if (NT_SUCCESS(status = PhGetFileSize(fileHandle, &fileSize64)))
    {
        if (context->Service == MENUITEM_VIRUSTOTAL_UPLOAD || 
            context->Service == MENUITEM_VIRUSTOTAL_UPLOAD_SERVICE)
        {
            if (fileSize64.QuadPart < 32 * 1024 * 1024)
            {
                context->VtApiUpload = TRUE;
            }
            
            if (fileSize64.QuadPart > 128 * 1024 * 1024) // 128 MB
            {
                RaiseUploadError(context, L"The file is too large (over 128 MB)", ERROR_FILE_TOO_LARGE);
                goto CleanupExit;
            }
        }
        else if (
            context->Service == MENUITEM_HYBRIDANALYSIS_UPLOAD ||
            context->Service == MENUITEM_HYBRIDANALYSIS_UPLOAD_SERVICE
            )
        {
            if (fileSize64.QuadPart > 128 * 1024 * 1024) // 128 MB
            {
                RaiseUploadError(context, L"The file is too large (over 128 MB)", ERROR_FILE_TOO_LARGE);
                goto CleanupExit;
            }
        }
        else
        {
            if (fileSize64.QuadPart > 20 * 1024 * 1024) // 20 MB
            {
                RaiseUploadError(context, L"The file is too large (over 20 MB)", ERROR_FILE_TOO_LARGE);
                goto CleanupExit;
            }
        }

        context->FileSize = PhFormatSize(fileSize64.QuadPart, -1);
        context->TotalFileLength = fileSize64.LowPart;
    }

    switch (context->Service)
    {
    case MENUITEM_VIRUSTOTAL_UPLOAD:
    case MENUITEM_VIRUSTOTAL_UPLOAD_SERVICE:
        {
            PPH_STRING tempHashString = NULL;
            PSTR uploadUrl = NULL;
            PSTR quote = NULL;
            PVOID rootJsonObject;

            if (!NT_SUCCESS(status = HashFileAndResetPosition(fileHandle, &fileSize64, Sha256HashAlgorithm, &tempHashString)))
            {
                RaiseUploadError(context, L"Unable to hash the file", RtlNtStatusToDosError(status));
                goto CleanupExit;
            }

            context->FileHash = tempHashString;
            subObjectName = PhConcatStrings2(L"/file/upload/?sha256=", PhGetString(context->FileHash));

            PhMoveReference(&context->LaunchCommand, PhFormatString(
                L"https://www.virustotal.com/file/%s/analysis/",
                PhGetString(context->FileHash)
                ));

            if (!(subRequestBuffer = PerformSubRequest(
                context,
                serviceInfo->HostName,
                subObjectName->Buffer
                )))
            {
                goto CleanupExit;
            }

            if (rootJsonObject = PhCreateJsonParser(subRequestBuffer->Buffer))
            {  
                if (context->FileExists = PhGetJsonObjectBool(rootJsonObject, "file_exists"))
                {
                    INT64 detected = 0;
                    INT64 detectedMax = 0;
                    PVOID detectionRatio;

                    if (detectionRatio = PhGetJsonObject(rootJsonObject, "detection_ratio"))
                    {
                        detected = PhGetJsonArrayLong64(detectionRatio, 0);
                        detectedMax = PhGetJsonArrayLong64(detectionRatio, 1);
                    }

                    context->Detected = PhFormatString(L"%I64d", detected);
                    context->MaxDetected = PhFormatString(L"%I64d", detectedMax);
                    context->UploadUrl = PhGetJsonValueAsString(rootJsonObject, "upload_url");
                    context->ReAnalyseUrl = PhGetJsonValueAsString(rootJsonObject, "reanalyse_url");
                    context->LastAnalysisUrl = PhGetJsonValueAsString(rootJsonObject, "last_analysis_url");
                    context->FirstAnalysisDate = PhGetJsonValueAsString(rootJsonObject, "first_analysis_date");
                    context->LastAnalysisDate = PhGetJsonValueAsString(rootJsonObject, "last_analysis_date");
                    context->LastAnalysisAgo = PhGetJsonValueAsString(rootJsonObject, "last_analysis_ago");

                    PhMoveReference(&context->FirstAnalysisDate, VirusTotalStringToTime(context->FirstAnalysisDate));
                    PhMoveReference(&context->LastAnalysisDate, VirusTotalStringToTime(context->LastAnalysisDate));
                    
                    if (!PhIsNullOrEmptyString(context->ReAnalyseUrl))
                    {
                        PhMoveReference(&context->ReAnalyseUrl, PhFormatString(
                            L"%s%s", 
                            L"https://www.virustotal.com",
                            PhGetString(context->ReAnalyseUrl)
                            ));
                    }

                    if (context->VtApiUpload)
                    {
                        PPH_BYTES resource = VirusTotalGetCachedDbHash(&ProcessObjectDbHash);

                        PhMoveReference(&context->UploadUrl, PhFormatString(
                            L"%s%s?\x0061\x0070\x0069\x006B\x0065\x0079=%S&resource=%s",
                            L"https://www.virustotal.com",
                            L"/vtapi/v2/file/scan",
                            resource->Buffer,
                            PhGetString(context->FileHash)
                            ));

                        PhClearReference(&resource);
                    }

                    if (!PhIsNullOrEmptyString(context->UploadUrl))
                    {
                        PostMessage(context->DialogHandle, UM_EXISTS, 0, 0);
                    }
                    else
                    {
                        RaiseUploadError(context, L"Unable to parse the UploadUrl.", RtlNtStatusToDosError(STATUS_FAIL_CHECK));
                    }
                }
                else
                {
                    context->UploadUrl = PhGetJsonValueAsString(rootJsonObject, "upload_url");

                    // No file found... Start the upload.
                    if (!PhIsNullOrEmptyString(context->UploadUrl))
                    {
                        PostMessage(context->DialogHandle, UM_UPLOAD, 0, 0);
                    }
                    else
                    {
                        RaiseUploadError(context, L"Received invalid response.", RtlNtStatusToDosError(STATUS_FAIL_CHECK));
                    }
                }
 
                PhFreeJsonParser(rootJsonObject);
            }
            else
            {
                RaiseUploadError(context, L"Unable to parse the response.", RtlNtStatusToDosError(STATUS_FAIL_CHECK));
            }
        }
        break;
    case MENUITEM_JOTTI_UPLOAD:
    case MENUITEM_JOTTI_UPLOAD_SERVICE:
        {
            // Create the default upload URL.
            context->UploadUrl = PhFormatString(L"https://%s%s", serviceInfo->HostName, serviceInfo->UploadObjectName);

            // Start the upload.
            PostMessage(context->DialogHandle, UM_UPLOAD, 0, 0);
        }
        break;
    case MENUITEM_HYBRIDANALYSIS_UPLOAD:
    case MENUITEM_HYBRIDANALYSIS_UPLOAD_SERVICE:
        {
            // Create the default upload URL.
            context->UploadUrl = PhFormatString(L"https://%s%s", serviceInfo->HostName, serviceInfo->UploadObjectName);

            PostMessage(context->DialogHandle, UM_UPLOAD, 0, 0);
        }
    }

CleanupExit:

    PhClearReference(&subObjectName);
    PhClearReference(&subRequestBuffer);

    if (fileHandle)
        NtClose(fileHandle);

    PhDereferenceObject(context);

    return status;
}

NTSTATUS UploadRecheckThreadStart(
    _In_ PVOID Parameter
    )
{
    PUPLOAD_CONTEXT context = (PUPLOAD_CONTEXT)Parameter;
    PVIRUSTOTAL_API_RESPONSE response;
    
    response = VirusTotalRequestFileReScan(context->FileHash);

    if (response->ResponseCode == 1)
        PhMoveReference(&context->ReAnalyseUrl, response->PermaLink); 
    else
        RaiseUploadError(context, L"VirusTotal API error", (ULONG)response->ResponseCode);

    if (!PhIsNullOrEmptyString(context->ReAnalyseUrl))
    {
        PhShellExecute(NULL, PhGetString(context->ReAnalyseUrl), NULL);
        //PostMessage(context->DialogHandle, UM_LAUNCH, 0, 0);
    }
    else
    {
        RaiseUploadError(context, L"Unable to complete the ReAnalyse request (please try again after a few minutes)", ERROR_INVALID_DATA);
    }

    return STATUS_SUCCESS;
}


LRESULT CALLBACK TaskDialogSubclassProc(
    _In_ HWND hwndDlg,
    _In_ UINT uMsg,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam,
    _In_ UINT_PTR uIdSubclass,
    _In_ ULONG_PTR dwRefData
    )
{
    PUPLOAD_CONTEXT context = (PUPLOAD_CONTEXT)dwRefData;

    switch (uMsg)
    {
    case WM_NCDESTROY:
        {
            TaskDialogFreeContext(context);

            RemoveWindowSubclass(hwndDlg, TaskDialogSubclassProc, uIdSubclass);
        }
        break;
    case UM_UPLOAD:
        {
            ShowVirusTotalProgressDialog(context);
        }
        break;
    case UM_EXISTS:
        {
            switch (PhGetIntegerSetting(SETTING_NAME_VIRUSTOTAL_DEFAULT_ACTION))
            {
            //default:
            case 1:
                {
                    ShowVirusTotalProgressDialog(context);
                }
                break;
            case 2:
                {
                    if (!PhIsNullOrEmptyString(context->ReAnalyseUrl))
                        PhShellExecute(hwndDlg, PhGetString(context->ReAnalyseUrl), NULL);
                }
                break;
            case 3:
                {
                    if (!PhIsNullOrEmptyString(context->LaunchCommand))
                    {
                        PhShellExecute(hwndDlg, PhGetString(context->LaunchCommand), NULL);
                    }
                }
                break;
            default:
                {
                    ShowFileFoundDialog(context);
                }
            }
        }
        break;
    case UM_LAUNCH:
        {
            if (!PhIsNullOrEmptyString(context->LaunchCommand))
            {
                PhShellExecute(hwndDlg, context->LaunchCommand->Buffer, NULL);
            }

            PostQuitMessage(0);
        }
        break;
    case UM_ERROR:
        {
            VirusTotalShowErrorDialog(context);
        }
        break;
    }

    return DefSubclassProc(hwndDlg, uMsg, wParam, lParam);
}

HRESULT CALLBACK TaskDialogBootstrapCallback(
    _In_ HWND hwndDlg,
    _In_ UINT uMsg,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam,
    _In_ LONG_PTR dwRefData
    )
{
    PUPLOAD_CONTEXT context = (PUPLOAD_CONTEXT)dwRefData;

    switch (uMsg)
    {
    case TDN_CREATED:
        {
            context->DialogHandle = hwndDlg;

            // Center the update window on PH if it's visible else we center on the desktop.
            PhCenterWindow(hwndDlg, (IsWindowVisible(PhMainWndHandle) && !IsMinimized(PhMainWndHandle)) ? PhMainWndHandle : NULL);

            // Create the Taskdialog icons
            TaskDialogCreateIcons(context);

            if (SUCCEEDED(CoCreateInstance(&CLSID_TaskbarList, NULL, CLSCTX_INPROC_SERVER, &IID_ITaskbarList3, &context->TaskbarListClass)))
            {
                if (!SUCCEEDED(ITaskbarList3_HrInit(context->TaskbarListClass)))
                {
                    ITaskbarList3_Release(context->TaskbarListClass);
                    context->TaskbarListClass = NULL;
                }
            }

            // Subclass the Taskdialog
            SetWindowSubclass(hwndDlg, TaskDialogSubclassProc, 0, (ULONG_PTR)context);

            ShowVirusTotalUploadDialog(context);
        }
        break;
    }

    return S_OK;
}

NTSTATUS ShowUpdateDialogThread(
    _In_ PVOID Parameter
    )
{
    PH_AUTO_POOL autoPool;
    TASKDIALOGCONFIG config = { sizeof(TASKDIALOGCONFIG) };
    PUPLOAD_CONTEXT context = (PUPLOAD_CONTEXT)Parameter;
    BOOL checked = FALSE;

    PhInitializeAutoPool(&autoPool);

    config.hInstance = PluginInstance->DllBase;
    config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_CAN_BE_MINIMIZED;
    config.pszContent = L"Initializing...";
    config.lpCallbackData = (LONG_PTR)context;
    config.pfCallback = TaskDialogBootstrapCallback;

    // Start TaskDialog bootstrap
    TaskDialogIndirect(&config, NULL, NULL, &checked);

    PhDeleteAutoPool(&autoPool);

    return STATUS_SUCCESS;
}

VOID UploadToOnlineService(
    _In_ PPH_STRING FileName,
    _In_ ULONG Service
    )
{
    PUPLOAD_CONTEXT context;

    if (PhBeginInitOnce(&UploadContextTypeInitOnce))
    {
        UploadContextType = PhCreateObjectType(L"OnlineChecksObjectType", 0, UploadContextDeleteProcedure);
        PhEndInitOnce(&UploadContextTypeInitOnce);
    }

    context = (PUPLOAD_CONTEXT)PhCreateObject(sizeof(UPLOAD_CONTEXT), UploadContextType);
    memset(context, 0, sizeof(UPLOAD_CONTEXT));

    PhReferenceObject(FileName);
    context->Service = Service;
    context->FileName = FileName;
    context->BaseFileName = PhGetBaseName(context->FileName);

    PhCreateThread2(ShowUpdateDialogThread, (PVOID)context);
}

VOID UploadServiceToOnlineService(
    _In_ PPH_SERVICE_ITEM ServiceItem,
    _In_ ULONG Service
    )
{
    NTSTATUS status;
    PPH_STRING serviceFileName;
    PPH_STRING serviceBinaryPath = NULL;

    if (PhBeginInitOnce(&UploadContextTypeInitOnce))
    {
        UploadContextType = PhCreateObjectType(L"OnlineChecksObjectType", 0, UploadContextDeleteProcedure);
        PhEndInitOnce(&UploadContextTypeInitOnce);
    }

    if (NT_SUCCESS(status = QueryServiceFileName(
        &ServiceItem->Name->sr,
        &serviceFileName,
        &serviceBinaryPath
        )))
    {
        PUPLOAD_CONTEXT context;

        context = (PUPLOAD_CONTEXT)PhCreateObject(sizeof(UPLOAD_CONTEXT), UploadContextType);
        memset(context, 0, sizeof(UPLOAD_CONTEXT));

        context->Service = Service;
        context->FileName = serviceFileName;
        context->BaseFileName = PhGetBaseName(context->FileName);

        PhCreateThread2(ShowUpdateDialogThread, (PVOID)context);
    }
    else
    {
        PhShowStatus(PhMainWndHandle, L"Unable to query the service", status, 0);
    }

    if (serviceBinaryPath)
        PhDereferenceObject(serviceBinaryPath);
}