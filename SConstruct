#!/usr/bin/env python
import os
import sys

from methods import print_error


libname = "godot-vosk"
projectdir = "project"
vosk_dir = "thirdparty/vosk"

localEnv = Environment(tools=["default"], PLATFORM="")

# Build profiles can be used to decrease compile times.
# You can either specify "disabled_classes", OR
# explicitly specify "enabled_classes" which disables all other classes.
# Modify the example file as needed and uncomment the line below or
# manually specify the build_profile parameter when running SCons.

# localEnv["build_profile"] = "build_profile.json"

customs = ["custom.py"]
customs = [os.path.abspath(path) for path in customs]

opts = Variables(customs, ARGUMENTS)
opts.Update(localEnv)

Help(opts.GenerateHelpText(localEnv))

env = localEnv.Clone()

if not (os.path.isdir("godot-cpp") and os.listdir("godot-cpp")):
    print_error("""godot-cpp is not available within this folder, as Git submodules haven't been initialized.
Run the following command to download godot-cpp:

    git submodule update --init --recursive""")
    sys.exit(1)

env = SConscript("godot-cpp/SConstruct", {"env": env, "customs": customs})

env.Append(CPPPATH=["src/", vosk_dir + "/include/"])

# Vosk loaded at runtime (dlopen); include path needed for vosk_api.h.
# Linux needs -ldl; Android needs the build-time NEEDED entry for bionic.
platform = env["platform"]
arch     = env.get("arch", "")
vosk_lib_dir = None
if platform == "linux":
    if arch in ("", "x86_64"):
        vosk_lib_dir = vosk_dir + "/libs/linux/x86_64"
    env.Append(LIBS=["dl"])          # dlopen / dlsym / dladdr
elif platform == "macos":
    vosk_lib_dir = vosk_dir + "/libs/macos"
elif platform == "android":
    if arch in ("arm64", "arm64v8", "aarch64"):
        vosk_lib_dir = vosk_dir + "/libs/android/arm64-v8a"
    elif arch in ("arm32", "armv7", "armeabi-v7a"):
        vosk_lib_dir = vosk_dir + "/libs/android/armeabi-v7a"
    if vosk_lib_dir:
        env.Append(LIBPATH=[vosk_lib_dir])
        env.Append(LIBS=["vosk"])

# Set RPATH so the OS finds the Vosk library next to our extension at runtime.
if vosk_lib_dir:
    if platform == "linux":
        env.Append(LINKFLAGS=["-Wl,-rpath,$ORIGIN"])
    elif platform == "macos":
        env.Append(LINKFLAGS=["-Wl,-rpath,@loader_path"])
else:
    print("WARNING: Vosk prebuilt library not found for platform={} arch={};".format(platform, arch),
          "the plugin will fail to initialise at runtime.")

sources = Glob("src/*.cpp")

# Compile vosk_recognizer.cpp with -fexceptions (godot-cpp uses -fno-exceptions).
vosk_env = env.Clone()
if env.get("CC", "").startswith("cl") or env.get("platform", "") == "windows":
    vosk_env.Append(CXXFLAGS=["/EHsc"])
else:
    vosk_env.Append(CXXFLAGS=["-fexceptions"])

if env["target"] in ["editor", "template_debug"]:
    try:
        doc_data = env.GodotCPPDocData("src/gen/doc_data.gen.cpp", source=Glob("doc_classes/*.xml"))
        sources.append(doc_data)
    except AttributeError:
        print("Not including class reference as we're targeting a pre-4.3 baseline.")

vosk_sources = []
other_sources = []
for s in sources:
    if "vosk_recognizer" in str(s):
        vosk_sources.append(s)
    else:
        other_sources.append(s)

vosk_objs = [vosk_env.SharedObject(s) for s in vosk_sources]
compiled_sources = other_sources + vosk_objs

# .dev doesn't inhibit compatibility, so we don't need to key it.
# .universal just means "compatible with all relevant arches" so we don't need to key it.
suffix = env['suffix'].replace(".dev", "").replace(".universal", "")

lib_filename = "{}{}{}{}".format(env.subst('$SHLIBPREFIX'), libname, suffix, env.subst('$SHLIBSUFFIX'))

library = env.SharedLibrary(
    "bin/{}/{}".format(env['platform'], lib_filename),
    source=compiled_sources,
)

copy = env.Install("{}/bin/{}/".format(projectdir, env["platform"]), library)

# Copy the prebuilt Vosk library into project/bin/<platform>/.
vosk_copy = []
if platform == "linux" and vosk_lib_dir:
    vosk_copy = env.InstallAs(
        "{}/bin/linux/libvosk.so".format(projectdir),
        "{}/libvosk.so".format(vosk_lib_dir),
    )
elif platform == "macos" and vosk_lib_dir:
    vosk_copy = env.InstallAs(
        "{}/bin/macos/libvosk.dylib".format(projectdir),
        "{}/libvosk.dylib".format(vosk_lib_dir),
    )
elif platform == "android" and vosk_lib_dir:
    if arch in ("arm64", "arm64v8", "aarch64"):
        abi_suffix = "arm64-v8a"
    else:
        abi_suffix = "armeabi-v7a"
    vosk_copy = env.InstallAs(
        "{}/bin/android/{}/libvosk.so".format(projectdir, abi_suffix),
        "{}/libvosk.so".format(vosk_lib_dir),
    )

default_args = [library, copy] + (list(vosk_copy) if vosk_copy else [])
Default(*default_args)
