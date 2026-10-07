/*
 * Allocation-free USB device core.  Host-controller drivers provide EP0
 * transport operations; class drivers consume the standard request,
 * enumeration, and descriptor-iteration APIs below.
 */
#include "USBCore.h"
#include "UHCIMemory.h"

uhci_u16
USBCoreReadLE16(const uhci_u8 *bytes)
{
    if (!bytes)
        return 0;
    return (uhci_u16)(bytes[0] | ((uhci_u16)bytes[1] << 8));
}

void
USBCoreDeviceInitialize(USBCoreDevice *device,
                        const USBTransportOperations *operations,
                        void *hostContext, void *deviceContext)
{
    if (!device)
        return;
    device->operations = operations;
    device->hostContext = hostContext;
    device->deviceContext = deviceContext;
}

int
USBCoreControlTransfer(USBCoreDevice *device, const USBSetupPacket *setup,
                       void *bytes, uhci_u16 *actualLength)
{
    if (actualLength)
        *actualLength = 0;
    if (!device || !device->operations ||
        !device->operations->controlTransfer || !setup)
        return 0;
    return device->operations->controlTransfer(device->hostContext,
                                                device->deviceContext,
                                                setup, bytes, actualLength);
}

int
USBCoreGetDescriptor(USBCoreDevice *device, uhci_u8 type, uhci_u8 index,
                     void *bytes, uhci_u16 length, uhci_u16 *actualLength)
{
    USBSetupPacket setup;
    setup.requestType = USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE;
    setup.request = USB_REQ_GET_DESCRIPTOR;
    setup.value = ((uhci_u16)type << 8) | index;
    setup.index = 0;
    setup.length = length;
    return USBCoreControlTransfer(device, &setup, bytes, actualLength);
}

int
USBCoreSetConfiguration(USBCoreDevice *device, uhci_u8 value)
{
    USBSetupPacket setup;
    setup.requestType = USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_DEVICE;
    setup.request = USB_REQ_SET_CONFIG;
    setup.value = value;
    setup.index = 0;
    setup.length = 0;
    return USBCoreControlTransfer(device, &setup, 0, 0);
}

int
USBCoreSetInterface(USBCoreDevice *device, uhci_u8 interfaceNumber,
                    uhci_u8 alternateSetting)
{
    USBSetupPacket setup;
    setup.requestType = USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_INTERFACE;
    setup.request = USB_REQ_SET_INTERFACE;
    setup.value = alternateSetting;
    setup.index = interfaceNumber;
    setup.length = 0;
    return USBCoreControlTransfer(device, &setup, 0, 0);
}

static int
get_exact_descriptor(USBCoreDevice *device, USBEnumerationData *result,
                     uhci_u8 stage, uhci_u8 type, uhci_u8 index, void *bytes,
                     uhci_u16 length)
{
    uhci_u16 actual = 0;
    result->stage = stage;
    result->expectedLength = length;
    result->actualLength = 0;
    if (!USBCoreGetDescriptor(device, type, index, bytes, length, &actual)) {
        result->actualLength = actual;
        result->error = USB_ENUM_ERROR_TRANSFER;
        return 0;
    }
    result->actualLength = actual;
    if (actual != length) {
        result->error = USB_ENUM_ERROR_SHORT;
        return 0;
    }
    return 1;
}

int
USBCoreReadDevice(USBCoreDevice *device, USBEnumerationData *result)
{
    uhci_u8 packet;

    if (!device || !result)
        return 0;
    bzero(result, sizeof(*result));
    if (!get_exact_descriptor(device, result, USB_ENUM_STAGE_DEVICE_8,
                              USB_DESC_DEVICE, 0, result->deviceDescriptor, 8))
        return 0;
    if (result->deviceDescriptor[0] < USB_DEVICE_DESCRIPTOR_BYTES ||
        result->deviceDescriptor[1] != USB_DESC_DEVICE) {
        result->error = USB_ENUM_ERROR_INVALID;
        return 0;
    }
    packet = result->deviceDescriptor[7];
    if (!packet) {
        result->error = USB_ENUM_ERROR_INVALID;
        return 0;
    }
    result->stage = USB_ENUM_STAGE_EP0_UPDATE;
    result->endpointZeroMaxPacket = packet;
    result->expectedLength = packet;
    result->actualLength = 0;
    if (!device->operations || !device->operations->updateEndpointZero ||
        !device->operations->updateEndpointZero(device->hostContext,
                                                device->deviceContext,
                                                packet)) {
        result->error = USB_ENUM_ERROR_EP0_UPDATE;
        return 0;
    }
    if (!get_exact_descriptor(device, result, USB_ENUM_STAGE_DEVICE_FULL,
                              USB_DESC_DEVICE, 0, result->deviceDescriptor,
                              USB_DEVICE_DESCRIPTOR_BYTES))
        return 0;
    if (result->deviceDescriptor[0] < USB_DEVICE_DESCRIPTOR_BYTES ||
        result->deviceDescriptor[1] != USB_DESC_DEVICE) {
        result->error = USB_ENUM_ERROR_INVALID;
        return 0;
    }
    result->vendorID = USBCoreReadLE16(result->deviceDescriptor + 8);
    result->productID = USBCoreReadLE16(result->deviceDescriptor + 10);
    result->numberConfigurations = result->deviceDescriptor[17];
    if (!result->numberConfigurations) {
        result->error = USB_ENUM_ERROR_INVALID;
        return 0;
    }

    result->stage = USB_ENUM_STAGE_COMPLETE;
    return 1;
}

int USBCoreReadConfiguration(USBCoreDevice *device, USBEnumerationData *result,
                            uhci_u8 index)
{
    uhci_u16 total;
    USBDescriptorIterator iterator;
    const uhci_u8 *descriptor;
    uhci_u8 length, type;
    int rc;
    if (!device || !result) return 0;
    result->configurationIndex = index;
    result->configurationValue = 0;
    result->configurationLength = 0;
    result->error = USB_ENUM_ERROR_NONE;
    if (index >= result->numberConfigurations) {
        result->error = USB_ENUM_ERROR_INVALID; return 0;
    }
    if (!get_exact_descriptor(device, result, USB_ENUM_STAGE_CONFIG_9,
                              USB_DESC_CONFIG, index,
                              result->configurationDescriptor,
                              USB_CONFIG_HEADER_BYTES))
        return 0;
    if (result->configurationDescriptor[0] < USB_CONFIG_HEADER_BYTES ||
        result->configurationDescriptor[1] != USB_DESC_CONFIG) {
        result->error = USB_ENUM_ERROR_INVALID;
        return 0;
    }
    total = USBCoreReadLE16(result->configurationDescriptor + 2);
    if (total < USB_CONFIG_HEADER_BYTES ||
        total > USB_MAX_CONFIG_DESCRIPTOR) {
        result->error = USB_ENUM_ERROR_INVALID;
        return 0;
    }
    if (!get_exact_descriptor(device, result, USB_ENUM_STAGE_CONFIG_FULL,
                              USB_DESC_CONFIG, index,
                              result->configurationDescriptor, total))
        return 0;
    if (result->configurationDescriptor[0] < USB_CONFIG_HEADER_BYTES ||
        result->configurationDescriptor[1] != USB_DESC_CONFIG ||
        USBCoreReadLE16(result->configurationDescriptor + 2) != total ||
        !result->configurationDescriptor[5]) {
        result->error = USB_ENUM_ERROR_INVALID;
        return 0;
    }
    result->configurationLength = total;
    result->configurationValue = result->configurationDescriptor[5];
    USBCoreDescriptorIteratorInitialize(&iterator, result->configurationDescriptor, total);
    while ((rc = USBCoreDescriptorNext(&iterator, &descriptor, &length, &type)) > 0) {
        if ((type == USB_DESC_INTERFACE && length < 9) ||
            (type == USB_DESC_ENDPOINT && length < 7)) {
            result->error = USB_ENUM_ERROR_INVALID; return 0;
        }
    }
    if (rc < 0) { result->error = USB_ENUM_ERROR_INVALID; return 0; }
    result->stage = USB_ENUM_STAGE_COMPLETE;
    result->error = USB_ENUM_ERROR_NONE;
    return 1;
}

int USBCoreEnumerateDevice(USBCoreDevice *device, USBEnumerationData *result)
{
    return USBCoreReadDevice(device, result) &&
        USBCoreReadConfiguration(device, result, 0);
}

int USBCoreSelectConfiguration(USBCoreDevice *device, USBEnumerationData *result,
                               USBConfigurationRank rank, void *context)
{
    unsigned i, selected = 0;
    int best = 0, failed = 0, score;
    if (!device || !result || !rank) return -1;
    for (i = 0; i < result->numberConfigurations; i++) {
        if (!USBCoreReadConfiguration(device, result, (uhci_u8)i)) {
            /* The observer logs the failing index and transfer diagnostic. */
            rank(context, result); failed = 1; continue;
        }
        score = rank(context, result);
        if (score < 0) failed = 1;
        if (score > best) { selected = i; best = score; }
    }
    if (failed) return -1;
    if (!best) return 0;
    return USBCoreReadConfiguration(device, result, (uhci_u8)selected) ? 1 : -1;
}

const char *
USBCoreEnumerationErrorName(uhci_u8 error)
{
    switch (error) {
    case USB_ENUM_ERROR_TRANSFER: return "control transfer failed";
    case USB_ENUM_ERROR_SHORT: return "short descriptor";
    case USB_ENUM_ERROR_INVALID: return "invalid descriptor";
    case USB_ENUM_ERROR_EP0_UPDATE: return "endpoint-zero update failed";
    default: return "unspecified error";
    }
}

const char *
USBCoreEnumerationStageName(uhci_u8 stage)
{
    switch (stage) {
    case USB_ENUM_STAGE_DEVICE_8: return "initial device descriptor";
    case USB_ENUM_STAGE_EP0_UPDATE: return "endpoint-zero update";
    case USB_ENUM_STAGE_DEVICE_FULL: return "device descriptor";
    case USB_ENUM_STAGE_CONFIG_9: return "configuration header";
    case USB_ENUM_STAGE_CONFIG_FULL: return "configuration descriptor";
    case USB_ENUM_STAGE_COMPLETE: return "complete";
    default: return "initialization";
    }
}

void
USBCoreDescriptorIteratorInitialize(USBDescriptorIterator *iterator,
                                    const uhci_u8 *bytes, uhci_u16 length)
{
    if (!iterator)
        return;
    iterator->bytes = bytes;
    iterator->length = length;
    iterator->offset = 0;
}

int
USBCoreDescriptorNext(USBDescriptorIterator *iterator,
                      const uhci_u8 **descriptor,
                      uhci_u8 *descriptorLength,
                      uhci_u8 *descriptorType)
{
    uhci_u8 length;
    if (!iterator || !iterator->bytes)
        return USB_DESCRIPTOR_MALFORMED;
    if (iterator->offset == iterator->length)
        return USB_DESCRIPTOR_END;
    if ((uhci_u16)(iterator->offset + 2) > iterator->length)
        return USB_DESCRIPTOR_MALFORMED;
    length = iterator->bytes[iterator->offset];
    if (length < 2 ||
        (uhci_u16)(iterator->offset + length) > iterator->length)
        return USB_DESCRIPTOR_MALFORMED;
    if (descriptor)
        *descriptor = iterator->bytes + iterator->offset;
    if (descriptorLength)
        *descriptorLength = length;
    if (descriptorType)
        *descriptorType = iterator->bytes[iterator->offset + 1];
    iterator->offset = (uhci_u16)(iterator->offset + length);
    return USB_DESCRIPTOR_FOUND;
}
