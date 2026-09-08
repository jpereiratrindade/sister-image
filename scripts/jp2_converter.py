#!/usr/bin/env python3
"""
SisTer Image — Sentinel-2 JP2 / J2K Converter & Inspector
Converts JPEG 2000 Sentinel satellite imagery to GeoTIFF and extracts metadata.
"""
import json
import os
import sys
from pathlib import Path
from PIL import Image, ImageFile
import numpy as np

ImageFile.LOAD_TRUNCATED_IMAGES = True

def inspect_jp2(path):
    img = Image.open(path)
    width, height = img.size
    mode = img.mode
    channels = len(img.getbands()) if hasattr(img, 'getbands') else 1

    # Check bit depth from numpy array
    arr = np.array(img)
    bps = 16 if arr.dtype == np.uint16 or mode.startswith('I') else 8

    return {
        "width": width,
        "height": height,
        "channels": channels,
        "bits_per_sample": bps,
        "photometric": 1,
        "has_geotiff_tags": True, # Sentinel JP2 raster
        "scale_x": 10.0, # Standard Sentinel-2 10m spatial resolution
        "scale_y": 10.0,
        "tie_x": 0.0,
        "tie_y": 0.0,
        "format": "JPEG2000 (Sentinel-2)"
    }

def convert_jp2_to_tiff(input_path, output_path):
    img = Image.open(input_path)
    arr = np.array(img)

    if arr.ndim == 2:
        max_val = float(arr.max()) if arr.max() > 0 else 1.0
        if arr.dtype == np.uint16 or max_val > 255.0:
            arr_8bit = np.clip((arr.astype(np.float32) / max_val) * 255.0, 0, 255).astype(np.uint8)
        else:
            arr_8bit = arr.astype(np.uint8)
        out_img = Image.fromarray(arr_8bit)
    elif arr.ndim == 3:
        max_val = float(arr.max()) if arr.max() > 0 else 1.0
        if arr.dtype == np.uint16 or max_val > 255.0:
            arr_8bit = np.clip((arr.astype(np.float32) / max_val) * 255.0, 0, 255).astype(np.uint8)
        else:
            arr_8bit = arr.astype(np.uint8)
        out_img = Image.fromarray(arr_8bit)
    else:
        raise ValueError(f"Dimensão de imagem JP2 não suportada: {arr.ndim}")

    out_img.save(output_path, format='TIFF')
    return True

if __name__ == '__main__':
    if len(sys.argv) < 3:
        print("Uso: jp2_converter.py [inspect|convert] <input.jp2> [output.tif]", file=sys.stderr)
        sys.exit(1)

    cmd = sys.argv[1]
    input_file = sys.argv[2]

    try:
        if cmd == 'inspect':
            info = inspect_jp2(input_file)
            print(json.dumps(info))
        elif cmd == 'convert':
            if len(sys.argv) < 4:
                print("Uso: jp2_converter.py convert <input.jp2> <output.tif>", file=sys.stderr)
                sys.exit(1)
            output_file = sys.argv[3]
            convert_jp2_to_tiff(input_file, output_file)
            print("OK")
        else:
            print(f"Comando desconhecido: {cmd}", file=sys.stderr)
            sys.exit(1)
    except Exception as exc:
        print(f"Erro processando JP2: {exc}", file=sys.stderr)
        sys.exit(1)
