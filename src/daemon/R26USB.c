#include "R26USB.h"
#include "../shared/RingBuffer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Global pointer to shared memory (set by main before calling capture)
extern R26SharedAudio *g_shm;
extern volatile bool g_running;

int r26_open(R26Device *dev) {
    memset(dev, 0, sizeof(*dev));
    dev->sample_rate = 48000;
    dev->channels = 2;
    dev->bit_depth = 24;

    int rc = libusb_init(&dev->ctx);
    if (rc < 0) {
        fprintf(stderr, "r26d: libusb_init failed: %s\n", libusb_error_name(rc));
        return -1;
    }

    dev->handle = libusb_open_device_with_vid_pid(dev->ctx, R26_VENDOR_ID, R26_PRODUCT_ID);
    if (!dev->handle) {
        fprintf(stderr, "r26d: Roland R-26 not found (VID=%04x PID=%04x)\n",
                R26_VENDOR_ID, R26_PRODUCT_ID);
        fprintf(stderr, "r26d: Make sure the R-26 is connected via USB and powered on.\n");
        fprintf(stderr, "r26d: Run 'system_profiler SPUSBDataType' to check.\n");
        libusb_exit(dev->ctx);
        dev->ctx = NULL;
        return -1;
    }

    printf("r26d: Roland R-26 found!\n");

    // Log device speed — critical for isochronous transfer setup
    int speed = libusb_get_device_speed(libusb_get_device(dev->handle));
    const char *speed_str = "Unknown";
    switch (speed) {
        case LIBUSB_SPEED_LOW:       speed_str = "Low (1.5 Mbps)"; break;
        case LIBUSB_SPEED_FULL:      speed_str = "Full (12 Mbps)"; break;
        case LIBUSB_SPEED_HIGH:      speed_str = "High (480 Mbps)"; break;
        case LIBUSB_SPEED_SUPER:     speed_str = "Super (5 Gbps)"; break;
        case LIBUSB_SPEED_SUPER_PLUS: speed_str = "Super+ (10 Gbps)"; break;
    }
    printf("r26d: Device speed: %s\n", speed_str);

    // Try to set configuration 1 explicitly
    rc = libusb_set_configuration(dev->handle, 1);
    if (rc < 0) {
        printf("r26d: set_configuration(1): %s (may be OK if already set)\n",
               libusb_error_name(rc));
    } else {
        printf("r26d: Configuration 1 set\n");
    }

    // Detach kernel driver if attached (e.g. old kext or Apple's default)
    for (int i = 0; i < 4; i++) {
        if (libusb_kernel_driver_active(dev->handle, i) == 1) {
            printf("r26d: Detaching kernel driver from interface %d\n", i);
            rc = libusb_detach_kernel_driver(dev->handle, i);
            if (rc < 0 && rc != LIBUSB_ERROR_NOT_FOUND) {
                fprintf(stderr, "r26d: Warning: could not detach kernel driver from interface %d: %s\n",
                        i, libusb_error_name(rc));
            } else {
                dev->kernel_detached = true;
            }
        }
    }

    return 0;
}

void r26_close(R26Device *dev) {
    if (dev->iface_claimed) {
        libusb_release_interface(dev->handle, dev->audio_ep.iface_num);
        dev->iface_claimed = false;
    }
    if (dev->handle) {
        libusb_close(dev->handle);
        dev->handle = NULL;
    }
    if (dev->ctx) {
        libusb_exit(dev->ctx);
        dev->ctx = NULL;
    }
}

static const char *ep_type_str(uint8_t type) {
    switch (type & 0x03) {
        case LIBUSB_ENDPOINT_TRANSFER_TYPE_CONTROL:     return "Control";
        case LIBUSB_ENDPOINT_TRANSFER_TYPE_ISOCHRONOUS: return "Isochronous";
        case LIBUSB_ENDPOINT_TRANSFER_TYPE_BULK:        return "Bulk";
        case LIBUSB_ENDPOINT_TRANSFER_TYPE_INTERRUPT:   return "Interrupt";
        default: return "Unknown";
    }
}

void r26_dump_descriptors(R26Device *dev) {
    libusb_device *usbdev = libusb_get_device(dev->handle);
    struct libusb_device_descriptor desc;
    libusb_get_device_descriptor(usbdev, &desc);

    printf("\n=== Roland R-26 USB Descriptor Dump ===\n");
    printf("  Vendor ID:  0x%04x\n", desc.idVendor);
    printf("  Product ID: 0x%04x\n", desc.idProduct);
    printf("  Device Ver: %d.%d\n", desc.bcdDevice >> 8, desc.bcdDevice & 0xFF);
    printf("  Class:      0x%02x\n", desc.bDeviceClass);
    printf("  SubClass:   0x%02x\n", desc.bDeviceSubClass);
    printf("  Protocol:   0x%02x\n", desc.bDeviceProtocol);
    printf("  Configs:    %d\n", desc.bNumConfigurations);

    unsigned char str[256];
    if (desc.iManufacturer &&
        libusb_get_string_descriptor_ascii(dev->handle, desc.iManufacturer, str, sizeof(str)) > 0) {
        printf("  Manufacturer: %s\n", str);
    }
    if (desc.iProduct &&
        libusb_get_string_descriptor_ascii(dev->handle, desc.iProduct, str, sizeof(str)) > 0) {
        printf("  Product: %s\n", str);
    }
    if (desc.iSerialNumber &&
        libusb_get_string_descriptor_ascii(dev->handle, desc.iSerialNumber, str, sizeof(str)) > 0) {
        printf("  Serial: %s\n", str);
    }

    for (int ci = 0; ci < desc.bNumConfigurations; ci++) {
        struct libusb_config_descriptor *config;
        int rc = libusb_get_config_descriptor(usbdev, ci, &config);
        if (rc < 0) continue;

        printf("\n  Configuration %d:\n", config->bConfigurationValue);
        printf("    Interfaces: %d\n", config->bNumInterfaces);

        for (int ii = 0; ii < config->bNumInterfaces; ii++) {
            const struct libusb_interface *iface = &config->interface[ii];
            printf("\n    Interface %d (%d alt settings):\n", ii, iface->num_altsetting);

            for (int ai = 0; ai < iface->num_altsetting; ai++) {
                const struct libusb_interface_descriptor *alt = &iface->altsetting[ai];
                printf("      Alt Setting %d:\n", alt->bAlternateSetting);
                printf("        Class:    0x%02x", alt->bInterfaceClass);
                if (alt->bInterfaceClass == 0xFF) printf(" (Vendor-Specific)");
                else if (alt->bInterfaceClass == 0x01) printf(" (Audio)");
                printf("\n");
                printf("        SubClass: 0x%02x\n", alt->bInterfaceSubClass);
                printf("        Protocol: 0x%02x\n", alt->bInterfaceProtocol);
                printf("        Endpoints: %d\n", alt->bNumEndpoints);

                for (int ei = 0; ei < alt->bNumEndpoints; ei++) {
                    const struct libusb_endpoint_descriptor *ep = &alt->endpoint[ei];
                    printf("          EP 0x%02x: %s %s, MaxPacket=%d, Interval=%d\n",
                           ep->bEndpointAddress,
                           (ep->bEndpointAddress & 0x80) ? "IN " : "OUT",
                           ep_type_str(ep->bmAttributes),
                           ep->wMaxPacketSize,
                           ep->bInterval);

                    // Dump extra descriptors (class-specific)
                    if (ep->extra_length > 0) {
                        printf("            Extra data (%d bytes):", ep->extra_length);
                        for (int x = 0; x < ep->extra_length; x++) {
                            if (x % 16 == 0) printf("\n              ");
                            printf("%02x ", ep->extra[x]);
                        }
                        printf("\n");
                    }
                }

                // Dump interface extra descriptors
                if (alt->extra_length > 0) {
                    printf("        Extra data (%d bytes):", alt->extra_length);
                    for (int x = 0; x < alt->extra_length; x++) {
                        if (x % 16 == 0) printf("\n          ");
                        printf("%02x ", alt->extra[x]);
                    }
                    printf("\n");
                }
            }
        }
        libusb_free_config_descriptor(config);
    }
    printf("\n=== End Descriptor Dump ===\n\n");
}

int r26_probe(R26Device *dev) {
    libusb_device *usbdev = libusb_get_device(dev->handle);
    struct libusb_device_descriptor desc;
    libusb_get_device_descriptor(usbdev, &desc);

    struct libusb_config_descriptor *config;
    int rc = libusb_get_active_config_descriptor(usbdev, &config);
    if (rc < 0) {
        rc = libusb_get_config_descriptor(usbdev, 0, &config);
        if (rc < 0) {
            fprintf(stderr, "r26d: Cannot read config descriptor: %s\n", libusb_error_name(rc));
            return -1;
        }
    }

    // Roland R-26 uses vendor-specific class (0xFF) with isochronous endpoints.
    // We look for interfaces with isochronous IN endpoints (audio input from device).
    bool found_in = false;
    bool found_out = false;

    for (int ii = 0; ii < config->bNumInterfaces && !found_in; ii++) {
        const struct libusb_interface *iface = &config->interface[ii];
        for (int ai = 0; ai < iface->num_altsetting; ai++) {
            const struct libusb_interface_descriptor *alt = &iface->altsetting[ai];

            // Skip alt setting 0 (zero-bandwidth) for vendor-specific interfaces
            if (alt->bInterfaceClass == 0xFF && alt->bNumEndpoints == 0)
                continue;

            for (int ei = 0; ei < alt->bNumEndpoints; ei++) {
                const struct libusb_endpoint_descriptor *ep = &alt->endpoint[ei];
                uint8_t type = ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK;

                if (type == LIBUSB_ENDPOINT_TRANSFER_TYPE_ISOCHRONOUS) {
                    if ((ep->bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_IN) {
                        if (!found_in) {
                            dev->audio_ep.iface_num = alt->bInterfaceNumber;
                            dev->audio_ep.alt_setting = alt->bAlternateSetting;
                            dev->audio_ep.ep_in = ep->bEndpointAddress;
                            dev->audio_ep.max_packet_in = ep->wMaxPacketSize;
                            dev->audio_ep.interval = ep->bInterval;
                            found_in = true;
                            printf("r26d: Found audio IN endpoint 0x%02x on interface %d alt %d (maxpacket=%d)\n",
                                   ep->bEndpointAddress, alt->bInterfaceNumber,
                                   alt->bAlternateSetting, ep->wMaxPacketSize);
                        }
                    } else {
                        if (!found_out) {
                            dev->audio_ep.ep_out = ep->bEndpointAddress;
                            dev->audio_ep.max_packet_out = ep->wMaxPacketSize;
                            found_out = true;
                            printf("r26d: Found audio OUT endpoint 0x%02x (maxpacket=%d)\n",
                                   ep->bEndpointAddress, ep->wMaxPacketSize);
                        }
                    }
                }
            }
        }
    }

    int num_interfaces = config->bNumInterfaces;
    libusb_free_config_descriptor(config);

    if (!found_in) {
        fprintf(stderr, "r26d: No isochronous IN endpoint found.\n");
        fprintf(stderr, "r26d: The R-26 may need to be in USB Audio Interface mode.\n");
        fprintf(stderr, "r26d: On the R-26: Menu -> AUDIO I/F -> USB mode.\n");
        return -1;
    }

    // Claim all interfaces — Roland devices need both IN and OUT active
    for (int i = 0; i < num_interfaces; i++) {
        rc = libusb_claim_interface(dev->handle, i);
        if (rc < 0) {
            fprintf(stderr, "r26d: Cannot claim interface %d: %s\n",
                    i, libusb_error_name(rc));
            if (i == dev->audio_ep.iface_num) {
                fprintf(stderr, "r26d: Try running with sudo, or check USB permissions.\n");
                return -1;
            }
        } else {
            printf("r26d: Claimed interface %d\n", i);
        }
    }
    dev->iface_claimed = true;

    // Set alt setting 1 on the OUT interface (interface 1) to activate it
    if (found_out) {
        // The OUT endpoint was on interface 1 alt 1 (from descriptor dump)
        rc = libusb_set_interface_alt_setting(dev->handle, 1, 1);
        if (rc < 0) {
            fprintf(stderr, "r26d: Warning: Cannot set OUT interface alt setting: %s\n",
                    libusb_error_name(rc));
        } else {
            printf("r26d: Set interface 1 alt setting 1 (OUT)\n");
        }
    }

    // Set alt setting on the IN interface to allocate isochronous bandwidth
    rc = libusb_set_interface_alt_setting(dev->handle,
                                           dev->audio_ep.iface_num,
                                           dev->audio_ep.alt_setting);
    if (rc < 0) {
        fprintf(stderr, "r26d: Cannot set alt setting %d: %s\n",
                dev->audio_ep.alt_setting, libusb_error_name(rc));
        return -1;
    }
    printf("r26d: Set interface %d alt setting %d (IN)\n",
           dev->audio_ep.iface_num, dev->audio_ep.alt_setting);

    // Send Roland vendor-specific activation command
    // Roland devices typically need a vendor request to start the audio engine
    rc = libusb_control_transfer(dev->handle,
        LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_INTERFACE | LIBUSB_ENDPOINT_OUT,
        0x01,   // vendor request - start audio
        0x0000,
        0x0000, // interface 0 (control)
        NULL, 0,
        1000);
    if (rc < 0) {
        printf("r26d: Vendor activate (0x01) returned: %s (may be OK)\n",
               libusb_error_name(rc));
    } else {
        printf("r26d: Vendor activate sent\n");
    }

    // Try alternate activation: SET_CUR on sampling frequency to wake the device
    uint8_t freq_data[3] = { 0x80, 0xBB, 0x00 }; // 48000 Hz LE
    rc = libusb_control_transfer(dev->handle,
        LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_ENDPOINT | LIBUSB_ENDPOINT_OUT,
        0x01,   // SET_CUR
        0x0100, // Sampling Frequency Control
        dev->audio_ep.ep_in,
        freq_data, 3,
        1000);
    if (rc < 0) {
        printf("r26d: SET_CUR sample rate returned: %s (may be OK)\n",
               libusb_error_name(rc));
    } else {
        printf("r26d: Sample rate SET_CUR sent to EP 0x%02x\n", dev->audio_ep.ep_in);
    }

    // Deduce audio format from max packet size.
    // For Roland vendor-specific audio at 48kHz stereo 24-bit:
    //   48000 Hz / 1000 ms * 2 ch * 3 bytes = 288 bytes/ms
    //   With 8 frames per ms: varies by microframe timing
    // Common sizes: 288 (48k/24bit/stereo), 576 (96k), 192 (48k/16bit/stereo)
    uint16_t mps = dev->audio_ep.max_packet_in;
    printf("r26d: Max packet size: %d bytes\n", mps);

    // We'll determine the exact format when we receive data.
    // Default assumption: 24-bit stereo at 48kHz
    dev->channels = 2;
    dev->bit_depth = 24;
    dev->sample_rate = 48000;

    // Try to estimate from packet size
    // bytes_per_frame = channels * (bit_depth / 8)
    // frames_per_packet = mps / bytes_per_frame
    // At 48kHz with 1ms packets: expect ~48 frames
    // At 44.1kHz: ~44 frames
    // At 96kHz: ~96 frames
    if (mps > 0) {
        int bpf_24 = 2 * 3; // stereo 24-bit = 6 bytes/frame
        int bpf_16 = 2 * 2; // stereo 16-bit = 4 bytes/frame
        int frames_24 = mps / bpf_24;
        int frames_16 = mps / bpf_16;
        printf("r26d: Estimated frames per packet: %d (24-bit) or %d (16-bit)\n",
               frames_24, frames_16);

        // Common Roland packet sizes give hints
        if (frames_24 >= 44 && frames_24 <= 50) {
            dev->bit_depth = 24;
            dev->sample_rate = 48000;
        } else if (frames_24 >= 88 && frames_24 <= 100) {
            dev->bit_depth = 24;
            dev->sample_rate = 96000;
        } else if (frames_16 >= 44 && frames_16 <= 50) {
            dev->bit_depth = 16;
            dev->sample_rate = 48000;
        }
    }

    printf("r26d: Assumed format: %d Hz, %d channels, %d-bit\n",
           dev->sample_rate, dev->channels, dev->bit_depth);

    return 0;
}

int r26_set_sample_rate(R26Device *dev, uint32_t rate) {
    // Roland vendor-specific sample rate setting.
    // This may need adjustment based on the actual R-26 protocol.
    // Many Roland devices accept SET_CUR on the sampling frequency control.
    uint8_t data[3] = {
        (uint8_t)(rate & 0xFF),
        (uint8_t)((rate >> 8) & 0xFF),
        (uint8_t)((rate >> 16) & 0xFF)
    };

    int rc = libusb_control_transfer(dev->handle,
        LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_ENDPOINT | LIBUSB_ENDPOINT_OUT,
        0x01,   // SET_CUR
        0x0100, // Sampling Frequency Control
        dev->audio_ep.ep_in,
        data, 3,
        1000);

    if (rc < 0) {
        // Try vendor-specific approach
        rc = libusb_control_transfer(dev->handle,
            LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_INTERFACE | LIBUSB_ENDPOINT_OUT,
            0x01,   // vendor request
            rate & 0xFFFF,
            (rate >> 16) & 0xFFFF,
            NULL, 0,
            1000);

        if (rc < 0) {
            fprintf(stderr, "r26d: Could not set sample rate to %d Hz: %s\n",
                    rate, libusb_error_name(rc));
            fprintf(stderr, "r26d: Continuing with device default rate.\n");
            return -1;
        }
    }

    dev->sample_rate = rate;
    printf("r26d: Sample rate set to %d Hz\n", rate);
    return 0;
}

// Convert raw USB audio bytes to float samples and write to ring buffer
// R-26 sends 24-bit audio in 4-byte (32-bit) containers, little-endian
static void process_audio_data(R26Device *dev, const uint8_t *data, int length) {
    if (!g_shm || length <= 0) return;

    uint32_t ch = dev->channels;
    // R-26 uses 4-byte subframes (bSubframeSize=4) with 24-bit resolution
    uint32_t bytes_per_sample = 4;
    uint32_t bytes_per_frame = ch * bytes_per_sample;

    if (bytes_per_frame == 0) return;

    uint32_t nframes = length / bytes_per_frame;
    if (nframes == 0) return;

    float fbuf[512 * R26_MAX_CHANNELS];
    if (nframes > 512) nframes = 512;

    for (uint32_t f = 0; f < nframes; f++) {
        for (uint32_t c = 0; c < ch; c++) {
            const uint8_t *p = data + (f * bytes_per_frame) + (c * bytes_per_sample);
            // 24-bit audio in 32-bit LE container: data is in bytes [0..2], byte [3] is padding
            int32_t raw = (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16));
            if (raw & 0x800000) raw |= 0xFF000000; // sign extend
            fbuf[f * ch + c] = (float)raw / 8388608.0f; // 2^23
        }
    }

    r26_ring_write(g_shm, fbuf, nframes);
}

// Track active transfers so we can detect when all have failed
static volatile int g_active_transfers = 0;

// Scratch buffer for copying packet data before resubmitting
static uint8_t g_pkt_copy[1024];

// Isochronous transfer callback — must be fast to avoid frame scheduling slip
static void iso_callback(struct libusb_transfer *transfer) {
    if (!g_running) {
        free(transfer->buffer);
        libusb_free_transfer(transfer);
        __atomic_sub_fetch(&g_active_transfers, 1, __ATOMIC_SEQ_CST);
        return;
    }

    if (transfer->status == LIBUSB_TRANSFER_COMPLETED ||
        transfer->status == LIBUSB_TRANSFER_TIMED_OUT) {

        R26Device *dev = (R26Device *)transfer->user_data;

        // Collect packet info BEFORE resubmitting (time-critical)
        int pkt_count = transfer->num_iso_packets;
        uint32_t pkt_lengths[R26_NUM_ISO_PACKETS];
        uint32_t pkt_offsets[R26_NUM_ISO_PACKETS];
        uint32_t total_copy = 0;

        for (int i = 0; i < pkt_count; i++) {
            struct libusb_iso_packet_descriptor *pkt = &transfer->iso_packet_desc[i];
            pkt_lengths[i] = 0;
            pkt_offsets[i] = total_copy;
            if (pkt->status == LIBUSB_TRANSFER_COMPLETED && pkt->actual_length > 0) {
                uint32_t len = pkt->actual_length;
                if (total_copy + len <= sizeof(g_pkt_copy)) {
                    uint8_t *src = libusb_get_iso_packet_buffer_simple(transfer, i);
                    memcpy(g_pkt_copy + total_copy, src, len);
                    pkt_lengths[i] = len;
                    total_copy += len;
                }
            }
        }

        // Resubmit IMMEDIATELY — before processing audio data
        libusb_set_iso_packet_lengths(transfer, transfer->iso_packet_desc[0].length);
        int rc = libusb_submit_transfer(transfer);
        if (rc < 0) {
            free(transfer->buffer);
            libusb_free_transfer(transfer);
            if (__atomic_sub_fetch(&g_active_transfers, 1, __ATOMIC_SEQ_CST) <= 0) {
                fprintf(stderr, "r26d: All IN transfers failed, stopping.\n");
                g_running = false;
            }
            return;
        }

        // Now process the copied audio data (no longer time-critical)
        for (int i = 0; i < pkt_count; i++) {
            if (pkt_lengths[i] > 0) {
                process_audio_data(dev, g_pkt_copy + pkt_offsets[i], pkt_lengths[i]);
            }
        }

    } else if (transfer->status == LIBUSB_TRANSFER_CANCELLED) {
        free(transfer->buffer);
        libusb_free_transfer(transfer);
        __atomic_sub_fetch(&g_active_transfers, 1, __ATOMIC_SEQ_CST);
    } else {
        // Try to resubmit on error
        int rc = libusb_submit_transfer(transfer);
        if (rc < 0) {
            free(transfer->buffer);
            libusb_free_transfer(transfer);
            if (__atomic_sub_fetch(&g_active_transfers, 1, __ATOMIC_SEQ_CST) <= 0) {
                fprintf(stderr, "r26d: All IN transfers failed, stopping.\n");
                g_running = false;
            }
        }
    }
}

// Callback for OUT (playback) transfers — resubmit silence immediately
static void iso_out_callback(struct libusb_transfer *transfer) {
    if (!g_running) {
        free(transfer->buffer);
        libusb_free_transfer(transfer);
        return;
    }

    // Resubmit immediately regardless of status (silence — no data to preserve)
    libusb_set_iso_packet_lengths(transfer, transfer->iso_packet_desc[0].length);
    int rc = libusb_submit_transfer(transfer);
    if (rc < 0) {
        free(transfer->buffer);
        libusb_free_transfer(transfer);
    }
}

int r26_start_capture(R26Device *dev) {
    printf("r26d: Starting isochronous capture from EP 0x%02x\n", dev->audio_ep.ep_in);

    uint16_t mps_in = dev->audio_ep.max_packet_in;
    if (mps_in == 0) mps_in = R26_MAX_PACKET_SIZE;

    uint16_t mps_out = dev->audio_ep.max_packet_out;
    if (mps_out == 0) mps_out = 64;

    // Submit OUT (playback) transfers with silence to kick-start the device
    if (dev->audio_ep.ep_out) {
        printf("r26d: Starting silence output on EP 0x%02x to activate device\n",
               dev->audio_ep.ep_out);
        for (int i = 0; i < R26_NUM_TRANSFERS; i++) {
            struct libusb_transfer *transfer = libusb_alloc_transfer(R26_NUM_ISO_PACKETS);
            if (!transfer) continue;

            uint8_t *buf = calloc(R26_NUM_ISO_PACKETS, mps_out);
            if (!buf) {
                libusb_free_transfer(transfer);
                continue;
            }

            libusb_fill_iso_transfer(transfer, dev->handle,
                                      dev->audio_ep.ep_out,
                                      buf,
                                      R26_NUM_ISO_PACKETS * mps_out,
                                      R26_NUM_ISO_PACKETS,
                                      iso_out_callback,
                                      dev,
                                      1000);

            libusb_set_iso_packet_lengths(transfer, mps_out);

            int rc = libusb_submit_transfer(transfer);
            if (rc < 0) {
                fprintf(stderr, "r26d: Cannot submit OUT transfer %d: %s\n",
                        i, libusb_error_name(rc));
                free(buf);
                libusb_free_transfer(transfer);
            } else {
                printf("r26d: OUT transfer %d submitted\n", i);
            }
        }
    }

    // Submit IN (capture) transfers
    for (int i = 0; i < R26_NUM_TRANSFERS; i++) {
        struct libusb_transfer *transfer = libusb_alloc_transfer(R26_NUM_ISO_PACKETS);
        if (!transfer) {
            fprintf(stderr, "r26d: Cannot allocate transfer\n");
            return -1;
        }

        uint8_t *buf = calloc(R26_NUM_ISO_PACKETS, mps_in);
        if (!buf) {
            libusb_free_transfer(transfer);
            fprintf(stderr, "r26d: Cannot allocate transfer buffer\n");
            return -1;
        }

        libusb_fill_iso_transfer(transfer, dev->handle,
                                  dev->audio_ep.ep_in,
                                  buf,
                                  R26_NUM_ISO_PACKETS * mps_in,
                                  R26_NUM_ISO_PACKETS,
                                  iso_callback,
                                  dev,
                                  1000);

        libusb_set_iso_packet_lengths(transfer, mps_in);

        int rc = libusb_submit_transfer(transfer);
        if (rc < 0) {
            fprintf(stderr, "r26d: Cannot submit IN transfer %d: %s\n", i, libusb_error_name(rc));
            free(buf);
            libusb_free_transfer(transfer);
            return -1;
        }
        __atomic_add_fetch(&g_active_transfers, 1, __ATOMIC_SEQ_CST);
    }

    printf("r26d: Capture started (%d IN + %d OUT transfers, %d packets each)\n",
           R26_NUM_TRANSFERS, R26_NUM_TRANSFERS, R26_NUM_ISO_PACKETS);

    // Update shared memory header
    if (g_shm) {
        atomic_store(&g_shm->sample_rate, dev->sample_rate);
        atomic_store(&g_shm->channels, dev->channels);
        atomic_store(&g_shm->bits_per_sample, dev->bit_depth);
        atomic_store(&g_shm->status, R26_STATUS_RUNNING);
    }

    // Event loop
    while (g_running) {
        int rc = libusb_handle_events_timeout(dev->ctx, &(struct timeval){0, 100000});
        if (rc < 0 && rc != LIBUSB_ERROR_INTERRUPTED) {
            fprintf(stderr, "r26d: Event handling error: %s\n", libusb_error_name(rc));
            break;
        }
    }

    if (g_shm) {
        atomic_store(&g_shm->status, R26_STATUS_STOPPED);
    }

    return 0;
}

void r26_stop_capture(R26Device *dev) {
    (void)dev;
    g_running = false;
}
