
#define WINBOOL BOOL
#include "winusb.h"

#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

#undef _WIN32
#undef _WIN64
#undef _MSC_VER
#undef interface
#include <libusb.h>

#define MY_VENDOR   0x138a
#define MY_PRODUCT  0x0097

struct driver_info {
    libusb_context *ctx;
    struct libusb_device_descriptor descr;
    libusb_device_handle * dev;
    DWORD timeouts[0x100];
    volatile char abort[0x100];
};

WINUSB_PIPE_INFORMATION info[] = {
    { UsbdPipeTypeBulk, 0x01, 0x40, 0 },
    { UsbdPipeTypeBulk, 0x81, 0x40, 0 },
    { UsbdPipeTypeBulk, 0x82, 0x40, 0 },
    { UsbdPipeTypeInterrupt, 0x83, 0x08, 4 },
    { UsbdPipeTypeInterrupt, 0x84, 0x10, 10 },
};

#define xstr(a) str(a)
#define str(a) #a
#define err(x) res_err(x, xstr(x))

void res_err(int result, const char* where) {
    if (result != 0) {
        printf("Failed '%s': %d - %s\n", where, result, libusb_error_name(result));
        exit(0);
    }
}


WINBOOL WINAPI 
WinUsb_GetDescriptor(
            WINUSB_INTERFACE_HANDLE InterfaceHandle, 
            UCHAR DescriptorType, 
            UCHAR Index,
            USHORT LanguageID,
            PUCHAR Buffer,
            ULONG BufferLength,
            PULONG LengthTransferred)
{
    struct driver_info *info = InterfaceHandle;
    int rc, i;

    printf("USB >>>>>>>>>> WinUsb_GetDescriptor\r\n");
    rc = libusb_control_transfer(info->dev, LIBUSB_ENDPOINT_IN,
                LIBUSB_REQUEST_GET_DESCRIPTOR, (uint16_t)((DescriptorType << 8) | Index),
                LanguageID, Buffer, (uint16_t) BufferLength, 1000);

    printf("rc=%d\r\n", rc);
    for(i=0;i<rc;i++) {
        printf("%02x", Buffer[i]);
    }
    printf("\r\n");
    *LengthTransferred = rc;

    if(rc < 0)
        return FALSE;

    return TRUE;
}

WINBOOL WINAPI 
WinUsb_QueryInterfaceSettings(
            WINUSB_INTERFACE_HANDLE InterfaceHandle, 
            UCHAR AlternateInterfaceNumber, 
            PUSB_INTERFACE_DESCRIPTOR UsbAltInterfaceDescriptor)
{
    printf("USB >>>>>>>>>> WinUsb_QueryInterfaceSettings\r\n");
    return FALSE;
}

WINBOOL WINAPI 
WinUsb_ControlTransfer(
            WINUSB_INTERFACE_HANDLE InterfaceHandle, 
            WINUSB_SETUP_PACKET SetupPacket,
            PUCHAR Buffer,
            ULONG BufferLength, 
            PULONG LengthTransferred, 
            LPOVERLAPPED Overlapped)
{
    struct driver_info *info = InterfaceHandle;
    int rc, i;

    /*
    if(pthread_self() != 0xcd3b40) {
    printf("USB >>>>>>>>>> WinUsb_ControlTransfer -- sleeping");
        sleep(5);
    }
    */

    printf("USB %lx >>>>>>>>>> WinUsb_ControlTransfer %x %x %x %x %x\r\n", 
                pthread_self(),
                SetupPacket.RequestType, 
                SetupPacket.Request, 
                SetupPacket.Value, 
                SetupPacket.Index,
                SetupPacket.Length);

    printf("Before: ");
    for(i=0;i<BufferLength;i++) {
        if(i > 8000000) {
            printf("...");
            break;
        } else {
            printf("%02x", Buffer[i]);
        }
    }
    printf("\r\n");
    rc = libusb_control_transfer(info->dev,
        SetupPacket.RequestType, SetupPacket.Request, SetupPacket.Value, SetupPacket.Index,
        Buffer, SetupPacket.Length, 10000);
    printf("rc=%d\r\n", rc);
    
    if(rc < 0) 
        return FALSE;
    
    printf("After: ");
    for(i=0;i<BufferLength;i++)
        printf("%02x ", Buffer[i]);
    printf("\r\n");

    *LengthTransferred = rc;

    return TRUE;
}

WINBOOL WINAPI 
WinUsb_ReadPipe(
            WINUSB_INTERFACE_HANDLE InterfaceHandle, 
            UCHAR PipeID, 
            PUCHAR Buffer, 
            ULONG BufferLength, 
            PULONG LengthTransferred, 
            LPOVERLAPPED Overlapped)
{
    struct driver_info *dev = InterfaceHandle;
    WINUSB_PIPE_INFORMATION *wpi = NULL;
    int i, rc;


    printf("USB %lx >>>>>>>>>> WinUsb_ReadPipe PipeID=%d\r\n", pthread_self(), PipeID);

    for(i=0;i<sizeof(info)/sizeof(*info);i++) {
        if(info[i].PipeId == PipeID)
            wpi = info + i;
    }
    if(!wpi) {
        printf("USB >>>>>>>>>>> unknown pipe!\n");
        return FALSE;
    }

    switch(wpi->PipeType) {
        case UsbdPipeTypeBulk:
            printf("USB >>>>>>>>>>> bulk xfer\n");
            rc = libusb_bulk_transfer(
                    dev->dev, 
                    PipeID, 
                    Buffer, 
                    BufferLength, 
                    (int*)LengthTransferred, 
                    dev->timeouts[PipeID]+1000);
            //sleep(3);
            break;

        case UsbdPipeTypeInterrupt:
            printf("USB >>>>>>>>>>> interrupt xfer?\n");
            dev->abort[PipeID] = 1;
            for(i=0;;) {
                rc = libusb_interrupt_transfer(
                        dev->dev, 
                        PipeID, 
                        Buffer, 
                        BufferLength, 
                        (int*)LengthTransferred, 
                        200);
                
                printf("USB >>>>>>>>>>> libusb_interrupt_transfer=%d!\n", rc);

                if(rc == LIBUSB_ERROR_TIMEOUT) {
                    if(dev->abort[PipeID] == 2) {
                        printf("USB >>>>>>>>>>> Pipe was aborted!\n");
                        rc = 0;
                        break;
                    }

                    if(dev->timeouts[PipeID]) {
                        if(i > dev->timeouts[PipeID])
                            break;
                        else
                            i += 200;
                    }
                    continue;
                }

                break;
            }
            dev->abort[PipeID] = 0;
            break;

        default:
            printf("USB >>>>>>>>>>> unknown pipe type!\n");
            return FALSE;
    }

    if(rc < 0)  {
        printf(">>>>>>>>>>> xfer Failed - %s\n", libusb_error_name(rc));
        exit(0);
        return FALSE;
    }
    

    printf("usb %d <<< ", PipeID);
    for(i=0;i<*LengthTransferred;i++) {
        if(i > 8000000) {
            printf("....");
            break;
        } else {
            printf("%02x", Buffer[i]);
        }
    }
    printf("\r\n");


    return TRUE;
}

void
claim_device(struct driver_info *info)
{
    libusb_device ** dev_list;
    int i;
    int dev_cnt = libusb_get_device_list(info->ctx, &dev_list);

    for (i = 0; i < dev_cnt; i++) {
        struct libusb_device_descriptor descriptor;

        libusb_get_device_descriptor(dev_list[i], &descriptor);

        if (MY_VENDOR == descriptor.idVendor && MY_PRODUCT == descriptor.idProduct) {
            printf("!!!!!!!!!!!! Found device %04x:%04x\n", descriptor.idVendor, descriptor.idProduct);

            err(libusb_get_device_descriptor(dev_list[i], &info->descr));
            err(libusb_open(dev_list[i], &info->dev));
            break;
        }
    }

    err(libusb_reset_device(info->dev));
    err(libusb_set_configuration(info->dev, 1));
    err(libusb_claim_interface(info->dev, 0));

}

WINBOOL WINAPI 
WinUsb_WritePipe(
            WINUSB_INTERFACE_HANDLE InterfaceHandle, 
            UCHAR PipeID,
            PUCHAR Buffer,
            ULONG BufferLength, 
            PULONG LengthTransferred, 
            LPOVERLAPPED Overlapped)
{
    int i, rc;
    struct driver_info *info = InterfaceHandle;
    int send;


    printf("USB %lx >>>>>>>>>> WinUsb_WritePipe PipeID=%x\r\n", pthread_self(), PipeID);
    
    printf("usb %d >>> ", PipeID);
    for(i=0;i<BufferLength;i++) {
        if(i > 8000000) {
            printf("...");
            break;
        } else {
            printf("%02x", Buffer[i]);
        }
    }
    printf("\r\n");

    if(Buffer[0] == 0x10 && Buffer[1] == 0 && Buffer[2] == 0) {
        abort();
        return FALSE;
    }

    rc = libusb_bulk_transfer(info->dev,
            PipeID,
            Buffer,
            BufferLength, 
            &send, 
            info->timeouts[PipeID]+1000);
    printf("rc=%d send=%d\r\n", rc, send);

#if 0
    if(rc < 0) {
        sleep(10);
        claim_device(info);
        sleep(10);
        rc = libusb_bulk_transfer(info->dev,
            PipeID,
            Buffer,
            BufferLength, 
            &send, 
            info->timeouts[PipeID]);
        printf("retry: rc=%d send=%d\r\n", rc, send);
    }
#endif
    
    if(rc < 0) {
        printf(">>>>>>>>>>> xfer Failed - %s\n", libusb_error_name(rc));
        exit(0);
        return FALSE;
    }
    *LengthTransferred = send;

    return TRUE;
}

WINBOOL WINAPI 
WinUsb_QueryPipe(
            WINUSB_INTERFACE_HANDLE InterfaceHandle,
            UCHAR AlternateInterfaceNumber,
            UCHAR PipeIndex,
            PWINUSB_PIPE_INFORMATION PipeInformation)
{
    printf("USB >>>>>>>>>> WinUsb_QueryPipe alt-if=%d pipe=%d\r\n", AlternateInterfaceNumber, PipeIndex);

    *PipeInformation = info[PipeIndex];

    return TRUE;
}

WINBOOL WINAPI WinUsb_ResetPipe (WINUSB_INTERFACE_HANDLE InterfaceHandle, UCHAR PipeID)
{
    printf("USB >>>>>>>>>> WinUsb_ResetPipe pipe=%d\r\n", PipeID);
    return TRUE;
}

// ============================================================

WINBOOL WINAPI 
WinUsb_SetPipePolicy(
            WINUSB_INTERFACE_HANDLE InterfaceHandle,
            UCHAR PipeID,
            ULONG PolicyType,
            ULONG ValueLength,
            PVOID Value)
{
    struct driver_info *info = InterfaceHandle;

    printf("USB >>>>>>>>>> WinUsb_SetPipePolicy pipe=%d, policy type=%d, val length=%d, val=%x\r\n", PipeID, PolicyType, ValueLength, *(DWORD*)Value);

    if(PolicyType == PIPE_TRANSFER_TIMEOUT) {
        info->timeouts[PipeID] = *(DWORD*)Value;
    }
    return TRUE;
}

WINBOOL WINAPI WinUsb_FlushPipe (WINUSB_INTERFACE_HANDLE InterfaceHandle, UCHAR PipeID)
{
    printf("USB >>>>>>>>>> WinUsb_FlushPipe\r\n");
    return TRUE;
}

WINBOOL WINAPI WinUsb_AbortPipe (WINUSB_INTERFACE_HANDLE InterfaceHandle, UCHAR PipeID)
{
    struct driver_info *dev = InterfaceHandle;
    printf("USB >>>>>>>>>> WinUsb_AbortPipe %x\r\n", PipeID);
    if(dev->abort[PipeID] == 1) {
        dev->abort[PipeID] = 2;

        while(dev->abort[PipeID] != 0)
            usleep(200);
    }
    printf("USB >>>>>>>>>> abort complete\r\n");
    return TRUE;
}

WINBOOL WINAPI WinUsb_Free (WINUSB_INTERFACE_HANDLE InterfaceHandle)
{
    printf("USB >>>>>>>>>> WinUsb_Free\r\n");
    return TRUE;
}

WINBOOL WINAPI WinUsb_GetAssociatedInterface (WINUSB_INTERFACE_HANDLE InterfaceHandle, UCHAR AssociatedInterfaceIndex, PWINUSB_INTERFACE_HANDLE AssociatedInterfaceHandle)
{
    printf("USB >>>>>>>>>> WinUsb_GetAssociatedInterface\r\n");
    return FALSE;
}

WINBOOL WINAPI WinUsb_GetCurrentAlternateSetting (WINUSB_INTERFACE_HANDLE InterfaceHandle, PUCHAR SettingNumber)
{
    printf("USB >>>>>>>>>> WinUsb_GetCurrentAlternateSetting\r\n");
    return FALSE;
}

WINBOOL WINAPI 
WinUsb_SetPowerPolicy(
            WINUSB_INTERFACE_HANDLE InterfaceHandle,
            ULONG PolicyType,
            ULONG ValueLength,
            PVOID Value)
{
    printf("USB >>>>>>>>>> WinUsb_SetPowerPolicy\r\n");
    return TRUE;
}

WINBOOL WINAPI 
WinUsb_SetCurrentAlternateSetting(
            WINUSB_INTERFACE_HANDLE InterfaceHandle,
            UCHAR SettingNumber)
{
    printf("USB >>>>>>>>>> WinUsb_SetCurrentAlternateSetting\r\n");
    return FALSE;
}

PUSB_INTERFACE_DESCRIPTOR WINAPI 
WinUsb_ParseConfigurationDescriptor (
            PUSB_CONFIGURATION_DESCRIPTOR ConfigurationDescriptor,
            PVOID StartPosition,
            LONG InterfaceNumber,
            LONG AlternateSetting,
            LONG InterfaceClass,
            LONG InterfaceSubClass,
            LONG InterfaceProtocol)
{
    printf("USB >>>>>>>>>> WinUsb_ParseConfigurationDescriptor\r\n");
    return NULL;
}

PUSB_COMMON_DESCRIPTOR WINAPI 
WinUsb_ParseDescriptors (PVOID DescriptorBuffer, 
            ULONG TotalLength,
            PVOID StartPosition,
            LONG DescriptorType)
{
    printf("USB >>>>>>>>>> WinUsb_ParseDescriptors\r\n");
    return NULL;
}

WINBOOL WINAPI WinUsb_GetPowerPolicy (WINUSB_INTERFACE_HANDLE InterfaceHandle, ULONG PolicyType, PULONG ValueLength, PVOID Value)
{
    printf("USB >>>>>>>>>> WinUsb_GetPowerPolicy\r\n");
    return FALSE;
}


BOOL WINAPI DllMain( HINSTANCE hinstDLL, DWORD     fdwReason, LPVOID    lpvReserved)
{
    printf("USB >>>>>>>>>> In DllMain reason=%x\r\n", fdwReason);

    return TRUE;
}

WINBOOL WINAPI WinUsb_Initialize (HANDLE DeviceHandle, PWINUSB_INTERFACE_HANDLE InterfaceHandle)
{
    struct driver_info *info = malloc(sizeof(struct driver_info));

    memset(info, 0, sizeof(*info));

    info->dev = NULL;
    *InterfaceHandle = info;

    printf("USB >>>>>>>>>> WinUsb_Initialize\r\n");
    libusb_init(&info->ctx);
    libusb_set_debug(info->ctx, 3);

    claim_device(info);

    return TRUE;
}

WINBOOL WINAPI WinUsb_GetOverlappedResult (WINUSB_INTERFACE_HANDLE InterfaceHandle, LPOVERLAPPED lpOverlapped, LPDWORD lpNumberOfBytesTransferred, WINBOOL bWait)
{
    printf("USB >>>>>>>>>> WinUsb_GetOverlappedResult\r\n");
    return FALSE;
}

WINBOOL WINAPI 
WinUsb_GetPipePolicy(
            WINUSB_INTERFACE_HANDLE InterfaceHandle,
            UCHAR PipeID,
            ULONG PolicyType,
            PULONG ValueLength,
            PVOID Value)
{
    struct driver_info *info = InterfaceHandle;

    printf("USB >>>>>>>>>> WinUsb_GetPipePolicy PipeID=%d PolicyType=%d ValueLength=%d\r\n", 
                PipeID, 
                PolicyType,
                *ValueLength);
    if(PolicyType == PIPE_TRANSFER_TIMEOUT) {
        *(DWORD*)Value = info->timeouts[PipeID];
    }
    printf("USB >>>>>>>>>> WinUsb_GetPipePolicy PipeID=%d PolicyType=%d ValueLength=%d Value=%x\r\n", 
                PipeID, 
                PolicyType,
                *ValueLength,
                *(DWORD*)Value);
    return TRUE;
}

WINBOOL WINAPI 
WinUsb_QueryDeviceInformation(
        WINUSB_INTERFACE_HANDLE InterfaceHandle,
        ULONG InformationType, 
        PULONG BufferLength, 
        PVOID Buffer)
{
    printf("USB >>>>>>>>>> WinUsb_QueryDeviceInformation\r\n");
    return FALSE;
}

