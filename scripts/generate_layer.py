#!/usr/bin/env python3
"""Generate the C++ sources of the VK_LAYER_SYSTRACE_apitrace Vulkan layer.

Inputs
    * the Khronos registry (``vk.xml``), which describes every Vulkan command
    * the Vulkan headers shipped with the Android NDK

Only commands whose ``PFN_vk<name>`` typedef exists in the headers are
generated.  The registry is normally newer than the platform headers (for
example NDK r28 ships Vulkan 1.3.275 while vk.xml may describe 1.4.x), so this
filter is what keeps the generated code compilable against the headers that
actually matter at build time.

Outputs (written to ``src/generated/``)
    dispatch_structs.h   InstanceTable / DeviceTable
    trampolines.inc      one vkst_vkXxx() forwarder per command
    tables.inc           table initialisers, name and group arrays
    entry_points.inc     sorted name -> trampoline table used by the two
                         vkGet*ProcAddr entry points
"""

import argparse
import os
import re
import sys
import xml.etree.ElementTree as ET

# Layer name advertised through vkEnumerateInstanceLayerProperties.  Must match
# the value used by scripts/deploy.sh and the README.
LAYER_NAME = "VK_LAYER_SYSTRACE_apitrace"
LAYER_DESCRIPTION = "Trace every Vulkan call as an ATrace/Perfetto slice"

# Commands that src/layer_entry.cpp implements by hand.  They must never be
# generated, otherwise the exported symbols would be duplicated.
HAND_WRITTEN = {
    "vkGetInstanceProcAddr",
    "vkGetDeviceProcAddr",
    "vkCreateInstance",
    "vkDestroyInstance",
    "vkCreateDevice",
    "vkDestroyDevice",
    "vkEnumerateInstanceLayerProperties",
    "vkEnumerateDeviceLayerProperties",
    "vkEnumerateInstanceExtensionProperties",
    "vkEnumerateDeviceExtensionProperties",
}

# Commands that are neither generated nor implemented by hand.  The loader
# exposes these as its own exported globals (see api_gen.cpp), so a layer can
# never observe them.
NOT_TRACED = {
    "vkEnumerateInstanceVersion",
}

# Commands that the Android loader looks up with InitDispatchTable() in
# api_gen.cpp, split by the table it looks them up in.  A null answer for one of
# them makes the loader reject the instance or the device with "missing <obj>
# proc: vkXxx", so each has to be reachable through the matching vkGet*ProcAddr
# entry point; that in turn requires the command to sit in the right table here.
#
# Transcribed from the INIT_PROC(true, ...) lines of the two InitDispatchTable
# functions.  The extension commands next to them are guarded by an extension
# check in the loader and fall back to a stub, so they are not listed.
LOADER_REQUIRED_INSTANCE = """
DestroyInstance EnumeratePhysicalDevices GetInstanceProcAddr
GetPhysicalDeviceProperties GetPhysicalDeviceQueueFamilyProperties
GetPhysicalDeviceMemoryProperties GetPhysicalDeviceFeatures
GetPhysicalDeviceFormatProperties GetPhysicalDeviceImageFormatProperties
CreateDevice EnumerateDeviceExtensionProperties
GetPhysicalDeviceSparseImageFormatProperties
""".split()

LOADER_REQUIRED_DEVICE = """
GetDeviceProcAddr DestroyDevice GetDeviceQueue QueueSubmit QueueWaitIdle
DeviceWaitIdle AllocateMemory FreeMemory MapMemory UnmapMemory
FlushMappedMemoryRanges InvalidateMappedMemoryRanges GetDeviceMemoryCommitment
GetBufferMemoryRequirements BindBufferMemory GetImageMemoryRequirements
BindImageMemory GetImageSparseMemoryRequirements QueueBindSparse CreateFence
DestroyFence ResetFences GetFenceStatus WaitForFences CreateSemaphore
DestroySemaphore CreateEvent DestroyEvent GetEventStatus SetEvent ResetEvent
CreateQueryPool DestroyQueryPool GetQueryPoolResults CreateBuffer DestroyBuffer
CreateBufferView DestroyBufferView CreateImage DestroyImage
GetImageSubresourceLayout CreateImageView DestroyImageView CreateShaderModule
DestroyShaderModule CreatePipelineCache DestroyPipelineCache GetPipelineCacheData
MergePipelineCaches CreateGraphicsPipelines CreateComputePipelines DestroyPipeline
CreatePipelineLayout DestroyPipelineLayout CreateSampler DestroySampler
CreateDescriptorSetLayout DestroyDescriptorSetLayout CreateDescriptorPool
DestroyDescriptorPool ResetDescriptorPool AllocateDescriptorSets
FreeDescriptorSets UpdateDescriptorSets CreateFramebuffer DestroyFramebuffer
CreateRenderPass DestroyRenderPass GetRenderAreaGranularity CreateCommandPool
DestroyCommandPool ResetCommandPool AllocateCommandBuffers FreeCommandBuffers
BeginCommandBuffer EndCommandBuffer ResetCommandBuffer CmdBindPipeline
CmdSetViewport CmdSetScissor CmdSetLineWidth CmdSetDepthBias
CmdSetBlendConstants CmdSetDepthBounds CmdSetStencilCompareMask
CmdSetStencilWriteMask CmdSetStencilReference CmdBindDescriptorSets
CmdBindIndexBuffer CmdBindVertexBuffers CmdDraw CmdDrawIndexed CmdDrawIndirect
CmdDrawIndexedIndirect CmdDispatch CmdDispatchIndirect CmdCopyBuffer CmdCopyImage
CmdBlitImage CmdCopyBufferToImage CmdCopyImageToBuffer CmdUpdateBuffer
CmdFillBuffer CmdClearColorImage CmdClearDepthStencilImage CmdClearAttachments
CmdResolveImage CmdSetEvent CmdResetEvent CmdWaitEvents CmdPipelineBarrier
CmdBeginQuery CmdEndQuery CmdResetQueryPool CmdWriteTimestamp
CmdCopyQueryPoolResults CmdPushConstants CmdBeginRenderPass CmdNextSubpass
CmdEndRenderPass CmdExecuteCommands
""".split()

# Mask groups.  The order defines the bit position in debug.vk.systrace.mask
# and must match the GroupBit enum in src/trace.h.
GROUPS = ["instance", "device", "create", "queue", "record"]
GROUP_ID = {name: index for index, name in enumerate(GROUPS)}

INSTANCE_LEVEL_FIRST_PARAMS = {"VkInstance", "VkPhysicalDevice"}
DEVICE_LEVEL_FIRST_PARAMS = {
    "VkDevice",
    "VkQueue",
    "VkCommandBuffer",
}


def elem_text(node):
    """Flatten an element and its children back into plain text."""
    parts = []
    if node.text:
        parts.append(node.text)
    for child in node:
        parts.append(elem_text(child))
        if child.tail:
            parts.append(child.tail)
    return "".join(parts)


def squeeze(text):
    return " ".join(text.split())


def split_args(text):
    """Split a C parameter list on top-level commas."""
    args = []
    depth = 0
    current = ""
    for char in text:
        if char in "([":
            depth += 1
        elif char in ")]":
            depth -= 1
        if char == "," and depth == 0:
            args.append(current)
            current = ""
        else:
            current += char
    if current.strip():
        args.append(current)
    return args


# Promoted types are spelled twice in the ecosystem: the registry defines
# ``<type name="VkPushConstantsInfoKHR" alias="VkPushConstantsInfo"/>`` and the
# platform headers may use either spelling for the very same struct.  Signatures
# coming from vk.xml and from the headers therefore have to be folded onto a
# common spelling before they can be compared textually.
TYPE_IDENTIFIER_RE = re.compile(r"\bVk[A-Za-z0-9_]*\b")


def load_type_aliases(root):
    """Map every aliased type name to the name it is an alias of."""
    parent = {}
    for node in root.findall(".//types/type"):
        name = node.get("name")
        alias = node.get("alias")
        if name and alias:
            parent[name] = alias
    return parent


def alias_root(parent, name):
    """Follow the alias chain to the spelling that owns the definition."""
    seen = set()
    while name in parent and name not in seen:
        seen.add(name)
        name = parent[name]
    return name


def resolve_aliases(text, parent):
    """Rewrite every type name in a signature to its canonical spelling."""
    if not parent:
        return text
    return TYPE_IDENTIFIER_RE.sub(lambda m: alias_root(parent, m.group(0)), text)


def canonical_type(declaration, alias_parent=None):
    """Normalise a C parameter declaration down to its bare type.

    Array parameters are turned into pointers because that is what they decay
    to in a function type, which lets declarations coming from vk.xml be
    compared textually with the ones in the platform headers.
    """
    text = squeeze(declaration)
    array = ""
    match = re.search(r"\[[^\]]*\]\s*$", text)
    if match:
        array = "*"
        text = text[: match.start()].strip()
    match = re.search(r"[A-Za-z_][A-Za-z0-9_]*$", text)
    if match:
        text = text[: match.start()].strip()
    return resolve_aliases(squeeze(text + array), alias_parent)


def canonical_registry_type(type_text, alias_parent=None):
    """Same normalisation as canonical_type, for a type that has no name.

    parse_params() has already dropped the parameter name, so the trailing
    identifier belongs to the type and must not be removed.
    """
    text = squeeze(type_text)
    match = re.search(r"\[[^\]]*\]\s*$", text)
    if match:
        text = text[: match.start()].strip() + "*"
    return resolve_aliases(squeeze(text), alias_parent)


def header_pfn_typedef(pfn_name, header_text):
    """Match the typedef of one PFN with the Khronos headers' layout."""
    return re.search(
        r"^typedef\s+([^\n]*?)\s*\(\s*VKAPI_PTR\s*\*\s*%s\s*\)\s*\(([^\n]*)\)\s*;\s*$"
        % re.escape(pfn_name),
        header_text,
        re.M,
    )


def header_pfn_raw(pfn_name, header_text):
    """Return (return type, [parameter declarations]) as the headers spell them.

    The declarations keep their parameter names, so a trampoline built from them
    can both be assigned to the matching ``PFN_vkXxx`` slot and forward its
    arguments without any name bookkeeping.
    """
    match = header_pfn_typedef(pfn_name, header_text)
    if not match:
        return None
    raw = squeeze(match.group(2))
    if raw in ("", "void"):
        return squeeze(match.group(1)), []
    return squeeze(match.group(1)), [squeeze(arg) for arg in split_args(raw)]


def header_pfn_signature(pfn_name, header_text, alias_parent=None):
    """Return (return_type, [param types]) for a PFN typedef, or None.

    The Khronos headers keep every PFN typedef on a single line, so the match is
    anchored to a line; that also stops the return type from swallowing the rest
    of the file when a name is missing.  Types are folded onto their registry
    spelling, so that they can be compared with the ones from vk.xml.
    """
    raw = header_pfn_raw(pfn_name, header_text)
    if raw is None:
        return None
    return_type, declarations = raw
    return return_type, [
        canonical_type(declaration, alias_parent) for declaration in declarations
    ]


def proto_return_type(proto):
    """Return type of a <proto> element, without the command name."""
    parts = []
    if proto.text:
        parts.append(proto.text)
    for child in proto:
        if child.tag == "name":
            break
        parts.append(elem_text(child))
        if child.tail:
            parts.append(child.tail)
    return squeeze("".join(parts))


def parse_params(command):
    """Return [(type, name, declaration)] for every <param> of a command."""
    params = []
    for param in command.findall("param"):
        # A parameter may be restricted to one API flavour; Vulkan SC carries
        # its own copy of some parameters (api="vulkansc") and must be skipped.
        api = param.get("api")
        if api is not None and "vulkan" not in api.split(","):
            continue
        name = param.findtext("name")
        decl = squeeze(elem_text(param))

        # The declaration is "<type> <name>[<array suffix>]", so the name is the
        # last identifier before the optional array suffix.  A plain substring
        # search is not enough: "pInfo" also occurs inside
        # "VkCopyMemoryToMicromapInfoEXT".
        match = re.search(r"\b%s\b" % re.escape(name), decl)
        if match is None:
            raise ValueError("cannot locate %r in %r" % (name, decl))
        suffix = decl[match.end():].strip()
        if suffix and not re.fullmatch(r"(\[[^\]]*\])+", suffix):
            raise ValueError("unexpected text after %r in %r" % (name, decl))
        type_text = squeeze(decl[: match.start()] + suffix)
        if not type_text:
            raise ValueError("no type for %r in %r" % (name, decl))
        params.append((type_text, name, decl))
    return params


# Names that cannot be a parameter: a declaration that ends in one of these has
# no name, and the trailing identifier is a part of its type.
TYPE_KEYWORDS = frozenset(
    ["void", "char", "int", "float", "double", "struct", "const", "unsigned",
     "size_t", "int8_t", "int16_t", "int32_t", "int64_t", "uint8_t", "uint16_t",
     "uint32_t", "uint64_t", "intptr_t", "uintptr_t"]
)


def param_name_of(declaration):
    """Name of a parameter in a header declaration, or None if it has none.

    ``const VkMemoryMapInfoKHR* pMemoryMapInfo`` has the name ``pMemoryMapInfo``,
    while ``const VkMemoryMapInfoKHR*`` has none: after the trailing identifier
    is taken for a candidate, what is left has to still contain the type.
    """
    stripped = re.sub(r"\[[^\]]*\]\s*$", "", declaration).strip()
    match = re.search(r"([A-Za-z_][A-Za-z0-9_]*)$", stripped)
    if not match:
        return None
    name = match.group(1)
    if name in TYPE_KEYWORDS or name.startswith("Vk") or name.startswith("PFN_"):
        return None
    return name if stripped[: match.start()].strip() else None


def emitted_params(record_name, header_declarations, registry_params):
    """Parameter list of a trampoline, spelled the way the headers spell it.

    The trampoline is stored in a ``PFN_vkXxx`` slot, so its signature has to be
    the header's one: a promoted type such as ``VkMemoryMapInfo`` does not exist
    in an older header, which only declares ``VkMemoryMapInfoKHR``.  The
    parameter names come from the header declaration, so the forwarding call
    below always names the parameters that were declared.
    """
    params = []
    for index, declaration in enumerate(header_declarations):
        name = param_name_of(declaration)
        if name is None:
            name = registry_params[index][1] or ("arg%d" % index)
            declaration = squeeze("%s %s" % (declaration, name))
        params.append((declaration, name, declaration))
    return params


def check_required_commands(records):
    """Check that the loader can resolve every command it insists on.

    The loader fails instance or device creation outright when one of these is
    missing, so getting their level wrong is not a tracing problem but a
    "the application no longer starts" problem.
    """
    levels = {record["name"]: record["level"] for record in records}
    problems = []
    for names, wanted in ((LOADER_REQUIRED_INSTANCE, "instance"),
                          (LOADER_REQUIRED_DEVICE, "device")):
        for short in names:
            name = "vk" + short
            if name in HAND_WRITTEN:
                continue  # answered by the entry point itself
            level = levels.get(name)
            if level is None:
                problems.append((name, "not generated, so vkGet*ProcAddr "
                                       "cannot answer for it"))
            elif level != wanted:
                problems.append((name, "is %s level, the loader asks for it "
                                       "through the %s table"
                                 % (level, wanted)))
    return problems


def check_declared_types(records, header_text):
    """Report every type a generated trampoline names but the headers lack.

    The signature check above compares vk.xml with the headers in the registry's
    own spelling, so it cannot notice that a header spells a type differently;
    this is what catches a trampoline that would fail to compile.
    """
    present = set(re.findall(r"\bVk[A-Za-z0-9_]+\b", header_text))
    present |= set(re.findall(r"\bPFN_vk[A-Za-z0-9_]+\b", header_text))

    problems = []
    for record in records:
        text = " ".join(
            [record["return"]] + [decl for _, _, decl in record["params"]]
        )
        for name in TYPE_IDENTIFIER_RE.findall(text):
            if name not in present:
                problems.append((record["name"], name))
    return problems


def find_signature_mismatches(records, header_text, alias_parent=None):
    """Compare generated signatures with the PFN typedefs we cast to.

    The dispatch tables store the generated trampolines through
    reinterpret_cast, so the compiler cannot notice when vk.xml and the
    platform headers disagree about a command's signature.  Doing it here is
    what keeps those casts honest.
    """
    mismatches = []
    for record in records:
        expected = header_pfn_signature(
            "PFN_" + record["name"], header_text, alias_parent
        )
        if expected is None:
            mismatches.append((record["name"], "PFN_%s not found in headers"
                               % record["name"]))
            continue
        return_type, arg_types = expected
        got_args = [
            canonical_registry_type(type_text, alias_parent)
            for type_text in record["registry_params"]
        ]
        want_return = resolve_aliases(squeeze(return_type), alias_parent)
        got_return = resolve_aliases(squeeze(record["registry_return"]), alias_parent)
        if want_return != got_return:
            mismatches.append((record["name"], "return type: header %r vs generated %r"
                               % (return_type, record["return"])))
        if arg_types != got_args:
            for index in range(max(len(arg_types), len(got_args))):
                want = arg_types[index] if index < len(arg_types) else "<missing>"
                have = got_args[index] if index < len(got_args) else "<missing>"
                if want != have:
                    mismatches.append((record["name"],
                                       "arg %d: header %r vs generated %r"
                                       % (index, want, have)))
    return mismatches


def resolve_command(name, commands):
    """Follow <command alias=...> links to the command that owns the signature."""
    seen = set()
    while commands[name]["alias"]:
        if name in seen:
            raise ValueError("alias cycle at %s" % name)
        seen.add(name)
        name = commands[name]["alias"]
    return name


def command_name(command):
    """Name of a <command> definition.

    Commands that own their signature carry the name in <proto><name>, and the
    element itself has no name attribute.  Alias-only entries use name=.
    """
    proto = command.find("proto")
    if proto is not None:
        return proto.findtext("name")
    return command.get("name")


def load_commands(root):
    """Map every command name to its alias link and signature availability.

    vk.xml nests <commands> blocks inside <feature>/<extension> elements, and a
    promoted command may appear both as "name alias=other" and as the block
    that actually carries the signature.  Entries with a signature win.
    """
    commands = {}
    for command in root.findall(".//commands/command"):
        name = command_name(command)
        if not name:
            continue
        has_signature = command.find("proto") is not None
        entry = commands.get(name)
        if entry is None:
            commands[name] = {"alias": command.get("alias"),
                              "has_signature": has_signature}
        elif has_signature and not entry["has_signature"]:
            entry["has_signature"] = True
            entry["alias"] = None
    return commands


def header_pfn_names(header_paths):
    """Collect every PFN_vkXxx typedef declared by the given headers."""
    text = ""
    for path in header_paths:
        with open(path, "r") as handle:
            text += handle.read()
    return set(re.findall(r"\bPFN_(vk[A-Za-z0-9_]+)\s*\)", text))


def fallback_return(return_type):
    """Value returned when the next layer/driver has no entry for a command."""
    if return_type == "void":
        return "return;"
    if return_type == "VkResult":
        return "return VK_ERROR_EXTENSION_NOT_PRESENT;"
    if return_type == "VkBool32":
        return "return VK_FALSE;"
    if return_type.endswith("*") or return_type.startswith("PFN_"):
        return "return nullptr;"
    return "return {};"


def classify(full_name, first_param_type):
    """Return (level, group) for a command."""
    if first_param_type in INSTANCE_LEVEL_FIRST_PARAMS:
        level = "instance"
    elif first_param_type in DEVICE_LEVEL_FIRST_PARAMS:
        level = "device"
    else:
        level = None  # resolved by the caller (see NON_DISPATCHABLE_FIRST)

    short = full_name[2:]  # strip "vk"
    if short.startswith("Cmd"):
        group = "record"
    elif short.startswith("Queue") or full_name in (
        "vkAcquireNextImageKHR",
        "vkAcquireNextImage2KHR",
        "vkDeviceWaitIdle",
        "vkQueueWaitIdle",
        "vkResetFences",
        "vkWaitForFences",
        "vkWaitSemaphores",
        "vkWaitSemaphoresKHR",
        "vkSignalSemaphore",
        "vkSignalSemaphoreKHR",
        "vkGetFenceStatus",
        "vkGetSemaphoreCounterValue",
        "vkGetSemaphoreCounterValueKHR",
        "vkGetEventStatus",
        "vkQueuePresentKHR",
        "vkQueueBindSparse",
        "vkQueueSubmit",
        "vkQueueSubmit2",
        "vkQueueSubmit2KHR",
    ):
        group = "queue"
    elif short.startswith(("Create", "Destroy", "Allocate", "Free")):
        group = "create"
    elif level == "instance":
        group = "instance"
    else:
        group = "device"

    if level is None:
        # Commands whose leading parameter is not a dispatchable object still
        # belong to the device (e.g. VkExternalComputeQueueNV).  They resolve
        # their table through the "most recent device" fallback at run time.
        level = "device"
    return level, group


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(here)
    generated = os.path.join(root, "src", "generated")

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--vk-xml",
        default=os.environ.get("VK_XML", "/usr/share/vulkan/registry/vk.xml"),
        help="path to vk.xml (default: %(default)s)",
    )
    parser.add_argument(
        "--vulkan-header",
        action="append",
        default=None,
        help="header to filter commands against; repeatable",
    )
    parser.add_argument(
        "--print-skipped",
        action="store_true",
        help="list commands that were skipped because the headers lack them",
    )
    parser.add_argument(
        "--allow-signature-mismatch",
        action="store_true",
        help="generate anyway when vk.xml and the headers disagree",
    )
    args = parser.parse_args()

    headers = args.vulkan_header
    if not headers:
        ndk = os.environ.get("ANDROID_NDK_HOME", "")
        if ndk:
            sysroot = os.path.join(
                ndk, "toolchains", "llvm", "prebuilt", "linux-x86_64", "sysroot",
                "usr", "include", "vulkan",
            )
        else:
            sysroot = "/usr/include/vulkan"
        headers = [
            os.path.join(sysroot, "vulkan_core.h"),
            os.path.join(sysroot, "vulkan_android.h"),
        ]
    for header in headers:
        if not os.path.isfile(header):
            sys.exit("vulkan header not found: %s" % header)

    available = header_pfn_names(headers)
    header_text = ""
    for header in headers:
        with open(header, "r") as handle:
            header_text += handle.read()

    # The registry is parsed once and reused for the alias graph, the alias
    # links between commands, and the signature of every canonical command.
    registry = ET.parse(args.vk_xml).getroot()
    commands = load_commands(registry)
    alias_parent = load_type_aliases(registry)

    selected = []
    skipped = []
    for name in sorted(commands):
        if name in HAND_WRITTEN or name in NOT_TRACED:
            continue
        if name not in available:
            skipped.append(name)
            continue
        canonical = resolve_command(name, commands)
        if not commands[canonical]["has_signature"]:
            skipped.append(name)
            continue
        selected.append((name, canonical))

    if not selected:
        sys.exit("no commands selected; check --vulkan-header")

    # Parse the signature of every canonical command once.
    raw = {}
    for command in registry.findall(".//commands/command"):
        if command.find("proto") is None:
            continue
        raw[command_name(command)] = command

    records = []
    mismatches = []
    for name, canonical in selected:
        command = raw[canonical]
        params = parse_params(command)
        first_param = params[0][0] if params else ""
        level, group = classify(name, first_param)

        # The signature is taken from the headers rather than from the registry:
        # the trampoline is stored in a PFN_vkXxx slot, and only the header
        # spelling is assignable to it.  vk.xml may be newer than the headers,
        # so a promoted type can be spelled differently in the two.
        declared = header_pfn_raw("PFN_" + name, header_text)
        if declared is None:
            mismatches.append((name, "PFN_%s not found in the headers" % name))
            continue
        header_return, header_params = declared
        if len(header_params) != len(params):
            mismatches.append(
                (name, "the headers declare %d parameters, vk.xml %d"
                 % (len(header_params), len(params))))
            continue

        records.append(
            {
                "name": name,
                "canonical": canonical,
                # Emitted signature: spelled by the headers, which is what the
                # PFN_vkXxx slots are declared with.
                "return": header_return,
                "params": emitted_params(name, header_params, params),
                # Registry signature: kept in the registry's own spelling, so
                # that find_signature_mismatches() can still tell whether vk.xml
                # and the headers describe the same command.
                "registry_return": proto_return_type(command.find("proto")),
                "registry_params": [type_text for type_text, _, _ in params],
                "level": level,
                "group": group,
                "needs_handle_key": first_param
                not in INSTANCE_LEVEL_FIRST_PARAMS | DEVICE_LEVEL_FIRST_PARAMS,
            }
        )

    for index, record in enumerate(records):
        record["id"] = index
        record["slot"] = record["name"][2:]  # drop the "vk" prefix

    mismatches += find_signature_mismatches(records, header_text, alias_parent)
    if mismatches:
        print("error: %d generated command(s) disagree with the Vulkan headers:"
              % len(set(name for name, _ in mismatches)), file=sys.stderr)
        for name, detail in mismatches[:40]:
            print("  %s: %s" % (name, detail), file=sys.stderr)
        if len(mismatches) > 40:
            print("  ... %d more" % (len(mismatches) - 40), file=sys.stderr)
        if not args.allow_signature_mismatch:
            print("Refusing to generate code that would misinterpret arguments "
                  "at run time. Pass --allow-signature-mismatch to override.",
                  file=sys.stderr)
            return 1

    undeclared = check_declared_types(records, header_text)
    if undeclared:
        print("error: %d type(s) used by the generated trampolines are not "
              "declared by the headers:" % len(set(name for _, name in undeclared)),
              file=sys.stderr)
        for command, type_name in undeclared[:40]:
            print("  %s: %s" % (command, type_name), file=sys.stderr)
        print("These headers cannot compile the layer; generation stopped.",
              file=sys.stderr)
        return 1

    unresolvable = check_required_commands(records)
    if unresolvable:
        print("error: %d command(s) the loader insists on cannot be resolved:"
              % len(unresolvable), file=sys.stderr)
        for command, detail in unresolvable:
            print("  %s: %s" % (command, detail), file=sys.stderr)
        print("The loader would fail vkCreateInstance or vkCreateDevice with "
              "\"missing proc\"; generation stopped.", file=sys.stderr)
        return 1

    write_dispatch_structs(os.path.join(generated, "dispatch_structs.h"), records)
    write_trampolines(os.path.join(generated, "trampolines.inc"), records)
    write_tables(os.path.join(generated, "tables.inc"), records)
    write_entry_points(os.path.join(generated, "entry_points.inc"), records)

    counts = {}
    for record in records:
        counts[record["group"]] = counts.get(record["group"], 0) + 1
    print("generated %d commands" % len(records))
    for group in GROUPS:
        if group in counts:
            print("  %-9s %4d" % (group, counts[group]))
    odd = [r["name"] for r in records if r["needs_handle_key"]]
    if odd:
        print("  non-dispatchable leading parameter (fallback table): %s"
              % ", ".join(odd))
    if args.print_skipped:
        print("skipped %d commands not present in the headers:" % len(skipped))
        for name in skipped:
            print("  " + name)


BANNER = "// Generated by scripts/generate_layer.py -- do not edit.\n"


def write_dispatch_structs(path, records):
    with open(path, "w") as out:
        out.write(BANNER)
        out.write("#pragma once\n\n")
        out.write("#include <vulkan/vulkan.h>\n\n")
        out.write("namespace vkst {\n\n")
        out.write("// Instance level: the leading parameter is VkInstance or\n"
                  "// VkPhysicalDevice.\n")
        out.write("struct InstanceTable {\n")
        out.write("    uintptr_t key;\n")
        out.write("    PFN_vkGetInstanceProcAddr nextGIPA;\n")
        out.write("    PFN_vkGetDeviceProcAddr nextGDPA;\n")
        for record in records:
            if record["level"] == "instance":
                out.write("    %s %s;\n" % (pfn_name(record), record["slot"]))
        out.write("};\n\n")
        out.write("// Device level: the leading parameter is VkDevice, VkQueue or\n"
                  "// VkCommandBuffer.\n")
        out.write("struct DeviceTable {\n")
        out.write("    uintptr_t key;\n")
        out.write("    PFN_vkGetInstanceProcAddr nextGIPA;\n")
        out.write("    PFN_vkGetDeviceProcAddr nextGDPA;\n")
        for record in records:
            if record["level"] == "device":
                out.write("    %s %s;\n" % (pfn_name(record), record["slot"]))
        out.write("};\n\n")
        out.write("}  // namespace vkst\n")


def pfn_name(record):
    return "PFN_%s" % record["name"]


def trampoline_name(record):
    return "vkst_%s" % record["name"]


def signature(record):
    """Declaration of a trampoline, spelled like the loader's own prototypes.

    VKAPI_ATTR carries a calling-convention attribute on 32 bit ARM, and the
    tables store trampolines through reinterpret_cast, so the spelling has to
    match vulkan_core.h exactly.
    """
    args = ", ".join(decl for _, _, decl in record["params"]) or "void"
    return "VKAPI_ATTR %s VKAPI_CALL %s(%s)" % (
        record["return"], trampoline_name(record), args)


def call_args(record):
    return ", ".join(name for _, name, _ in record["params"])


def write_trampolines(path, records):
    with open(path, "w") as out:
        out.write(BANNER)
        out.write("\n// vkst_vkXxx() forwards one Vulkan command to the next\n"
                  "// layer/driver, wrapped in an ATrace slice.\n\n")
        for record in records:
            args = call_args(record)
            args = "(%s)" % args if args else "()"
            out.write("extern \"C\" %s {\n" % signature(record))
            out.write("    vkst::MaybeReload();\n")
            out.write("    vkst::TraceScope _vkst_scope{vkst::kCmdNames[%d], %d};\n"
                      % (record["id"], record["id"]))
            if record["needs_handle_key"]:
                out.write("    const uintptr_t _vkst_key = 0;\n")
                out.write("    vkst::DeviceTable* _vkst_t = vkst::FallbackDeviceTable();\n")
            elif record["level"] == "instance":
                first = record["params"][0][1]
                out.write("    const uintptr_t _vkst_key = vkst::KeyOf(%s);\n" % first)
                out.write("    vkst::InstanceTable* _vkst_t = "
                          "vkst::FindInstanceTable(_vkst_key);\n")
            else:
                first = record["params"][0][1]
                out.write("    const uintptr_t _vkst_key = vkst::KeyOf(%s);\n" % first)
                out.write("    vkst::DeviceTable* _vkst_t = "
                          "vkst::FindDeviceTable(_vkst_key);\n")
            out.write("    %s _vkst_fn = _vkst_t ? _vkst_t->%s : nullptr;\n"
                      % (pfn_name(record), record["slot"]))
            out.write("    if (VKST_UNLIKELY(!_vkst_fn)) {\n")
            out.write("        vkst::ReportMissingEntry(%d, _vkst_key, "
                      "_vkst_t != nullptr);\n" % record["id"])
            out.write("        %s\n" % fallback_return(record["return"]))
            out.write("    }\n")
            out.write("    return _vkst_fn%s;\n" % args)
            out.write("}\n\n")


def init_call(record, handle, gpa):
    return "%s(%s, \"%s\")" % (gpa, handle, record["name"])


def write_tables(path, records):
    instance_records = [r for r in records if r["level"] == "instance"]
    device_records = [r for r in records if r["level"] == "device"]

    with open(path, "w") as out:
        out.write(BANNER)
        out.write("\n")
        out.write("namespace vkst {\n\n")
        out.write("void InitInstanceTable(InstanceTable* t, uintptr_t key,\n"
                  "                       VkInstance instance,\n"
                  "                       PFN_vkGetInstanceProcAddr nextGIPA,\n"
                  "                       PFN_vkGetDeviceProcAddr nextGDPA) {\n")
        out.write("    t->key = key;\n")
        out.write("    t->nextGIPA = nextGIPA;\n")
        out.write("    t->nextGDPA = nextGDPA;\n")
        for record in instance_records:
            out.write("    t->%s = reinterpret_cast<%s>(%s);\n"
                      % (record["slot"], pfn_name(record),
                         init_call(record, "instance", "nextGIPA")))
        out.write("}\n\n")

        out.write("void InitDeviceTable(DeviceTable* t, uintptr_t key,\n"
                  "                     VkDevice device,\n"
                  "                     PFN_vkGetInstanceProcAddr nextGIPA,\n"
                  "                     PFN_vkGetDeviceProcAddr nextGDPA) {\n")
        out.write("    t->key = key;\n")
        out.write("    t->nextGIPA = nextGIPA;\n")
        out.write("    t->nextGDPA = nextGDPA;\n")
        for record in device_records:
            out.write("    t->%s = reinterpret_cast<%s>(%s);\n"
                      % (record["slot"], pfn_name(record),
                         init_call(record, "device", "nextGDPA")))
        out.write("}\n\n")

        out.write("extern const uint32_t kCommandCount = %d;\n\n" % len(records))
        out.write("extern const char* const kCmdNames[kCommandCount] = {\n")
        for record in records:
            out.write("    \"%s\",\n" % record["name"])
        out.write("};\n\n")
        out.write("extern const uint8_t kCmdGroups[kCommandCount] = {\n")
        for record in records:
            out.write("    kGroup%s,\n" % record["group"].capitalize())
        out.write("};\n\n")
        out.write("}  // namespace vkst\n")


def write_entry_points(path, records):
    ordered = sorted(records, key=lambda r: r["name"])
    with open(path, "w") as out:
        out.write(BANNER)
        out.write("\nnamespace vkst {\n\n")
        out.write("extern const Entry kEntries[] = {\n")
        for record in ordered:
            out.write("    {\"%s\", reinterpret_cast<PFN_vkVoidFunction>(%s), %s},\n"
                      % (record["name"], trampoline_name(record),
                         "true" if record["level"] == "instance" else "false"))
        out.write("};\n\n")
        out.write("extern const uint32_t kEntryCount = %d;\n\n" % len(ordered))
        out.write("}  // namespace vkst\n")


if __name__ == "__main__":
    main()
