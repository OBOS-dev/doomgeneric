#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include <obos/syscall.h>

enum hda_ioctls {
    IOCTL_HDA_BASE_IOCTLs = 0x100,

    IOCTL_HDA_OUTPUT_STREAM_COUNT,
    IOCTL_HDA_OUTPUT_STREAM_SELECT,
    IOCTL_HDA_OUTPUT_STREAM_SELECTED,

    IOCTL_HDA_CODEC_COUNT,
    IOCTL_HDA_CODEC_SELECT,
    IOCTL_HDA_CODEC_SELECTED,

    IOCTL_HDA_CODEC_OUTPUT_GROUP_COUNT,
    IOCTL_HDA_CODEC_SELECT_OUTPUT_GROUP,
    IOCTL_HDA_CODEC_SELECTED_OUTPUT_GROUP,

    IOCTL_HDA_OUTPUT_GROUP_OUTPUT_COUNT,
    IOCTL_HDA_OUTPUT_GROUP_SELECT_OUTPUT,
    IOCTL_HDA_OUTPUT_GROUP_SELECTED_OUTPUT,

    IOCTL_HDA_OUTPUT_GET_PRESENCE,
    IOCTL_HDA_OUTPUT_GET_INFO,

    IOCTL_HDA_STREAM_SETUP,
    IOCTL_HDA_STREAM_PLAY,
    IOCTL_HDA_STREAM_QUEUE_DATA, // No parameters, but the next write will queue the data written
    IOCTL_HDA_STREAM_CLEAR_QUEUE,
    IOCTL_HDA_STREAM_SHUTDOWN,
    IOCTL_HDA_STREAM_GET_STATUS,
    IOCTL_HDA_STREAM_GET_REMAINING,
    IOCTL_HDA_STREAM_GET_BUFFER_SIZE,

    IOCTL_HDA_PATH_FIND,
    IOCTL_HDA_PATH_SETUP,
    IOCTL_HDA_PATH_SHUTDOWN,
    IOCTL_HDA_PATH_VOLUME,
    IOCTL_HDA_PATH_MUTE,

    // takes in a handle instead of a struct fd
    IOCTL_HDA_STREAM_SETUP_USER,
};

enum {
    FORMAT_PCM8,
    FORMAT_PCM16,
    FORMAT_PCM20,
    FORMAT_PCM24,
    FORMAT_PCM32,
};

typedef          size_t *hda_get_size_parameter;
typedef          size_t *hda_get_count_parameter;
typedef          size_t *hda_get_index_parameter;
typedef    const size_t *hda_set_index_parameter;
typedef            bool *hda_output_get_presence_parameter;
typedef const uintptr_t *hda_path_shutdown_parameter;
typedef struct stream_parameters {
    uint32_t sample_rate; // in hertz
    uint32_t channels;
    uint8_t format;
} stream_parameters;
typedef struct hda_stream_setup_parameters {
    stream_parameters stream_params;
    uint32_t ring_buffer_size;
    // doesn't need to be a pipe
    // can be nullptr
    struct fd* ring_buffer_pipe; 
} *hda_stream_setup_parameters;
typedef struct hda_stream_setup_user_parameters {
    stream_parameters stream_params;
    uint32_t ring_buffer_size;
    // doesn't need to be a pipe
    // can be HANDLE_INVALID
    handle ring_buffer_pipe; 
} *hda_stream_setup_user_parameters;
typedef const bool *hda_stream_play;
typedef struct hda_path_find_parameters {
    bool same_stream; // whether all paths will be playing the same stream.
    size_t other_path_count;
    uintptr_t found_path; // output.
    uintptr_t other_paths[];
} *hda_path_find_parameters;
typedef struct hda_path_setup_parameters {
    uintptr_t path;
    stream_parameters stream_parameters; // stream parameters hint<-->actual stream parameters
} *hda_path_setup_parameters;
typedef uint32_t* hda_path_get_status_parameter;
struct hda_path_boolean_parameter {
    uintptr_t path;
    bool par1;
};
struct hda_path_byte_parameter {
    uintptr_t path;
    uint8_t par1;
};
typedef const struct hda_path_boolean_parameter *hda_path_mute_parameter;
typedef const struct hda_path_byte_parameter *hda_path_volume_parameter;