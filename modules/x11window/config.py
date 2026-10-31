def can_build(env, platform):
    # Shows X11 windows (Composite + DRI3 dmabufs imported into the Vulkan RenderingDevice, or MIT-SHM copies) and sends
    # them input. libxcb is loaded at run time, so the build needs no X libraries.
    return platform == "linuxbsd" and env["vulkan"]


def configure(env):
    pass


def get_doc_classes():
    return [
        "X11WindowSource",
        "X11WindowTexture",
    ]


def get_doc_path():
    return "doc_classes"
