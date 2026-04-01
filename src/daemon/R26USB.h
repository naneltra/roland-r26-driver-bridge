#ifndef R26_USB_H
#define R26_USB_H

#include <libusb.h>
#include <stdbool.h>
#include <stdint.h>

// Roland R-26 USB identifiers
#define R26_VENDOR_ID   0x0582  // Roland Corporation
#define R26_PRODUCT_ID  0x013E  // R-26

// USB transfer parameters
#define R26_NUM_ISO_PACKETS     8
#define R26_NUM_TRANSFERS       4
#define R26_MAX_PACKET_SIZE     1024

// Audio endpoint info discovered during probe
typedef struct {
    uint8_t     iface_num;      // USB interface number
    uint8_t     alt_setting;    // alternate setting for audio
    uint8_t     ep_in;          // input endpoint address (device -> host)
    uint8_t     ep_out;         // output endpoint address (host -> device)
    uint16_t    max_packet_in;  // max packet size for input
    uint16_t    max_packet_out; // max packet size for output
    int         interval;       // polling interval
} R26EndpointInfo;

// Device context
typedef struct {
    libusb_device_handle    *handle;
    libusb_context          *ctx;
    R26EndpointInfo         audio_ep;
    bool                    iface_claimed;
    bool                    kernel_detached;
    uint32_t                sample_rate;
    uint32_t                channels;
    uint32_t                bit_depth;
} R26Device;

// Initialize libusb and find the R-26
int r26_open(R26Device *dev);

// Close the device and release resources
void r26_close(R26Device *dev);

// Probe USB descriptors and find audio endpoints
int r26_probe(R26Device *dev);

// Print USB descriptor details to stdout
void r26_dump_descriptors(R26Device *dev);

// Set sample rate via vendor-specific control transfer (if needed)
int r26_set_sample_rate(R26Device *dev, uint32_t rate);

// Start isochronous audio capture
int r26_start_capture(R26Device *dev);

// Stop audio capture
void r26_stop_capture(R26Device *dev);

#endif // R26_USB_H
