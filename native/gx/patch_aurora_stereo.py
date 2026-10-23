#!/usr/bin/env python3
"""Shared texture memory for the stereoscopic window (native/gx/present/stereo_bridge.cpp).

usage: patch_aurora_stereo.py SOURCE OUTPUT

SOURCE is Aurora's lib/webgpu/gpu.cpp. When the adapter supports them, the device is created with the
SharedTextureMemoryIOSurface and SharedFenceMTLSharedEvent features, which Dawn leaves off unless asked.
"""
import sys
from pathlib import Path


def fail(message):
    raise SystemExit(f'Aurora stereo patch: {message}')


def main():
    if len(sys.argv) != 3:
        fail('usage: patch_aurora_stereo.py SOURCE OUTPUT')
    source, output = map(Path, sys.argv[1:])
    text = source.read_text()
    anchor = '#ifdef TRACY_ENABLE\n      if (feature == wgpu::FeatureName::TimestampQuery) {\n'
    if text.count(anchor) != 1:
        fail('anchor mismatch: device features')
    text = text.replace(
        anchor,
        '#ifdef WEBGPU_DAWN\n'
        '      // Petari: the stereoscopic window shares IOSurfaces with the host app.\n'
        '      if (feature == wgpu::FeatureName::SharedTextureMemoryIOSurface ||\n'
        '          feature == wgpu::FeatureName::SharedFenceMTLSharedEvent) {\n'
        '        requiredFeatures.push_back(feature);\n'
        '      }\n'
        '#endif\n' + anchor)
    output.parent.mkdir(parents=True, exist_ok=True)
    if not output.exists() or output.read_text() != text:
        output.write_text(text)


if __name__ == '__main__':
    main()
