#!/usr/bin/env python3
"""Check the stock cellular discovery contracts in the local arm64 20F66 cache."""
import json
import plistlib
import re
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
SYSTEM = ROOT / "ios165/rootfs/System/Library"
CACHE = ROOT / "ios165/20F66__iPhone10,3_6/dyld_shared_cache_arm64"
IPSW = ROOT.parent / "ipsw-tools/ipsw"
ABP = "libIOABP.dylib"
BASE = "libTelephonyBasebandDynamic.dylib"
PCI = "libPCITransport.dylib"


def dyld(command, *args):
    return subprocess.check_output(
        [IPSW, "dyld", command, CACHE, *args], text=True, stderr=subprocess.DEVNULL)


def dis(image, symbol):
    return dyld("disass", "--symbol-image", image, "--symbol", symbol)


def ordered(body, *strings):
    for needle in strings:
        _, found, body = body.partition(needle)
        if not found:
            return False
    return True


def opens_type_zero(body):
    return bool(re.search(r"mov\s+w2, #0\n(?:[^\n]*\n){0,2}[^\n]*\bbl\s+_IOServiceOpen_stub", body))


def check():
    identities = {
        ABP: "A999D602-74AC-3CED-B3D5-CAD0FB341433",
        PCI: "B1123C38-5FFC-3DDF-9D1C-522BC50E1DB4",
        BASE: "1676A8FB-7875-36A5-8CCE-F674D502BF11",
        "ApplePDPHelper": "2EA0A72E-008C-3F52-B6C9-D376647342BC",
    }
    build = plistlib.loads((SYSTEM / "CoreServices/SystemVersion.plist").read_bytes())
    checks = {"rootfs_20f66": build["ProductBuildVersion"] == "20F66"}
    for image, uuid in identities.items():
        checks["uuid_" + image] = bool(re.search(
            r"LC_UUID\s+" + uuid, dyld("macho", image, "-l")))
    if not all(checks.values()):
        raise RuntimeError("Source identity mismatch; use the local arm64 20F66 restore artifacts")

    daemon = SYSTEM / "Frameworks/CoreTelephony.framework/Support/CommCenter"
    subprocess.run(["codesign", "--verify", "--strict", daemon], check=True)
    entitlements = plistlib.loads(subprocess.check_output(
        ["codesign", "-d", "--entitlements", ":-", daemon], stderr=subprocess.DEVNULL))
    for name in ("AppleBasebandPCI", "AppleBasebandPCIControl"):
        checks["signed_daemon_declares_" + name] = (
            entitlements.get("com.apple.driver." + name + ".user-access") is True)
    checks["stock_daemon_uuid"] = "5A907E0E-BF03-376B-94E6-E737BDD40300" in \
        subprocess.check_output(["xcrun", "dwarfdump", "--uuid", daemon], text=True)
    imports = subprocess.check_output(["xcrun", "nm", "-mu", daemon], text=True)
    checks["commcenter_imports_baseband_and_pdp_clients"] = all(
        name in imports for name in ("_TelephonyBasebandCreateController (from libTelephonyBasebandDynamic)",
                                     "__ZN23ApplePDPHelperInterface11getInstanceEv (from ApplePDPHelper)"))
    jobs = SYSTEM / "LaunchDaemons"
    for name, gate in (("com.apple.CommCenter.plist", "_LimitLoadToDeviceTree"),
                       ("com.apple.CommCenter-noncellular.plist", "_LimitLoadFromDeviceTree")):
        job = plistlib.loads((jobs / name).read_bytes())
        checks[name] = (job.get(gate) == "baseband" and job["Label"] == "com.apple.CommCenter" and
                        "com.apple.commcenter.coretelephony.xpc" in job["MachServices"])

    start = dis(ABP, "__ZN10IOABPClass8start_nlEPKcP11__CFRunLoopP16dispatch_queue_s"
                     "U13block_pointerFvjES7_U13block_pointerFv12abpErrorTypejiE")
    checks["channel_direct_cfstring_and_lifecycle"] = ordered(start,
        '"AppleBasebandPCIInterface"', "_IOServiceMatching_stub", "mov\tx20, x0",
        "_CFStringCreateWithCString_stub", "mov\tx24, x0", '"ABPInterfaceProtocol"',
        "mov\tx0, x20", "mov\tx2, x24", "_CFDictionarySetValue_stub",
        '"IOServiceFirstMatch"', "mov\tx2, x20", "_IOServiceAddMatchingNotification_stub",
        '"IOServiceTerminate"', "mov\tx2, x20", "_IOServiceAddMatchingNotification_stub")
    checks["control_class_lookup"] = ordered(dis(ABP, "__ZN17IOABPControlClass5startEv"),
        '"AppleBasebandPCIControl"', "_IOServiceMatching_stub", "_IOServiceGetMatchingService_stub")
    for label, symbol in (("control", "__ZN17IOABPControlClass7open_nlEP11__CFRunLoopP16dispatch_queue_s"),
                          ("channel", "__ZN10IOABPClass7open_nlEj")):
        body = dis(ABP, symbol)
        checks[label + "_open_type_zero_then_handshake"] = opens_type_zero(body) and ordered(
            body, "_IOServiceOpen_stub", "_IOConnectCallScalarMethod_stub")
    checks["channel_general_interest"] = ordered(dis(ABP, "__ZN10IOABPClass25setUpNotificationCallbackEj"),
        '"IOGeneralInterest"', "notificationCallback", "_IOServiceAddInterestNotification_stub")
    checks["notify_port_runloop_or_queue"] = ordered(dis(ABP, "__ZN10IOABPClass19setNotificationPortEv"),
        "_IONotificationPortCreate_stub", "_IONotificationPortGetRunLoopSource_stub",
        "_CFRunLoopAddSource_stub", "_IONotificationPortSetDispatchQueue_stub")
    checks["notification_port_before_matching"] = ordered(start,
        "IOABPClass19setNotificationPortEv", '"IOServiceFirstMatch"')
    for name, member in (("read", "9readAsyncEPvjPFvS0_iS0_ES0_"),
                         ("write", "10writeAsyncEPKvjPFvPviS2_ES2_")):
        symbol = "__ZN10IOABPClass" + member
        checks["pci_" + name + "_forwards_to_libioabp"] = symbol + "_stub" in dis(
            PCI, "__ZN9abpKernel" + member)
        body = dis(ABP, symbol)
        checks["channel_async_" + name + "_api"] = (
            ordered(body, "pthread_mutex_lock", "_IONotificationPortGetMachPort_stub",
                    "_IOConnectCallAsyncScalarMethod_stub", "pthread_mutex_unlock") and
            "no async port" in body)
    checks["channel_close_handshake_and_release"] = ordered(dis(ABP, "__ZN10IOABPClass8close_nlEv"),
        "_IOConnectCallScalarMethod_stub", "_IOServiceClose_stub", "_IOObjectRelease_stub")
    checks["channel_stop_cleans_up_queued_notifications"] = ordered(
        dis(ABP, "__ZN10IOABPClass4stopEP16dispatch_group_s"),
        "IOABPClass8close_nlEv", "IOABPClass14clearIteratorsEv",
        "IOABPClass21clearNotificationPortEv", "dispatch_group_enter", "dispatch_channel_async")
    checks["channel_stop_group_leave_after_block_release"] = ordered(
        dis(ABP, "____ZN10IOABPClass4stopEP16dispatch_group_s_block_invoke"),
        "_Block_release_stub", "dispatch_group_leave")
    checks["baseband_discovery"] = ordered(
        dis(BASE, "___TelephonyBasebandControllerInitContext_block_invoke_2"),
        '"AppleBaseband"', "_IOServiceMatching_stub", "_IOServiceGetMatchingService_stub")
    checks["baseband_late_publication"] = ordered(
        dis(BASE, "___TelephonyBasebandControllerInitContext_block_invoke.6"),
        '"AppleBaseband"', "_IOServiceMatching_stub", '"IOServiceMatched"',
        "_IOServiceAddMatchingNotification_stub")
    checks["baseband_type_zero"] = opens_type_zero(
        dis(BASE, "___TelephonyBasebandControllerInitContext_block_invoke.10"))
    checks["pdp_singleton_initializes"] = "__ZN23ApplePDPHelperInterface4initEv" in dis(
        "ApplePDPHelper", "____ZN23ApplePDPHelperInterface11getInstanceEv_block_invoke")
    pdp = dis("ApplePDPHelper", "__ZN23ApplePDPHelperInterface4initEv")
    checks["pdp_class_open_interest"] = opens_type_zero(pdp) and ordered(pdp,
        '"AppleIPAppender"', "_IOServiceMatching_stub", "_IOServiceGetMatchingService_stub",
        "_IOServiceOpen_stub", "_IOServiceAddInterestNotification_stub")

    probe = dis(PCI, "__ZN3pci9transport6kernel12probeVariantEv")
    picker = dis(PCI, "____ZN3pci9transport6kernel12probeVariantEv_block_invoke")
    checks["pci_variant_cached_per_process"] = ordered(probe, "sOnce", "E3ret", "_dispatch_once")
    checks["pci_variant_selection"] = ordered(picker, "x8, 0x20c64c000", "x8, x8, #0x410",
        "mov\tw2, #0x2", "initializer_list", "isServicePresentEPKc", "mov\tw8, #0")
    checks["pci_variant_lookup_by_class"] = ordered(dis(PCI, "__ZN3pci9transport6kernel16isServicePresentEPKc"),
        "_IOServiceMatching_stub", "_IOServiceGetMatchingService_stub")
    pci_strings = {int(a, 16): s for a, s in re.findall(
        r'^\s*(0x[\da-f]+): "([^"]+)"$', dyld("macho", PCI, "-s"), re.M)}
    controls = [int(w, 16) for w in dyld("dump", "0x20c64c410", "--addr", "--count", "4").splitlines()]
    checks["pci_variant_codes"] = [(pci_strings.get(addr), code) for addr, code in
        zip(controls[::2], controls[1::2])] == [("AppleBasebandPCIControl", 1), ("AppleConvergedIPCBBControl", 2)]
    checks["pci_listener_manager_class"] = ordered(dis(PCI, "__ZN3pci5event11PDPListener5startEv"),
        "probeVariantEv", '"AppleBasebandPCIPDPManagerBase"', "_IOServiceMatching_stub",
        "_IOServiceGetMatchingService_stub")

    # The pinned image uses a constexpr initializer of 18 enum / cstring pairs.
    addr = "0x20c64c430"
    mapping = dis(PCI, "__ZN9abpKernel26pciTransportInterfaceToABPE21PCITransportInterface")
    checks["channel_map_source"] = ordered(mapping, "data_" + addr[2:], "mov\tw2, #0x120",
                                            "_memmove", "mov\tw2, #0x12", "initializer_list")
    words = [int(word, 16) for word in dyld("dump", addr, "--addr", "--count", "36").splitlines()]
    channels = {key: pci_strings[value] for key, value in zip(words[::2], words[1::2])
                if pci_strings.get(value) in {"pbl", "sahara", "qmux1", "qmux2"}}
    checks["channel_map_values"] = channels == {0: "pbl", 1: "sahara", 4: "qmux1", 5: "qmux2"}
    return {"checks": checks, "channel_enum_to_string": channels}


if __name__ == "__main__":
    evidence = check()
    print(json.dumps(evidence, indent=2))
    raise SystemExit(not all(evidence["checks"].values()))
