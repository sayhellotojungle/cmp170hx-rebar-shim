#!/usr/bin/env python3
"""Real BAR1 P2P transfer test using the CUDA driver API (ctypes).

No CUDA toolkit/nvcc needed: libcuda.so is installed by the NVIDIA driver.
It enables peer access between every ordered pair of CUDA devices, copies a
64 MiB pattern with cuMemcpyPeer, verifies the destination bytes and checks
that the source is unchanged.
"""

import ctypes
import struct
import sys

CUDA_SUCCESS = 0
CUDA_ERROR_PEER_ACCESS_ALREADY_ENABLED = 704
CUDA_ERROR_PEER_ACCESS_UNSUPPORTED = 706

CU_DEVICE_ATTRIBUTE_PCI_DOMAIN_ID = 35
CU_DEVICE_ATTRIBUTE_PCI_BUS_ID = 33
CU_DEVICE_ATTRIBUTE_PCI_DEVICE_ID = 34

N = 64 * 1024 * 1024  # bytes per device

lib = ctypes.CDLL("libcuda.so.1")


def resolve(*names):
    for n in names:
        try:
            return getattr(lib, n)
        except AttributeError:
            continue
    raise AttributeError(names)


def proto(f, argtypes):
    f.restype = ctypes.c_int
    f.argtypes = argtypes
    return f


cuInit = proto(resolve("cuInit"), [ctypes.c_uint])
cuDeviceGetCount = proto(resolve("cuDeviceGetCount"),
                         [ctypes.POINTER(ctypes.c_int)])
cuDeviceGet = proto(resolve("cuDeviceGet"),
                    [ctypes.POINTER(ctypes.c_int), ctypes.c_int])
cuDeviceGetName = proto(resolve("cuDeviceGetName"),
                        [ctypes.c_char_p, ctypes.c_int, ctypes.c_int])
cuDeviceGetAttribute = proto(resolve("cuDeviceGetAttribute"),
                             [ctypes.POINTER(ctypes.c_int), ctypes.c_int,
                              ctypes.c_int])
cuDeviceTotalMem = proto(resolve("cuDeviceTotalMem_v2", "cuDeviceTotalMem"),
                         [ctypes.POINTER(ctypes.c_size_t), ctypes.c_int])
cuDeviceCanAccessPeer = proto(resolve("cuDeviceCanAccessPeer"),
                              [ctypes.POINTER(ctypes.c_int), ctypes.c_int,
                               ctypes.c_int])
cuDevicePrimaryCtxRetain = proto(resolve("cuDevicePrimaryCtxRetain"),
                                 [ctypes.POINTER(ctypes.c_void_p),
                                  ctypes.c_int])
cuDevicePrimaryCtxRelease = proto(
    resolve("cuDevicePrimaryCtxRelease_v2", "cuDevicePrimaryCtxRelease"),
    [ctypes.c_void_p])
cuCtxSetCurrent = proto(resolve("cuCtxSetCurrent"), [ctypes.c_void_p])
cuCtxSynchronize = proto(resolve("cuCtxSynchronize"), [])
cuCtxEnablePeerAccess = proto(resolve("cuCtxEnablePeerAccess"),
                              [ctypes.c_void_p, ctypes.c_uint])
cuMemAlloc = proto(resolve("cuMemAlloc_v2", "cuMemAlloc"),
                   [ctypes.POINTER(ctypes.c_uint64), ctypes.c_size_t])
cuMemFree = proto(resolve("cuMemFree_v2", "cuMemFree"), [ctypes.c_uint64])
cuMemsetD32 = proto(resolve("cuMemsetD32_v2", "cuMemsetD32"),
                    [ctypes.c_uint64, ctypes.c_uint, ctypes.c_size_t])
cuMemcpyDtoH = proto(resolve("cuMemcpyDtoH_v2", "cuMemcpyDtoH"),
                     [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_size_t])
cuMemcpyPeer = proto(resolve("cuMemcpyPeer"),
                     [ctypes.c_uint64, ctypes.c_void_p,
                      ctypes.c_uint64, ctypes.c_void_p, ctypes.c_size_t])
cuGetErrorName = proto(resolve("cuGetErrorName"),
                       [ctypes.c_int, ctypes.POINTER(ctypes.c_char_p)])


def err_name(rc):
    p = ctypes.c_char_p()
    if cuGetErrorName(rc, ctypes.byref(p)) == CUDA_SUCCESS and p.value:
        return p.value.decode()
    return str(rc)


def check(rc, what):
    if rc != CUDA_SUCCESS:
        raise RuntimeError("%s: %s (%d)" % (what, err_name(rc), rc))


def main():
    check(cuInit(0), "cuInit")

    count = ctypes.c_int()
    check(cuDeviceGetCount(ctypes.byref(count)), "cuDeviceGetCount")
    n = count.value
    print("CUDA devices: %d" % n)
    if n < 2:
        print("FAIL: need at least two CUDA devices")
        return 1

    devs, ctxs, ptrs, patterns, expected = [], [], [], [], []
    for i in range(n):
        d = ctypes.c_int()
        check(cuDeviceGet(ctypes.byref(d), i), "cuDeviceGet")
        name = ctypes.create_string_buffer(256)
        check(cuDeviceGetName(name, 256, d.value), "cuDeviceGetName")
        total = ctypes.c_size_t()
        check(cuDeviceTotalMem(ctypes.byref(total), d.value),
              "cuDeviceTotalMem")
        dom, bus, dev = ctypes.c_int(), ctypes.c_int(), ctypes.c_int()
        check(cuDeviceGetAttribute(ctypes.byref(dom),
                                   CU_DEVICE_ATTRIBUTE_PCI_DOMAIN_ID,
                                   d.value), "attr domain")
        check(cuDeviceGetAttribute(ctypes.byref(bus),
                                   CU_DEVICE_ATTRIBUTE_PCI_BUS_ID,
                                   d.value), "attr bus")
        check(cuDeviceGetAttribute(ctypes.byref(dev),
                                   CU_DEVICE_ATTRIBUTE_PCI_DEVICE_ID,
                                   d.value), "attr dev")
        print("  dev %d: %-24s %04x:%02x:%02x.0  %d MiB" %
              (i, name.value.decode(), dom.value, bus.value, dev.value,
               total.value >> 20))
        devs.append(d.value)

        ctx = ctypes.c_void_p()
        check(cuDevicePrimaryCtxRetain(ctypes.byref(ctx), d.value),
              "cuDevicePrimaryCtxRetain")
        ctxs.append(ctx)

        check(cuCtxSetCurrent(ctx), "cuCtxSetCurrent")
        p = ctypes.c_uint64()
        check(cuMemAlloc(ctypes.byref(p), N), "cuMemAlloc")
        pat = 0xA0000000 + i
        check(cuMemsetD32(p, pat, N // 4), "cuMemsetD32")
        check(cuCtxSynchronize(), "cuCtxSynchronize")
        ptrs.append(p.value)
        patterns.append(pat)
        expected.append(struct.pack("<I", pat) * (N // 4))

    ok = True
    host = ctypes.create_string_buffer(N)

    def read_back(dev_i):
        check(cuCtxSetCurrent(ctxs[dev_i]), "cuCtxSetCurrent")
        check(cuMemcpyDtoH(host, ptrs[dev_i], N), "cuMemcpyDtoH")
        return host.raw

    def first_diff(buf, exp):
        if buf == exp:
            return None
        for k in range(0, len(exp), 4):
            if buf[k:k + 4] != exp[k:k + 4]:
                return (k // 4, int.from_bytes(buf[k:k + 4], "little"),
                        int.from_bytes(exp[k:k + 4], "little"))
        return None

    print("self-check (each device holds its own pattern):")
    for i in range(n):
        d = first_diff(read_back(i), expected[i])
        if d is None:
            print("  dev %d: OK" % i)
        else:
            print("  dev %d: MISMATCH word=%d got=%#x want=%#x" % (i, *d))
            ok = False

    for i in range(n):
        for j in range(n):
            if i == j:
                continue
            print("pair %d -> %d:" % (i, j))

            # Restore both patterns: earlier copies overwrite destination buffers.
            check(cuCtxSetCurrent(ctxs[i]), "cuCtxSetCurrent")
            check(cuMemsetD32(ptrs[i], patterns[i], N // 4), "fill src")
            check(cuCtxSetCurrent(ctxs[j]), "cuCtxSetCurrent")
            check(cuMemsetD32(ptrs[j], patterns[j], N // 4), "fill dst")
            check(cuCtxSynchronize(), "cuCtxSynchronize")

            can = ctypes.c_int()
            rc = cuDeviceCanAccessPeer(ctypes.byref(can), i, j)
            if rc != CUDA_SUCCESS:
                print("  canAccess: error %s" % err_name(rc))
                ok = False
                continue
            if not can.value:
                print("  canAccess: 0 (driver says no P2P)")
                ok = False
                continue

            check(cuCtxSetCurrent(ctxs[i]), "cuCtxSetCurrent")
            rc = cuCtxEnablePeerAccess(ctxs[j], 0)
            if rc not in (CUDA_SUCCESS, CUDA_ERROR_PEER_ACCESS_ALREADY_ENABLED):
                print("  enablePeerAccess: %s" % err_name(rc))
                ok = False
                continue

            rc = cuMemcpyPeer(ptrs[j], ctxs[j], ptrs[i], ctxs[i], N)
            if rc != CUDA_SUCCESS:
                print("  cuMemcpyPeer: %s" % err_name(rc))
                ok = False
                continue
            check(cuCtxSynchronize(), "cuCtxSynchronize")

            dd = first_diff(read_back(j), expected[i])
            ds = first_diff(read_back(i), expected[i])
            if dd is not None:
                print("  DEST MISMATCH word=%d got=%#x want=%#x" % dd)
                ok = False
                continue
            if ds is not None:
                print("  SOURCE CHANGED word=%d got=%#x want=%#x" % ds)
                ok = False
                continue
            print("  PASS  %d MiB copied and verified" % (N >> 20))

    for i in range(n):
        cuCtxSetCurrent(ctxs[i])
        cuMemFree(ptrs[i])
        cuDevicePrimaryCtxRelease(ctxs[i])

    print("RESULT: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
