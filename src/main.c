#include <err.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#include "capture.h"
#include "dithering.h"

static void set_mono_threshold(struct capture_info *info, uint8_t threshold)
{
    // Convert the 8-bit threshold to the number of bits for rgb565 comparisons
    // and pre-shift.
    info->mono_threshold_r5 = threshold >> 3;
    info->mono_threshold_g6 = (threshold >> 2) << 5;
    info->mono_threshold_b5 = (threshold >> 3) << 11;
}

static void set_dithering(struct capture_info *info, uint8_t value)
{
    info->dithering = value;
}

static int initialize(uint32_t device, int width, int height, struct capture_info *info)
{
    memset(info, 0, sizeof(*info));

    if (capture_initialize(device, width, height, info) < 0)
        return -1;

    // This is an arbitrary value that looks relatively good for a program that wasn't
    // designed for monochrome.
    set_mono_threshold(info, 25);

    info->buffer = (uint16_t *) malloc(info->capture_stride * info->capture_height * sizeof(uint16_t));
    info->work = (uint8_t *) malloc(info->capture_width * info->capture_height * 4);
    info->dithering_buffer = (int16_t *) malloc(info->capture_width * info->capture_height * sizeof(int16_t));

    return 0;
}

// NOTE: Resources *should* be cleaned up on process exit...
static void finalize(struct capture_info *info)
{
    free(info->buffer);
    free(info->work);
    free(info->dithering_buffer);

    capture_finalize(info);
}

static void write_stdout(void *buffer, size_t len)
{
    if (write(STDOUT_FILENO, buffer, len) != (ssize_t) len)
        err(EXIT_FAILURE, "write");
}

static uint8_t *add_packet_length(uint8_t *out, uint32_t size)
{
    out[0] = (size >> 24);
    out[1] = (size >> 16) & 0xff;
    out[2] = (size >> 8) & 0xff;
    out[3] = (size & 0xff);
    return out + 4;
}

static int emit_rgb24(const struct capture_info *info)
{
    int width = info->capture_width;
    int height = info->capture_height;
    const uint16_t *image = info->buffer;

    uint8_t *out = add_packet_length(info->work, 3 * width * height);

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            uint16_t pixel = image[x];
            out[0] = (pixel >> 11) << 3;
            out[1] = ((pixel >> 5) & 0x3f) << 2;
            out[2] = (pixel & 0x1f) << 3;
            out += 3;
        }
        image += info->capture_stride;
    }
    write_stdout(info->work, out - info->work);
    return 0;
}

static int emit_rgb565(const struct capture_info *info)
{
    int width = info->capture_width;
    int height = info->capture_height;
    const uint16_t *image = info->buffer;

    uint8_t *out = add_packet_length(info->work, sizeof(uint16_t) * width * height);

    int width_bytes = width * sizeof(uint16_t);
    for (int y = 0; y < height; y++) {
        memcpy(out, image, width_bytes);
        out += width_bytes;
        image += info->capture_stride;
    }
    write_stdout(info->work, out - info->work);
    return 0;
}

static inline int to_1bpp(const struct capture_info *info, uint16_t rgb565)
{
    if ((rgb565 & 0x001f) > info->mono_threshold_r5 ||
            (rgb565 & 0x07e0) > info->mono_threshold_g6 ||
            (rgb565 & 0xf800) > info->mono_threshold_b5)
        return 1;
    else
        return 0;
}

static int emit_mono(const struct capture_info *info)
{
    int width = info->capture_width;
    int height = info->capture_height;
    const uint16_t *image = info->buffer;
    const int16_t * buffer = info->dithering_buffer;
    size_t row_skip = info->capture_stride - info->capture_width;
    uint8_t *out = add_packet_length(info->work, width * height / 8);

    if (info->dithering == DITHERING_NONE) {
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x += 8) {
                *out = to_1bpp(info, image[0])
                       | (to_1bpp(info, image[1]) << 1)
                       | (to_1bpp(info, image[2]) << 2)
                       | (to_1bpp(info, image[3]) << 3)
                       | (to_1bpp(info, image[4]) << 4)
                       | (to_1bpp(info, image[5]) << 5)
                       | (to_1bpp(info, image[6]) << 6)
                       | (to_1bpp(info, image[7]) << 7);
                image += 8;
                out++;
            }
            image += row_skip;
        }
    } else {
        dithering_apply(info);

        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x += 8) {
                *out = (buffer[0] != 0 ? 1 : 0)
                       |  ((buffer[1] != 0 ? 1 : 0) << 1)
                       |  ((buffer[2] != 0 ? 1 : 0) << 2)
                       |  ((buffer[3] != 0 ? 1 : 0) << 3)
                       |  ((buffer[4] != 0 ? 1 : 0) << 4)
                       |  ((buffer[5] != 0 ? 1 : 0) << 5)
                       |  ((buffer[6] != 0 ? 1 : 0) << 6)
                       |  ((buffer[7] != 0 ? 1 : 0) << 7);

                buffer += 8;
                out++;
            }
        }
    }
    write_stdout(info->work, out - info->work);
    return 0;
}

static int emit_mono_rotate_flip(const struct capture_info *info)
{
    int width = info->capture_width;
    int height = info->capture_height;
    int stride = info->capture_stride;
    const uint16_t *image = info->buffer;
    const int16_t * dithering_buffer = info->dithering_buffer;

    uint8_t *out = add_packet_length(info->work, width * height / 8);

    if (info->dithering == DITHERING_NONE) {
        for (int x = 0; x < width; x++) {
            const uint16_t *column = image;
            for (int y = 0; y < height; y += 8) {
                *out = to_1bpp(info, column[0])
                       | (to_1bpp(info, column[stride]) << 1)
                       | (to_1bpp(info, column[2 * stride]) << 2)
                       | (to_1bpp(info, column[3 * stride]) << 3)
                       | (to_1bpp(info, column[4 * stride]) << 4)
                       | (to_1bpp(info, column[5 * stride]) << 5)
                       | (to_1bpp(info, column[6 * stride]) << 6)
                       | (to_1bpp(info, column[7 * stride]) << 7);
                column += 8 * stride;
                out++;
            }
            image++;
        }
    } else {
        dithering_apply(info);

        for (uint16_t x = 0; x < width; x++) {
            const uint16_t *column = dithering_buffer;
            for (uint16_t y = 0; y < height; y += 8) {
                *out = ((column[0] != 0 ? 1 : 0))
                       |  ((column[width] != 0 ? 1 : 0) << 1)
                       |  ((column[width * 2] != 0 ? 1 : 0) << 2)
                       |  ((column[width * 3] != 0 ? 1 : 0) << 3)
                       |  ((column[width * 4] != 0 ? 1 : 0) << 4)
                       |  ((column[width * 5] != 0 ? 1 : 0) << 5)
                       |  ((column[width * 6] != 0 ? 1 : 0) << 6)
                       |  ((column[width * 7] != 0 ? 1 : 0) << 7);

                column += 8 * width;
                out++;
            }
            dithering_buffer++;
        }
    }
    write_stdout(info->work, out - info->work);
    return 0;
}

static int emit_capture_info(const struct capture_info *info)
{
    uint8_t *out = add_packet_length(info->work, 36);
    memcpy(out, &info->backend_name, 16);
    out += 16;
    memcpy(out, &info->display_id, sizeof(uint32_t));
    out += sizeof(uint32_t);
    memcpy(out, &info->display_width, sizeof(uint32_t));
    out += sizeof(uint32_t);
    memcpy(out, &info->display_height, sizeof(uint32_t));
    out += sizeof(uint32_t);
    memcpy(out, &info->capture_width, sizeof(uint32_t));
    out += sizeof(uint32_t);
    memcpy(out, &info->capture_height, sizeof(uint32_t));
    out += sizeof(uint32_t);
    write_stdout(info->work, out - info->work);
    return 0;
}

static void handle_stdin(struct capture_info *info)
{
    int amount_read = read(STDIN_FILENO, &info->request_buffer[info->request_buffer_ix], MAX_REQUEST_BUFFER_SIZE - info->request_buffer_ix - 1);
    if (amount_read < 0)
        err(EXIT_FAILURE, "Error reading stdin");
    if (amount_read == 0) {
        finalize(info);
        exit(EXIT_SUCCESS);
    }
    info->request_buffer_ix += amount_read;

    // Check if there's a command.
    while (info->request_buffer_ix >= 5) {
        // The request format is:
        //
        // 00 00 00 len cmd args
        //
        // Commands:
        // 02 -> capture rgb24
        // 03 -> capture rgb565
        // 04 -> capture 1bpp
        // 05 -> capture 1bbp, but scan down the columns
        // 06 <threshold> -> set the monochrome conversion threshold (no response)
        // 07 <dithering> -> set the dithering algorithm (no response)

        // NOTE: The request format is what it is since we're using Erlang's built-in 4-byte length
        //       framing for simplicity.
        if (info->request_buffer[0] != 0 ||
                info->request_buffer[1] != 0 ||
                info->request_buffer[2] != 0)
            err(EXIT_FAILURE, "Unexpected command: %02x %02x %02x %02x", info->request_buffer[0], info->request_buffer[1], info->request_buffer[2], info->request_buffer[3]);

        uint8_t len = 4 + info->request_buffer[3];
        if (info->request_buffer_ix < len)
            break;

        switch (info->request_buffer[4]) {
        case 1:
        case 2:
        case 3:
        case 4:
        case 5:
            info->send_snapshot = info->request_buffer[4];
            break;

        case 6:
            set_mono_threshold(info, info->request_buffer[5]);
            break;

        case 7:
            set_dithering(info, info->request_buffer[5]);
            break;

        default: // ignore
            break;
        }
        info->request_buffer_ix -= len;

        if (info->request_buffer_ix > 0)
            memmove(info->request_buffer, info->request_buffer + len, info->request_buffer_ix);
    }
}

static int send_snapshot(struct capture_info *info)
{
    switch (info->send_snapshot) {
    case 1:
    case 2:
        return emit_rgb24(info);
    case 3:
        return emit_rgb565(info);
    case 4:
        return emit_mono(info);
    case 5:
        return emit_mono_rotate_flip(info);
    default:
        return 0;
    }
}

int main(int argc, char *argv[])
{
    if (argc != 4)
        errx(EXIT_FAILURE, "rpi_fb_capture <display> <w> <h>\n");

    uint32_t display_device = strtoul(argv[1], NULL, 0);
    int width = strtol(argv[2], NULL, 0);
    int height = strtol(argv[3], NULL, 0);

    struct capture_info info;
    if (initialize(display_device, width, height, &info) < 0)
        errx(EXIT_FAILURE, "capture initialization failed");

    emit_capture_info(&info);

    for (;;) {
        struct pollfd fdset[1];

        fdset[0].fd = STDIN_FILENO;
        fdset[0].events = POLLIN;
        fdset[0].revents = 0;

        int rc = poll(fdset, 1, -1);
        if (rc < 0)
            err(EXIT_FAILURE, "poll");

        if (fdset[0].revents & (POLLIN | POLLHUP))
            handle_stdin(&info);

        if (info.send_snapshot) {
            capture(&info);

            send_snapshot(&info);
            info.send_snapshot = 0;
        }
    }
}
