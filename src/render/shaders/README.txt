GPU shaders
=========================================

quad.hlsl is the single source of the instanced quad pipeline shared by D3D11, Vulkan and Metal.
The outputs in gen/ are committed and embedded as byte arrays; the build never needs a shader
compiler (KT_SHADER_TOOLS=prebuilt behaviour is the default for the compile itself, see
shaders.cmake). Regenerate with

    python3 tools/gen_shaders.py                 # every step whose tool is found
    python3 tools/gen_shaders.py --only dxbc     # on Windows (fxc of the Windows SDK)
    python3 tools/gen_shaders.py --check         # compare only (ctest -R shaders_up_to_date)

or the CMake target kt_regen_shaders. Each header carries a content hash (k...Hash).

Outputs and the tools used for the committed copies:

  gen/quad_spirv.h     glslang 16.6.0 (HLSL front end, -D -V --target-env vulkan1.0,
                       --shift-texture-binding 1), validated with spirv-val (SPIRV-Tools v2026.4)
  gen/quad_dxbc.h      fxc.exe "Direct3D Shader Compiler 10.1", Windows SDK 10.0.26100.0,
                       /T vs_4_0 + ps_4_0 /O3 /Qstrip_reflect /Qstrip_debug (feature level 10_0)
  gen/quad_metallib.h  Apple metal 32023.864 (Xcode 26.3), -std=macos-metal2.3
                       -mmacosx-version-min=13.0 -fno-fast-math, then xcrun metallib
  gen/quad.metal       MSL 2.3 translation of quad.hlsl, maintained by hand next to it.
                       it was derived with SPIRV-Cross; when spirv-cross is installed,
                       `gen_shaders.py --msl-from-spirv` rewrites it from the SPIR-V instead.
                       Keep both files in step: the GPU golden tests compare every backend with
                       the software renderer.

Bindings: cbuffer Frame = b0 (SPIR-V set 0 binding 0, Metal buffer 0); the batch texture = t0
(SPIR-V binding 1, Metal texture 0); instances = vertex buffer slot 0 (Metal buffer 1).
GLSL 330 / ES 300 for the OpenGL backend is not generated yet (that backend is a later step).
