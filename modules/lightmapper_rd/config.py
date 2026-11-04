def can_build(env, platform):
    # Fork: also in export templates, so a game can bake its lightmaps at run time (needs a RenderingDevice).
    return env.editor_build or env["vulkan"] or env["d3d12"] or env["metal"]


def configure(env):
    pass
