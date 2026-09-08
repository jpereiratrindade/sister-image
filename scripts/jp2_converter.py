#!/usr/bin/env python3
"""
SisTer Image — Sentinel-2 JP2 / J2K Converter & Inspector
Converts JPEG 2000 Sentinel satellite imagery to GeoTIFF and extracts spatial metadata.
"""
import json
import math
import os
import re
import sys
from pathlib import Path
from PIL import Image, ImageFile, TiffImagePlugin
import numpy as np

ImageFile.LOAD_TRUNCATED_IMAGES = True
Image.MAX_IMAGE_PIXELS = None

def utm_to_latlon(easting, northing, zone=22, southern=True):
    a = 6378137.0
    f = 1 / 298.257223563
    b = a * (1 - f)
    e = math.sqrt(1 - (b / a) ** 2)
    e_prime_sq = (e ** 2) / (1 - e ** 2)
    k0 = 0.9996

    x = easting - 500000.0
    y = northing
    if southern:
        y -= 10000000.0

    m = y / k0
    mu = m / (a * (1 - e**2/4 - 3*e**4/64 - 5*e**6/256))

    e1 = (1 - math.sqrt(1 - e**2)) / (1 + math.sqrt(1 - e**2))

    j1 = (3*e1/2 - 27*e1**3/32)
    j2 = (21*e1**2/16 - 55*e1**4/32)
    j3 = (151*e1**3/96)
    j4 = (1097*e1**4/512)

    fp = mu + j1*math.sin(2*mu) + j2*math.sin(4*mu) + j3*math.sin(6*mu) + j4*math.sin(8*mu)

    c1 = e_prime_sq * (math.cos(fp))**2
    t1 = (math.tan(fp))**2
    r1 = a * (1 - e**2) / (1 - e**2 * (math.sin(fp))**2)**1.5
    n1 = a / math.sqrt(1 - e**2 * (math.sin(fp))**2)
    d = x / (n1 * k0)

    fact1 = n1 * math.tan(fp) / r1
    fact2 = d**2 / 2
    fact3 = (5 + 3*t1 + 10*c1 - 4*c1**2 - 9*e_prime_sq) * d**4 / 24
    fact4 = (61 + 90*t1 + 298*c1 + 45*t1**2 - 252*e_prime_sq - 3*c1**2) * d**6 / 720

    lat = fp - fact1 * (fact2 - fact3 + fact4)

    fact5 = d
    fact6 = (1 + 2*t1 + c1) * d**3 / 6
    fact7 = (5 - 2*c1 + 28*t1 - 3*c1**2 + 8*e_prime_sq + 24*t1**2) * d**5 / 120

    lon_diff = (fact5 - fact6 + fact7) / math.cos(fp)

    central_meridian = (zone - 1) * 6 - 180 + 3
    lon = central_meridian + math.degrees(lon_diff)
    lat = math.degrees(lat)

    return lat, lon

def parse_gmljp2_box(path_str):
    try:
        with open(path_str, 'rb') as f:
            data = f.read(500000)

        pos = data.find(b'<gml:RectifiedGrid')
        if pos == -1:
            pos = data.find(b'<gml:FeatureCollection')
        if pos == -1:
            return None

        end = data.find(b'</gml:FeatureCollection>', pos)
        if end == -1:
            end = data.find(b'</gml:RectifiedGridCoverage>', pos)
        if end == -1:
            end = pos + 10000

        snippet = data[pos:end+50].decode('utf-8', errors='ignore')

        epsg = 32722
        m_epsg = re.search(r'srsName=[\"\']urn:ogc:def:crs:EPSG::(\d+)[\"\']', snippet)
        if m_epsg:
            epsg = int(m_epsg.group(1))

        m_pos = re.search(r'<gml:pos>\s*([\d\.-]+)\s+([\d\.-]+)\s*</gml:pos>', snippet)
        tie_x, tie_y = 0.0, 0.0
        if m_pos:
            tie_x, tie_y = float(m_pos.group(1)), float(m_pos.group(2))

        offsets = re.findall(r'<gml:offsetVector[^>]*>\s*([\d\.-]+)\s+([\d\.-]+)\s*</gml:offsetVector>', snippet)
        scale_x, scale_y = 10.0, 10.0
        if offsets:
            try:
                scale_x = abs(float(offsets[0][0]))
                scale_y = abs(float(offsets[1][1]))
            except Exception:
                pass

        zone = 22
        southern = True
        if 32601 <= epsg <= 32660:
            zone = epsg - 32600
            southern = False
        elif 32701 <= epsg <= 32760:
            zone = epsg - 32700
            southern = True

        return {
            'epsg': epsg,
            'tie_x': tie_x,
            'tie_y': tie_y,
            'scale_x': scale_x,
            'scale_y': scale_y,
            'crs': f"EPSG:{epsg} (WGS 84 / UTM zone {zone}{'S' if southern else 'N'})",
            'zone': zone,
            'southern': southern
        }
    except Exception:
        return None

def extract_spatial_tags(img, path_str=""):
    gml = parse_gmljp2_box(path_str)
    if gml and gml['tie_x'] > 0:
        return gml['scale_x'], gml['scale_y'], gml['tie_x'], gml['tie_y'], gml['crs'], gml['zone'], gml['southern']

    scale_x, scale_y = 10.0, 10.0
    tie_x, tie_y = 0.0, 0.0
    crs = "WGS 84 / UTM zone 22S"
    zone = 22
    southern = True

    if hasattr(img, 'tag_v2'):
        tags = img.tag_v2
        if 33550 in tags:
            s = tags[33550]
            scale_x, scale_y = float(s[0]), float(s[1])
        if 33922 in tags:
            t = tags[33922]
            tie_x, tie_y = float(t[3]), float(t[4])
        if 34737 in tags:
            crs = str(tags[34737]).replace('\x00', '')

    import re
    m = re.search(r'zone\s*(\d+)([NSns]?)', crs, re.IGNORECASE)
    if not m:
        m = re.search(r'T(\d{2})[A-Z]{3}', path_str)
    if m:
        zone = int(m.group(1))
        if m.lastindex >= 2 and m.group(2):
            southern = (m.group(2).upper() == 'S')

    return scale_x, scale_y, tie_x, tie_y, crs, zone, southern

def inspect_jp2(path):
    img = Image.open(path)
    width, height = img.size
    mode = img.mode
    channels = len(img.getbands()) if hasattr(img, 'getbands') else 1
    bps = 16 if ('16' in mode or mode.startswith('I') or mode.startswith('F')) else 8

    scale_x, scale_y, tie_x, tie_y, crs, zone, southern = extract_spatial_tags(img, str(path))

    min_x = tie_x
    max_y = tie_y
    max_x = tie_x + width * scale_x
    min_y = tie_y - height * scale_y

    latlon_bounds = None
    if tie_x > 1000 and tie_y > 1000:
        try:
            lat1, lon1 = utm_to_latlon(min_x, max_y, zone=zone, southern=southern)
            lat2, lon2 = utm_to_latlon(max_x, min_y, zone=zone, southern=southern)
            latlon_bounds = [
                min(lat1, lat2), min(lon1, lon2),
                max(lat1, lat2), max(lon1, lon2)
            ]
        except Exception:
            pass

    area_sq_m = (width * scale_x) * (height * scale_y)
    area_sq_km = area_sq_m / 1000000.0
    area_ha = area_sq_km * 100.0

    return {
        "width": width,
        "height": height,
        "channels": channels,
        "bits_per_sample": bps,
        "photometric": 1,
        "has_geotiff_tags": True,
        "scale_x": scale_x,
        "scale_y": scale_y,
        "tie_x": tie_x,
        "tie_y": tie_y,
        "pixel_size_meters": round(scale_x, 3),
        "crs": crs,
        "spatial_extent_utm": [min_x, min_y, max_x, max_y],
        "latlon_bounds": latlon_bounds,
        "area_sq_km": round(area_sq_km, 4),
        "area_ha": round(area_ha, 2),
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

    tiffinfo = TiffImagePlugin.ImageFileDirectory_v2()
    gml_meta = parse_gmljp2_box(input_path)
    if gml_meta and gml_meta['tie_x'] > 0:
        tiffinfo[33550] = (gml_meta['scale_x'], gml_meta['scale_y'], 0.0)
        tiffinfo[33922] = (0.0, 0.0, 0.0, gml_meta['tie_x'], gml_meta['tie_y'], 0.0)
        tiffinfo[34737] = gml_meta['crs']
    elif hasattr(img, 'tag_v2'):
        try:
            for tag_id in [33550, 33922, 34735, 34737]:
                if tag_id in img.tag_v2:
                    tiffinfo[tag_id] = img.tag_v2[tag_id]
        except Exception:
            pass

    out_img.save(output_path, format='TIFF', tiffinfo=tiffinfo)
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
