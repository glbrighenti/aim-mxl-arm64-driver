#include <ntddk.h>
#include <usb.h>
#include <usbdlib.h>
#include <wdf.h>
#include <wdfusb.h>

static const GUID aim_interface_guid =
    {0xaf171a6e, 0x5c83, 0x47fd, {0xba, 0xec, 0x3b, 0x04, 0x98, 0xf2, 0x99, 0x8f}};

typedef struct _AIM_DEVICE_CONTEXT {
    WDFUSBDEVICE usb_device;
    WDFUSBINTERFACE usb_interface;
    WDFUSBPIPE read_pipe;
    WDFUSBPIPE write_pipe;
} AIM_DEVICE_CONTEXT;

typedef struct _AIM_FILE_CONTEXT {
    WDFUSBPIPE pipe;
} AIM_FILE_CONTEXT;

typedef struct _AIM_TRANSFER32 {
    ULONG reserved0;
    ULONG endpoint;
    ULONG direction;
    ULONG reserved_c;
    ULONG buffer_address;
    ULONG length;
    ULONG reserved18;
    ULONG transferred;
    UCHAR request_type;
    UCHAR vendor_request;
    USHORT vendor_value;
    USHORT vendor_index;
    USHORT reserved26;
    ULONG completed;
    ULONG reserved2c;
} AIM_TRANSFER32;

typedef struct _AIM_TRANSFER64 {
    ULONG reserved0;
    ULONG endpoint;
    ULONG direction;
    ULONG reserved_c;
    ULONGLONG buffer_address;
    ULONG length;
    ULONG reserved1c;
    ULONG transferred;
    UCHAR request_type;
    UCHAR vendor_request;
    USHORT vendor_value;
    USHORT vendor_index;
    USHORT reserved2a;
    ULONG completed;
    ULONG reserved30;
    ULONG reserved34;
} AIM_TRANSFER64;

typedef struct _AIM_REQUEST_CONTEXT {
    WDFMEMORY user_memory;
    ULONG endpoint;
    ULONG length;
    BOOLEAN direction_in;
    BOOLEAN wide_layout;
    UCHAR vendor_request;
    USHORT vendor_value;
    USHORT vendor_index;
} AIM_REQUEST_CONTEXT;

C_ASSERT(sizeof(AIM_TRANSFER32) == 0x30);
C_ASSERT(FIELD_OFFSET(AIM_TRANSFER32, endpoint) == 0x04);
C_ASSERT(FIELD_OFFSET(AIM_TRANSFER32, direction) == 0x08);
C_ASSERT(FIELD_OFFSET(AIM_TRANSFER32, buffer_address) == 0x10);
C_ASSERT(FIELD_OFFSET(AIM_TRANSFER32, length) == 0x14);
C_ASSERT(FIELD_OFFSET(AIM_TRANSFER32, transferred) == 0x1c);
C_ASSERT(FIELD_OFFSET(AIM_TRANSFER32, vendor_request) == 0x21);
C_ASSERT(FIELD_OFFSET(AIM_TRANSFER32, vendor_value) == 0x22);
C_ASSERT(FIELD_OFFSET(AIM_TRANSFER32, vendor_index) == 0x24);
C_ASSERT(FIELD_OFFSET(AIM_TRANSFER32, completed) == 0x28);
C_ASSERT(sizeof(AIM_TRANSFER64) == 0x38);
C_ASSERT(FIELD_OFFSET(AIM_TRANSFER64, buffer_address) == 0x10);
C_ASSERT(FIELD_OFFSET(AIM_TRANSFER64, length) == 0x18);
C_ASSERT(FIELD_OFFSET(AIM_TRANSFER64, transferred) == 0x20);
C_ASSERT(FIELD_OFFSET(AIM_TRANSFER64, vendor_request) == 0x25);
C_ASSERT(FIELD_OFFSET(AIM_TRANSFER64, vendor_value) == 0x26);
C_ASSERT(FIELD_OFFSET(AIM_TRANSFER64, vendor_index) == 0x28);
C_ASSERT(FIELD_OFFSET(AIM_TRANSFER64, completed) == 0x2c);

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(AIM_DEVICE_CONTEXT, AimGetDeviceContext);
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(AIM_FILE_CONTEXT, AimGetFileContext);
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(AIM_REQUEST_CONTEXT, AimGetRequestContext);

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD AimDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE AimPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE AimReleaseHardware;
EVT_WDF_IO_IN_CALLER_CONTEXT AimInCallerContext;
EVT_WDF_DEVICE_FILE_CREATE AimFileCreate;
EVT_WDF_IO_QUEUE_IO_READ AimRead;
EVT_WDF_IO_QUEUE_IO_WRITE AimWrite;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL AimIoControl;

VOID
AimInCallerContext(
    WDFDEVICE device,
    WDFREQUEST request
    )
{
    WDF_REQUEST_PARAMETERS parameters;
    AIM_TRANSFER32 *transfer;
    AIM_TRANSFER64 *wide_transfer;
    AIM_REQUEST_CONTEXT *context;
    ULONGLONG buffer_address;
    ULONG endpoint;
    ULONG direction;
    ULONG length;
    BOOLEAN wide_layout;
    NTSTATUS status;

    WDF_REQUEST_PARAMETERS_INIT(&parameters);
    WdfRequestGetParameters(request, &parameters);
    if (parameters.Type == WdfRequestTypeDeviceControl &&
        (parameters.Parameters.DeviceIoControl.IoControlCode == 0x220010 ||
         parameters.Parameters.DeviceIoControl.IoControlCode == 0x22000c)) {
        if (WdfRequestGetRequestorMode(request) != UserMode) {
            WdfRequestComplete(request, STATUS_INVALID_DEVICE_REQUEST);
            return;
        }
        status = WdfRequestRetrieveInputBuffer(request, sizeof(*transfer),
                                               (PVOID *)&transfer, NULL);
        if (!NT_SUCCESS(status)) {
            WdfRequestComplete(request, status);
            return;
        }
        wide_layout = parameters.Parameters.DeviceIoControl.InputBufferLength ==
                      sizeof(AIM_TRANSFER64);
        if ((!wide_layout && parameters.Parameters.DeviceIoControl.InputBufferLength !=
             sizeof(*transfer)) ||
            parameters.Parameters.DeviceIoControl.OutputBufferLength <
             parameters.Parameters.DeviceIoControl.InputBufferLength) {
            WdfRequestComplete(request, STATUS_INVALID_PARAMETER);
            return;
        }
        wide_transfer = (AIM_TRANSFER64 *)transfer;
        endpoint = wide_layout ? wide_transfer->endpoint : transfer->endpoint;
        direction = wide_layout ? wide_transfer->direction : transfer->direction;
        length = wide_layout ? wide_transfer->length : transfer->length;
        buffer_address = wide_layout ? wide_transfer->buffer_address : transfer->buffer_address;
        if (length > 0x10000 ||
            (parameters.Parameters.DeviceIoControl.IoControlCode == 0x22000c &&
             length > 0xffff) ||
            (parameters.Parameters.DeviceIoControl.IoControlCode == 0x220010 &&
             (direction > 1 || endpoint > 0xff)) ||
            (length != 0 && buffer_address == 0)) {
            WdfRequestComplete(request, STATUS_INVALID_PARAMETER);
            return;
        }

        context = AimGetRequestContext(request);
        context->endpoint = endpoint;
        context->length = length;
        context->direction_in = parameters.Parameters.DeviceIoControl.IoControlCode ==
                    0x220010 && direction != 0;
        context->wide_layout = wide_layout;
        context->vendor_request = wide_layout ? wide_transfer->vendor_request :
                                  transfer->vendor_request;
        context->vendor_value = wide_layout ? wide_transfer->vendor_value :
                                transfer->vendor_value;
        context->vendor_index = wide_layout ? wide_transfer->vendor_index :
                                transfer->vendor_index;
        context->user_memory = NULL;
        if (length != 0) {
            PVOID buffer = (PVOID)(ULONG_PTR)buffer_address;

            if (context->direction_in) {
                status = WdfRequestProbeAndLockUserBufferForWrite(request, buffer,
                                                                  length,
                                                                  &context->user_memory);
            } else {
                status = WdfRequestProbeAndLockUserBufferForRead(request, buffer,
                                                                 length,
                                                                 &context->user_memory);
            }
            if (!NT_SUCCESS(status)) {
                WdfRequestComplete(request, status);
                return;
            }
        }
    }

    status = WdfDeviceEnqueueRequest(device, request);
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(request, status);
    }
}

VOID
AimVendorControl(
    AIM_DEVICE_CONTEXT *device_context,
    WDFREQUEST request
    )
{
    AIM_REQUEST_CONTEXT *context = AimGetRequestContext(request);
    AIM_TRANSFER32 *transfer;
    ULONG *transfer_count;
    ULONG *completed;
    size_t result_length = context->wide_layout ? sizeof(AIM_TRANSFER64) : sizeof(*transfer);
    WDF_USB_CONTROL_SETUP_PACKET setup;
    WDF_MEMORY_DESCRIPTOR descriptor;
    WDF_REQUEST_SEND_OPTIONS options;
    ULONG transferred = 0;
    NTSTATUS status;

    status = WdfRequestRetrieveOutputBuffer(request, result_length,
                                            (PVOID *)&transfer, NULL);
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(request, status);
        return;
    }
    transfer_count = context->wide_layout ? &((AIM_TRANSFER64 *)transfer)->transferred :
                     &transfer->transferred;
    completed = context->wide_layout ? &((AIM_TRANSFER64 *)transfer)->completed :
                &transfer->completed;
    *transfer_count = 0;
    *completed = 0;
    WDF_USB_CONTROL_SETUP_PACKET_INIT_VENDOR(&setup, BmRequestHostToDevice,
                                              BmRequestToEndpoint, context->vendor_request,
                                              context->vendor_value, context->vendor_index);
    WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&options, WDF_REL_TIMEOUT_IN_SEC(10));
    if (context->length != 0) {
        WDF_MEMORY_DESCRIPTOR_INIT_HANDLE(&descriptor, context->user_memory, NULL);
    }
    status = WdfUsbTargetDeviceSendControlTransferSynchronously(
        device_context->usb_device, WDF_NO_HANDLE, &options, &setup,
        context->length != 0 ? &descriptor : NULL, &transferred);

    if (NT_SUCCESS(status)) {
        *transfer_count = transferred;
        *completed = 1;
    }
    WdfRequestCompleteWithInformation(request, status,
                                      NT_SUCCESS(status) ? result_length : 0);
}

VOID
AimTransfer(
    AIM_DEVICE_CONTEXT *device_context,
    WDFREQUEST request
    )
{
    AIM_REQUEST_CONTEXT *context = AimGetRequestContext(request);
    AIM_TRANSFER32 *transfer;
    ULONG *transfer_count;
    ULONG *completed;
    size_t result_length = context->wide_layout ? sizeof(AIM_TRANSFER64) : sizeof(*transfer);
    WDF_USB_PIPE_INFORMATION pipe_information = {0};
    WDF_MEMORY_DESCRIPTOR descriptor;
    WDF_REQUEST_SEND_OPTIONS options;
    WDFUSBPIPE pipe = NULL;
    PVOID read_buffer = NULL;
    UCHAR pipe_index;
    ULONG transferred = 0;
    NTSTATUS status;

    status = WdfRequestRetrieveOutputBuffer(request, result_length,
                                            (PVOID *)&transfer, NULL);
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(request, status);
        return;
    }
    transfer_count = context->wide_layout ? &((AIM_TRANSFER64 *)transfer)->transferred :
                     &transfer->transferred;
    completed = context->wide_layout ? &((AIM_TRANSFER64 *)transfer)->completed :
                &transfer->completed;
    *transfer_count = 0;
    *completed = 0;

    for (pipe_index = 0;
         pipe_index < WdfUsbInterfaceGetNumConfiguredPipes(device_context->usb_interface);
         ++pipe_index) {
        WDF_USB_PIPE_INFORMATION_INIT(&pipe_information);
        pipe = WdfUsbInterfaceGetConfiguredPipe(device_context->usb_interface,
                                                pipe_index, &pipe_information);
        if (pipe_information.EndpointAddress == context->endpoint &&
            pipe_information.PipeType == WdfUsbPipeTypeBulk &&
            WdfUsbTargetPipeIsInEndpoint(pipe) == context->direction_in) {
            break;
        }
    }
    if (pipe_index == WdfUsbInterfaceGetNumConfiguredPipes(device_context->usb_interface)) {
        WdfRequestComplete(request, STATUS_INVALID_PARAMETER);
        return;
    }

    WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&options, WDF_REL_TIMEOUT_IN_SEC(10));
    if (context->length != 0) {
        if (context->direction_in && context->length < pipe_information.MaximumPacketSize) {
            read_buffer = ExAllocatePool2(POOL_FLAG_NON_PAGED,
                                          pipe_information.MaximumPacketSize, 'lxmA');
            if (read_buffer == NULL) {
                WdfRequestComplete(request, STATUS_INSUFFICIENT_RESOURCES);
                return;
            }
            WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&descriptor, read_buffer,
                                              pipe_information.MaximumPacketSize);
        } else {
            WDF_MEMORY_DESCRIPTOR_INIT_HANDLE(&descriptor, context->user_memory, NULL);
        }
    }
    if (context->direction_in) {
        status = WdfUsbTargetPipeReadSynchronously(pipe, WDF_NO_HANDLE, &options,
                                                   context->length != 0 ? &descriptor : NULL,
                                                   &transferred);
        if (NT_SUCCESS(status) && read_buffer != NULL) {
            if (transferred > context->length) {
                status = STATUS_BUFFER_TOO_SMALL;
            } else {
                RtlCopyMemory(WdfMemoryGetBuffer(context->user_memory, NULL), read_buffer,
                              transferred);
            }
        }
    } else {
        status = WdfUsbTargetPipeWriteSynchronously(pipe, WDF_NO_HANDLE, &options,
                                                    context->length != 0 ? &descriptor : NULL,
                                                    &transferred);
    }
    if (read_buffer != NULL) {
        ExFreePoolWithTag(read_buffer, 'lxmA');
    }

    if (NT_SUCCESS(status)) {
        *transfer_count = transferred;
        *completed = 1;
    }
    WdfRequestCompleteWithInformation(request, status,
                                      NT_SUCCESS(status) ? result_length : 0);
}

VOID
AimIoControl(
    WDFQUEUE queue,
    WDFREQUEST request,
    size_t output_length,
    size_t input_length,
    ULONG control_code
    )
{
    AIM_DEVICE_CONTEXT *device_context = AimGetDeviceContext(WdfIoQueueGetDevice(queue));
    AIM_FILE_CONTEXT *file_context = AimGetFileContext(WdfRequestGetFileObject(request));
    WDF_REQUEST_SEND_OPTIONS options;
    USHORT descriptor_length = 0;
    PVOID output;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(input_length);

    if (control_code == 0x220010) {
        AimTransfer(device_context, request);
        return;
    }
    if (control_code == 0x22000c) {
        AimVendorControl(device_context, request);
        return;
    }

    if (control_code == 0x220008) {
        if (file_context->pipe == NULL) {
            WdfRequestComplete(request, STATUS_INVALID_DEVICE_REQUEST);
            return;
        }
        WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
        WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&options, WDF_REL_TIMEOUT_IN_SEC(10));
        status = WdfUsbTargetPipeResetSynchronously(file_context->pipe, WDF_NO_HANDLE,
                                                     &options);
        WdfRequestComplete(request, status);
        return;
    }

    if (control_code != 0x220000) {
        WdfRequestComplete(request, STATUS_INVALID_DEVICE_REQUEST);
        return;
    }

    status = WdfUsbTargetDeviceRetrieveConfigDescriptor(device_context->usb_device,
                                                          NULL, &descriptor_length);
    if (status != STATUS_BUFFER_TOO_SMALL && !NT_SUCCESS(status)) {
        WdfRequestComplete(request, status);
        return;
    }
    if (output_length < descriptor_length) {
        WdfRequestComplete(request, STATUS_BUFFER_TOO_SMALL);
        return;
    }

    status = WdfRequestRetrieveOutputBuffer(request, descriptor_length, &output, NULL);
    if (NT_SUCCESS(status)) {
        status = WdfUsbTargetDeviceRetrieveConfigDescriptor(device_context->usb_device,
                                                              output, &descriptor_length);
    }
    WdfRequestCompleteWithInformation(request, status,
                                      NT_SUCCESS(status) ? descriptor_length : 0);
}

VOID
AimFileCreate(
    WDFDEVICE device,
    WDFREQUEST request,
    WDFFILEOBJECT file_object
    )
{
    UNICODE_STRING read_name = RTL_CONSTANT_STRING(L"\\Bulk_Read_Pipe");
    UNICODE_STRING write_name = RTL_CONSTANT_STRING(L"\\Bulk_Write_Pipe");
    PCUNICODE_STRING file_name = WdfFileObjectGetFileName(file_object);
    AIM_DEVICE_CONTEXT *device_context = AimGetDeviceContext(device);
    AIM_FILE_CONTEXT *file_context = AimGetFileContext(file_object);
    NTSTATUS status = STATUS_SUCCESS;

    if (file_name->Length == 0) {
        file_context->pipe = NULL;
    } else if (RtlEqualUnicodeString(file_name, &read_name, TRUE)) {
        file_context->pipe = device_context->read_pipe;
    } else if (RtlEqualUnicodeString(file_name, &write_name, TRUE)) {
        file_context->pipe = device_context->write_pipe;
    } else {
        status = STATUS_OBJECT_NAME_NOT_FOUND;
    }

    WdfRequestComplete(request, status);
}

VOID
AimRead(
    WDFQUEUE queue,
    WDFREQUEST request,
    size_t length
    )
{
    AIM_FILE_CONTEXT *file_context = AimGetFileContext(WdfRequestGetFileObject(request));
    WDFMEMORY memory;
    WDF_MEMORY_DESCRIPTOR descriptor;
    WDF_REQUEST_SEND_OPTIONS options;
    ULONG transferred = 0;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(queue);
    UNREFERENCED_PARAMETER(length);

    if (file_context->pipe == NULL || !WdfUsbTargetPipeIsInEndpoint(file_context->pipe)) {
        WdfRequestComplete(request, STATUS_INVALID_DEVICE_REQUEST);
        return;
    }

    status = WdfRequestRetrieveOutputMemory(request, &memory);
    if (NT_SUCCESS(status)) {
        WDF_MEMORY_DESCRIPTOR_INIT_HANDLE(&descriptor, memory, NULL);
        WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
        WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&options, WDF_REL_TIMEOUT_IN_SEC(10));
        status = WdfUsbTargetPipeReadSynchronously(file_context->pipe, WDF_NO_HANDLE,
                                                    &options, &descriptor, &transferred);
    }
    WdfRequestCompleteWithInformation(request, status, NT_SUCCESS(status) ? transferred : 0);
}

VOID
AimWrite(
    WDFQUEUE queue,
    WDFREQUEST request,
    size_t length
    )
{
    AIM_FILE_CONTEXT *file_context = AimGetFileContext(WdfRequestGetFileObject(request));
    WDFMEMORY memory;
    WDF_MEMORY_DESCRIPTOR descriptor;
    WDF_REQUEST_SEND_OPTIONS options;
    ULONG transferred = 0;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(queue);
    UNREFERENCED_PARAMETER(length);

    if (file_context->pipe == NULL || WdfUsbTargetPipeIsInEndpoint(file_context->pipe)) {
        WdfRequestComplete(request, STATUS_INVALID_DEVICE_REQUEST);
        return;
    }

    status = WdfRequestRetrieveInputMemory(request, &memory);
    if (NT_SUCCESS(status)) {
        WDF_MEMORY_DESCRIPTOR_INIT_HANDLE(&descriptor, memory, NULL);
        WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
        WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&options, WDF_REL_TIMEOUT_IN_SEC(10));
        status = WdfUsbTargetPipeWriteSynchronously(file_context->pipe, WDF_NO_HANDLE,
                                                     &options, &descriptor, &transferred);
    }
    WdfRequestCompleteWithInformation(request, status, NT_SUCCESS(status) ? transferred : 0);
}

NTSTATUS
DriverEntry(
    PDRIVER_OBJECT driver_object,
    PUNICODE_STRING registry_path
    )
{
    WDF_DRIVER_CONFIG config;

    WDF_DRIVER_CONFIG_INIT(&config, AimDeviceAdd);
    return WdfDriverCreate(driver_object, registry_path, WDF_NO_OBJECT_ATTRIBUTES,
                           &config, WDF_NO_HANDLE);
}

NTSTATUS
AimDeviceAdd(
    WDFDRIVER driver,
    PWDFDEVICE_INIT device_init
    )
{
    WDFDEVICE device;
    WDF_FILEOBJECT_CONFIG file_config;
    WDF_IO_QUEUE_CONFIG queue_config;
    WDF_PNPPOWER_EVENT_CALLBACKS pnp_callbacks;
    WDF_OBJECT_ATTRIBUTES device_attributes;
    WDF_OBJECT_ATTRIBUTES file_attributes;
    WDF_OBJECT_ATTRIBUTES queue_attributes;
    WDF_OBJECT_ATTRIBUTES request_attributes;
    WDFQUEUE queue;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(driver);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp_callbacks);
    pnp_callbacks.EvtDevicePrepareHardware = AimPrepareHardware;
    pnp_callbacks.EvtDeviceReleaseHardware = AimReleaseHardware;
    WdfDeviceInitSetPnpPowerEventCallbacks(device_init, &pnp_callbacks);
    WdfDeviceInitSetIoInCallerContextCallback(device_init, AimInCallerContext);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&request_attributes, AIM_REQUEST_CONTEXT);
    WdfDeviceInitSetRequestAttributes(device_init, &request_attributes);
    WDF_FILEOBJECT_CONFIG_INIT(&file_config, AimFileCreate, WDF_NO_EVENT_CALLBACK,
                               WDF_NO_EVENT_CALLBACK);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&file_attributes, AIM_FILE_CONTEXT);
    WdfDeviceInitSetFileObjectConfig(device_init, &file_config, &file_attributes);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&device_attributes, AIM_DEVICE_CONTEXT);
    status = WdfDeviceCreate(&device_init, &device_attributes, &device);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queue_config, WdfIoQueueDispatchParallel);
    queue_config.EvtIoRead = AimRead;
    queue_config.EvtIoWrite = AimWrite;
    queue_config.EvtIoDeviceControl = AimIoControl;
    WDF_OBJECT_ATTRIBUTES_INIT(&queue_attributes);
    queue_attributes.ExecutionLevel = WdfExecutionLevelPassive;
    status = WdfIoQueueCreate(device, &queue_config, &queue_attributes, &queue);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    return WdfDeviceCreateDeviceInterface(device, &aim_interface_guid, NULL);
}

NTSTATUS
AimPrepareHardware(
    WDFDEVICE device,
    WDFCMRESLIST raw_resources,
    WDFCMRESLIST translated_resources
    )
{
    AIM_DEVICE_CONTEXT *device_context = AimGetDeviceContext(device);
    WDFUSBDEVICE usb_device;
    WDFUSBINTERFACE usb_interface;
    WDF_USB_DEVICE_SELECT_CONFIG_PARAMS select_config;
    WDFUSBPIPE pipe;
    UCHAR pipe_index;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(raw_resources);
    UNREFERENCED_PARAMETER(translated_resources);

    status = WdfUsbTargetDeviceCreate(device, WDF_NO_OBJECT_ATTRIBUTES, &usb_device);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_USB_DEVICE_SELECT_CONFIG_PARAMS_INIT_SINGLE_INTERFACE(&select_config);
    status = WdfUsbTargetDeviceSelectConfig(usb_device, WDF_NO_OBJECT_ATTRIBUTES,
                                            &select_config);
    if (!NT_SUCCESS(status)) {
        WdfObjectDelete(usb_device);
        return status;
    }

    usb_interface = select_config.Types.SingleInterface.ConfiguredUsbInterface;
    for (pipe_index = 0; pipe_index < WdfUsbInterfaceGetNumConfiguredPipes(usb_interface);
         ++pipe_index) {
        pipe = WdfUsbInterfaceGetConfiguredPipe(usb_interface, pipe_index, NULL);
        if (WdfUsbTargetPipeGetType(pipe) != WdfUsbPipeTypeBulk) {
            continue;
        }
        if (WdfUsbTargetPipeIsInEndpoint(pipe)) {
            device_context->read_pipe = pipe;
        } else {
            device_context->write_pipe = pipe;
        }
    }
    if (device_context->read_pipe == NULL || device_context->write_pipe == NULL) {
        device_context->read_pipe = NULL;
        device_context->write_pipe = NULL;
        WdfObjectDelete(usb_device);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    device_context->usb_device = usb_device;
    device_context->usb_interface = usb_interface;
    return STATUS_SUCCESS;
}

NTSTATUS
AimReleaseHardware(
    WDFDEVICE device,
    WDFCMRESLIST translated_resources
    )
{
    AIM_DEVICE_CONTEXT *device_context = AimGetDeviceContext(device);

    UNREFERENCED_PARAMETER(translated_resources);

    if (device_context->usb_device != NULL) {
        WdfObjectDelete(device_context->usb_device);
        device_context->usb_device = NULL;
    }
    device_context->usb_interface = NULL;
    device_context->read_pipe = NULL;
    device_context->write_pipe = NULL;
    return STATUS_SUCCESS;
}