def can_build(env, platform):
    # V4L2 hardware video decoding, and Linux dmabufs (e.g. decoder output) imported into the Vulkan RenderingDevice.
    return platform == "linuxbsd" and env["vulkan"]


def configure(env):
    pass


def get_doc_classes():
    return [
        "AudioStreamMedia",
        "AudioStreamPlaybackMedia",
        "DmabufImage",
        "HWVideoPlaneTexture",
        "HWVideoTexture",
        "MediaSource",
        "V4L2Decoder",
    ]


def get_doc_path():
    return "doc_classes"
