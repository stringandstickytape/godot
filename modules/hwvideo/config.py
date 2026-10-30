def can_build(env, platform):
    # Imports Linux dmabufs (e.g. hardware video decoder output) into the Vulkan RenderingDevice.
    return platform == "linuxbsd" and env["vulkan"]


def configure(env):
    pass


def get_doc_classes():
    return [
        "DmabufImage",
    ]


def get_doc_path():
    return "doc_classes"
