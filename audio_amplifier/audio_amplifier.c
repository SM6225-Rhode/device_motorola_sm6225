/*
 * SPDX-FileCopyrightText: 2015 The CyanogenMod Project
 * SPDX-FileCopyrightText: 2020-2025 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "audio_amplifier"
//#define LOG_NDEBUG 0

#include <cutils/str_parms.h>
#include <dlfcn.h>
#include <hardware/audio_amplifier.h>
#include <hardware/hardware.h>
#include <log/log.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/types.h>

/* clang-format off */
#include "platform.h"
#include "platform_api.h"
/* clang-format on */

#define UNUSED __attribute__((unused))

typedef struct amp_device {
    amplifier_device_t amp_dev;
    struct audio_device* adev;
    struct audio_usecase* usecase_tx;
    struct pcm* aw882xx_out;
    typeof(enable_snd_device)* enable_snd_device;
    typeof(enable_audio_route)* enable_audio_route;
    typeof(disable_snd_device)* disable_snd_device;
    typeof(disable_audio_route)* disable_audio_route;
    typeof(platform_get_pcm_device_id)* platform_get_pcm_device_id;
    typeof(get_usecase_from_list)* get_usecase_from_list;
} aw_t;

static aw_t* aw_dev = NULL;

static int is_speaker(uint32_t snd_device) {
    int speaker = 0;
    switch (snd_device) {
        case SND_DEVICE_OUT_SPEAKER:
        case SND_DEVICE_OUT_SPEAKER_REVERSE:
        case SND_DEVICE_OUT_SPEAKER_AND_HEADPHONES:
        case SND_DEVICE_OUT_VOICE_SPEAKER:
        case SND_DEVICE_OUT_VOICE_SPEAKER_2:
        case SND_DEVICE_OUT_SPEAKER_AND_HDMI:
        case SND_DEVICE_OUT_SPEAKER_AND_USB_HEADSET:
        case SND_DEVICE_OUT_SPEAKER_AND_ANC_HEADSET:
            speaker = 1;
            break;
    }

    return speaker;
}

static int aw882xx_start_feedback(void* adev, uint32_t snd_device) {
    aw_dev->adev = (struct audio_device*)adev;
    int pcm_dev_tx_id = 0, rc = 0;
    struct pcm_config pcm_config_aw882xx = {
            .channels = 2,
            .rate = 48000,
            .period_size = 256,
            .period_count = 4,
            .format = PCM_FORMAT_S16_LE,
            .start_threshold = 0,
            .stop_threshold = INT_MAX,
            .silence_threshold = 0,
    };

    if (!aw_dev) {
        ALOGE("%d: Invalid params", __LINE__);
        return -EINVAL;
    }

    if (aw_dev->aw882xx_out || !is_speaker(snd_device)) return 0;

    aw_dev->usecase_tx = (struct audio_usecase*)calloc(1, sizeof(struct audio_usecase));
    if (!aw_dev->usecase_tx) {
        ALOGE("%d: failed to allocate usecase", __LINE__);
        return -ENOMEM;
    }
    aw_dev->usecase_tx->id = USECASE_AUDIO_SPKR_CALIB_TX;
    aw_dev->usecase_tx->type = PCM_CAPTURE;
    aw_dev->usecase_tx->in_snd_device = SND_DEVICE_IN_CAPTURE_VI_FEEDBACK;
    list_init(&aw_dev->usecase_tx->device_list);

    list_add_head(&aw_dev->adev->usecase_list, &aw_dev->usecase_tx->list);
    aw_dev->enable_snd_device(aw_dev->adev, aw_dev->usecase_tx->in_snd_device);
    aw_dev->enable_audio_route(aw_dev->adev, aw_dev->usecase_tx);

    pcm_dev_tx_id =
            aw_dev->platform_get_pcm_device_id(aw_dev->usecase_tx->id, aw_dev->usecase_tx->type);
    ALOGD("pcm_dev_tx_id = %d", pcm_dev_tx_id);
    if (pcm_dev_tx_id < 0) {
        ALOGE("%d: Invalid pcm device for usecase (%d)", __LINE__, aw_dev->usecase_tx->id);
        rc = -ENODEV;
        goto error;
    }

    aw_dev->aw882xx_out =
            pcm_open(aw_dev->adev->snd_card, pcm_dev_tx_id, PCM_IN, &pcm_config_aw882xx);
    if (!pcm_is_ready(aw_dev->aw882xx_out)) {
        ALOGE("%d: %s", __LINE__, pcm_get_error(aw_dev->aw882xx_out));
        rc = -EIO;
        goto error;
    }

    rc = pcm_start(aw_dev->aw882xx_out);
    if (rc < 0) {
        ALOGE("%d: pcm start for TX failed", __LINE__);
        rc = -EINVAL;
        goto error;
    }
    return 0;

error:
    ALOGE("%s: error case", __func__);
    if (aw_dev->aw882xx_out != 0) {
        pcm_close(aw_dev->aw882xx_out);
        aw_dev->aw882xx_out = NULL;
    }
    list_remove(&aw_dev->usecase_tx->list);
    aw_dev->disable_snd_device(aw_dev->adev, aw_dev->usecase_tx->in_snd_device);
    aw_dev->disable_audio_route(aw_dev->adev, aw_dev->usecase_tx);
    free(aw_dev->usecase_tx);

    return rc;
}

static void aw882xx_stop_feedback(void* adev, uint32_t snd_device) {
    aw_dev->adev = (struct audio_device*)adev;
    if (!aw_dev) {
        ALOGE("%s: Invalid params", __func__);
        return;
    }

    if (!is_speaker(snd_device)) return;

    if (aw_dev->aw882xx_out) {
        pcm_close(aw_dev->aw882xx_out);
        aw_dev->aw882xx_out = NULL;
    }

    aw_dev->disable_snd_device(aw_dev->adev, SND_DEVICE_IN_CAPTURE_VI_FEEDBACK);

    aw_dev->usecase_tx = aw_dev->get_usecase_from_list(aw_dev->adev, USECASE_AUDIO_SPKR_CALIB_TX);
    if (aw_dev->usecase_tx) {
        list_remove(&aw_dev->usecase_tx->list);
        aw_dev->disable_audio_route(aw_dev->adev, aw_dev->usecase_tx);
        free(aw_dev->usecase_tx);
    }
    return;
}

static int amp_set_feedback(UNUSED amplifier_device_t* device, void* adev, uint32_t devices,
                            bool enable) {
    aw_dev->adev = (struct audio_device*)adev;
    if (enable) {
        aw882xx_start_feedback(aw_dev->adev, devices);
    } else {
        aw882xx_stop_feedback(aw_dev->adev, devices);
    }
    return 0;
}

static int amp_dev_close(hw_device_t* device) {
    aw_t* dev = (aw_t*)device;
    if (dev) free(dev);

    return 0;
}

static int amp_module_open(const hw_module_t* module, const char* name, hw_device_t** device) {
    if (strcmp(name, AMPLIFIER_HARDWARE_INTERFACE)) {
        ALOGE("%s:%d: %s does not match amplifier hardware interface name\n", __func__, __LINE__,
              name);
        return -ENODEV;
    }

    aw_dev = calloc(1, sizeof(aw_t));
    if (!aw_dev) {
        ALOGE("%s:%d: Unable to allocate memory for amplifier device\n", __func__, __LINE__);
        return -ENOMEM;
    }

    aw_dev->amp_dev.common.tag = HARDWARE_DEVICE_TAG;
    aw_dev->amp_dev.common.module = (hw_module_t*)module;
    aw_dev->amp_dev.common.version = HARDWARE_DEVICE_API_VERSION(1, 0);
    aw_dev->amp_dev.common.close = amp_dev_close;

    aw_dev->amp_dev.set_input_devices = NULL;
    aw_dev->amp_dev.set_output_devices = NULL;
    aw_dev->amp_dev.enable_output_devices = NULL;
    aw_dev->amp_dev.enable_input_devices = NULL;
    aw_dev->amp_dev.set_mode = NULL;
    aw_dev->amp_dev.output_stream_start = NULL;
    aw_dev->amp_dev.input_stream_start = NULL;
    aw_dev->amp_dev.output_stream_standby = NULL;
    aw_dev->amp_dev.input_stream_standby = NULL;
    aw_dev->amp_dev.set_parameters = NULL;
    aw_dev->amp_dev.out_set_parameters = NULL;
    aw_dev->amp_dev.in_set_parameters = NULL;
    aw_dev->amp_dev.set_feedback = amp_set_feedback;

#define LOAD_AHAL_SYMBOL(symbol)                                          \
    do {                                                                  \
        aw_dev->symbol = dlsym(RTLD_NEXT, #symbol);                       \
        if (aw_dev->symbol == NULL) {                                     \
            ALOGW("%s: %s not found (%s)", __func__, #symbol, dlerror()); \
            free(aw_dev);                                                 \
            return -ENODEV;                                               \
        }                                                                 \
    } while (0)

    LOAD_AHAL_SYMBOL(enable_snd_device);
    LOAD_AHAL_SYMBOL(enable_audio_route);
    LOAD_AHAL_SYMBOL(disable_snd_device);
    LOAD_AHAL_SYMBOL(disable_audio_route);
    LOAD_AHAL_SYMBOL(platform_get_pcm_device_id);
    LOAD_AHAL_SYMBOL(get_usecase_from_list);

#undef LOAD_AHAL_SYMBOL

    *device = (hw_device_t*)aw_dev;

    return 0;
}

static struct hw_module_methods_t hal_module_methods = {
        .open = amp_module_open,
};

/* clang-format off */
amplifier_module_t HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .module_api_version = AMPLIFIER_MODULE_API_VERSION_0_1,
        .hal_api_version = HARDWARE_HAL_API_VERSION,
        .id = AMPLIFIER_HARDWARE_MODULE_ID,
        .name = "AW882XX audio amplifier HAL",
        .author = "The LineageOS Project",
        .methods = &hal_module_methods,
    },
};
