'use strict';

const $ = id => document.getElementById(id);
let currentJob = null;
let currentGeoJSON = null;
let currentRasterInfo = null;
let currentClippedInfo = null;
let currentViewMode = 'map';
let polling = false;

const states = { uploading: 'Enviando', running: 'Processando', completed: 'Concluída', failed: 'Falhou' };

function statusMessage(text, failure = false) {
  const bar = $('status-bar');
  if (bar) {
    bar.textContent = text;
    bar.className = 'status-bar' + (failure ? ' error' : '');
  }
}

async function api(path, options = {}) {
  const response = await fetch(path, options);
  if (!response.ok) {
    let text = 'Não foi possível concluir a solicitação.';
    try { text = (await response.json()).message || text; } catch {}
    throw Error(text);
  }
  return response;
}

function getClassifyParams() {
  const p = new URLSearchParams();
  for (const id of ['mode', 'window', 'classes', 'homogeneity', 'min_valid', 'nodata']) {
    if ($(id)) p.set(id, $(id).value);
  }
  if ($('ignore_zero')) p.set('ignore_zero', $('ignore_zero').checked);
  return p;
}

function setWorking(value) {
  if ($('submit-classify')) $('submit-classify').disabled = value;
  if ($('demo-btn')) $('demo-btn').disabled = value;
  if ($('task-progress')) $('task-progress').hidden = !value;
}

function latlonToUtm(lat, lon, zone = 22, southern = true) {
  const a = 6378137.0;
  const f = 1 / 298.257223563;
  const k0 = 0.9996;

  const latRad = lat * Math.PI / 180;
  const lonRad = lon * Math.PI / 180;

  const centralMeridian = (zone - 1) * 6 - 180 + 3;
  const lon0Rad = centralMeridian * Math.PI / 180;

  const eSq = f * (2 - f);
  const ePrimeSq = eSq / (1 - eSq);

  const sinLat = Math.sin(latRad);
  const cosLat = Math.cos(latRad);
  const tanLat = Math.tan(latRad);

  const N = a / Math.sqrt(1 - eSq * sinLat ** 2);
  const T = tanLat ** 2;
  const C = ePrimeSq * cosLat ** 2;
  const A = (lonRad - lon0Rad) * cosLat;

  const M = a * ((1 - eSq / 4 - 3 * eSq ** 2 / 64 - 5 * eSq ** 3 / 256) * latRad
    - (3 * eSq / 8 + 3 * eSq ** 2 / 32 + 45 * eSq ** 3 / 1024) * Math.sin(2 * latRad)
    + (15 * eSq ** 2 / 256 + 45 * eSq ** 3 / 1024) * Math.sin(4 * latRad)
    - (35 * eSq ** 3 / 3072) * Math.sin(6 * latRad));

  let easting = k0 * N * (A + (1 - T + C) * A ** 3 / 6 + (5 - 18 * T + T ** 2 + 72 * C - 58 * ePrimeSq) * A ** 5 / 120) + 500000.0;
  let northing = k0 * (M + N * tanLat * (A ** 2 / 2 + (5 - T + 9 * C + 4 * C ** 2) * A ** 4 / 24 + (61 - 58 * T + T ** 2 + 600 * C - 330 * ePrimeSq) * A ** 6 / 720));

  if (southern && northing < 0) {
    northing += 10000000.0;
  }

  return { easting, northing };
}

function utmToLatLon(easting, northing, zone = 22, southern = true) {
  const a = 6378137.0;
  const f = 1 / 298.257223563;
  const b = a * (1 - f);
  const e = Math.sqrt(1 - (b / a) ** 2);
  const ePrimeSq = (e ** 2) / (1 - e ** 2);
  const k0 = 0.9996;

  let x = easting - 500000.0;
  let y = northing;
  if (southern) {
    y -= 10000000.0;
  }

  const m = y / k0;
  const mu = m / (a * (1 - e ** 2 / 4 - 3 * e ** 4 / 64 - 5 * e ** 6 / 256));

  const e1 = (1 - Math.sqrt(1 - e ** 2)) / (1 + Math.sqrt(1 - e ** 2));

  const j1 = (3 * e1 / 2 - 27 * e1 ** 3 / 32);
  const j2 = (21 * e1 ** 2 / 16 - 55 * e1 ** 4 / 32);
  const j3 = (151 * e1 ** 3 / 96);
  const j4 = (1097 * e1 ** 4 / 512);

  const fp = mu + j1 * Math.sin(2 * mu) + j2 * Math.sin(4 * mu) + j3 * Math.sin(6 * mu) + j4 * Math.sin(8 * mu);

  const c1 = ePrimeSq * Math.cos(fp) ** 2;
  const t1 = Math.tan(fp) ** 2;
  const r1 = a * (1 - e ** 2) / (1 - e ** 2 * Math.sin(fp) ** 2) ** 1.5;
  const n1 = a / Math.sqrt(1 - e ** 2 * Math.sin(fp) ** 2);
  const d = x / (n1 * k0);

  const fact1 = n1 * Math.tan(fp) / r1;
  const fact2 = d ** 2 / 2;
  const fact3 = (5 + 3 * t1 + 10 * c1 - 4 * c1 ** 2 - 9 * ePrimeSq) * d ** 4 / 24;
  const fact4 = (61 + 90 * t1 + 298 * c1 + 45 * t1 ** 2 - 252 * ePrimeSq - 3 * c1 ** 2) * d ** 6 / 720;

  const latRad = fp - fact1 * (fact2 - fact3 + fact4);

  const fact5 = d;
  const fact6 = (1 + 2 * t1 + c1) * d ** 3 / 6;
  const fact7 = (5 - 2 * c1 + 28 * t1 - 3 * c1 ** 2 + 8 * ePrimeSq + 24 * t1 ** 2) * d ** 5 / 120;

  const lonDiff = (fact5 - fact6 + fact7) / Math.cos(fp);

  const centralMeridian = (zone - 1) * 6 - 180 + 3;
  const lon = centralMeridian + (lonDiff * 180 / Math.PI);
  const lat = latRad * 180 / Math.PI;

  return { lat, lon };
}

function updateWindowSpatialScale() {
  const winEl = $('window');
  const scaleEl = $('window-spatial-scale');
  if (!winEl || !scaleEl) return;

  const winPx = parseInt(winEl.value, 10) || 128;
  const pixelMeters = currentRasterInfo ? (currentRasterInfo.pixel_size_meters || currentRasterInfo.scale_x || 10.0) : 10.0;

  const totalMeters = winPx * pixelMeters;
  const totalKm = totalMeters / 1000.0;
  const areaSqM = totalMeters * totalMeters;
  const areaHa = areaSqM / 10000.0;

  scaleEl.textContent = `Escala: ${winPx} px = ${totalMeters.toLocaleString()} m (${totalKm.toFixed(2)} km) × ${totalMeters.toLocaleString()} m | Área da Janela: ${areaHa.toFixed(2)} ha (Res. ${pixelMeters}m/px)`;
}

// Draw vector polygon outlines and point markers on canvas
function drawVectorOverlay(ctx, width, height, geojson) {
  if (!geojson || !geojson.features || !geojson.features.length) return;

  const bbox = geojson.bbox || [0, 0, width, height];
  const isLatLon = (bbox[0] >= -180 && bbox[2] <= 180 && bbox[1] >= -90 && bbox[3] <= 90);
  let mapX, mapY;

  if (isLatLon && currentRasterInfo && currentRasterInfo.tie_x > 0 && currentRasterInfo.scale_x > 0) {
    const tieX = currentRasterInfo.tie_x;
    const tieY = currentRasterInfo.tie_y;
    const scaleX = currentRasterInfo.scale_x;
    const scaleY = currentRasterInfo.scale_y;
    const zone = currentRasterInfo.zone || 22;

    mapX = lonLat => {
      const lon = lonLat[0], lat = lonLat[1];
      const utm = latlonToUtm(lat, lon, zone, true);
      const pxX = (utm.easting - tieX) / scaleX;
      return (pxX / currentRasterInfo.width) * width;
    };

    mapY = lonLat => {
      const lon = lonLat[0], lat = lonLat[1];
      const utm = latlonToUtm(lat, lon, zone, true);
      const pxY = (tieY - utm.northing) / scaleY;
      return (pxY / currentRasterInfo.height) * height;
    };
  } else {
    const minX = bbox[0], minY = bbox[1], maxX = bbox[2], maxY = bbox[3];
    const diffX = maxX - minX > 1e-9 ? maxX - minX : 1;
    const diffY = maxY - minY > 1e-9 ? maxY - minY : 1;
    const pad = 40;
    const availW = Math.max(10, width - pad * 2);
    const availH = Math.max(10, height - pad * 2);

    mapX = lonLat => (lonLat[0] - minX) / diffX * availW + pad;
    mapY = lonLat => (maxY - lonLat[1]) / diffY * availH + pad;
  }

  ctx.strokeStyle = '#06b6d4';
  ctx.fillStyle = 'rgba(6, 182, 212, 0.25)';
  ctx.lineWidth = 2;

  for (const feat of geojson.features) {
    if (!feat.geometry) continue;
    const gtype = feat.geometry.type;

    if (gtype === 'Polygon') {
      for (const ring of feat.geometry.coordinates) {
        ctx.beginPath();
        for (let i = 0; i < ring.length; i++) {
          const px = mapX(ring[i]);
          const py = mapY(ring[i]);
          if (i === 0) ctx.moveTo(px, py);
          else ctx.lineTo(px, py);
        }
        ctx.closePath();
        ctx.stroke();
        ctx.fill();
      }
    } else if (gtype === 'LineString') {
      ctx.beginPath();
      for (let i = 0; i < feat.geometry.coordinates.length; i++) {
        const px = mapX(feat.geometry.coordinates[i]);
        const py = mapY(feat.geometry.coordinates[i]);
        if (i === 0) ctx.moveTo(px, py);
        else ctx.lineTo(px, py);
      }
      ctx.stroke();
    } else if (gtype === 'Point') {
      const px = mapX(feat.geometry.coordinates);
      const py = mapY(feat.geometry.coordinates);

      ctx.save();
      ctx.beginPath();
      ctx.arc(px, py, 7, 0, 2 * Math.PI);
      ctx.fillStyle = '#06b6d4';
      ctx.shadowColor = '#06b6d4';
      ctx.shadowBlur = 12;
      ctx.fill();
      ctx.strokeStyle = '#e0f2fe';
      ctx.lineWidth = 2;
      ctx.stroke();
      ctx.restore();
    }
  }
}

// Render raw PGM image preview to canvas
async function renderPGM(path) {
  const response = await api(path);
  const bytes = new Uint8Array(await response.arrayBuffer());
  let end = 0, lines = 0;
  while (end < bytes.length && lines < 3) {
    if (bytes[end++] === 10) lines++;
  }
  const header = new TextDecoder().decode(bytes.slice(0, end)).trim().split(/\s+/);
  const width = Number(header[1]), height = Number(header[2]);
  if (header[0] !== 'P5' || width > 2048 || height > 2048 || bytes.length - end !== width * height) {
    throw Error('Prévia PGM inválida.');
  }

  const canvas = $('viewport-canvas');
  canvas.width = width;
  canvas.height = height;
  const ctx = canvas.getContext('2d');
  const image = ctx.createImageData(width, height);
  for (let i = 0; i < width * height; i++) {
    const c = bytes[end + i];
    image.data.set([c, c, c, 255], i * 4);
  }
  ctx.putImageData(image, 0, 0);
  canvas.hidden = false;
  if ($('viewport-empty')) $('viewport-empty').hidden = true;

  if (currentViewMode === 'vector' && currentGeoJSON) {
    drawVectorOverlay(ctx, width, height, currentGeoJSON);
  }
}

// Render vector shape independently on blank canvas when no image is loaded
function renderStandaloneVector(geojson) {
  const width = 800, height = 600;
  const canvas = $('viewport-canvas');
  canvas.width = width;
  canvas.height = height;
  const ctx = canvas.getContext('2d');

  ctx.fillStyle = '#0b1329';
  ctx.fillRect(0, 0, width, height);

  drawVectorOverlay(ctx, width, height, geojson);
  canvas.hidden = false;
  if ($('viewport-empty')) $('viewport-empty').hidden = true;
}

let leafletInstance = null;
let leafletGeoJsonLayer = null;
let leafletRasterOverlay = null;

async function renderLeafletMap(geojson) {
  const container = $('leaflet-map');
  const canvas = $('viewport-canvas');
  if (!container) return;

  if (canvas) canvas.hidden = true;
  container.hidden = false;
  if ($('viewport-empty')) $('viewport-empty').hidden = true;

  if (typeof L === 'undefined') {
    statusMessage('Biblioteca Leaflet não disponível (modo offline). Usando 2D canvas.', true);
    if (canvas) canvas.hidden = false;
    container.hidden = true;
    renderStandaloneVector(geojson);
    return;
  }

  if (!leafletInstance) {
    const satellite = L.tileLayer('https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}', {
      maxZoom: 19,
      attribution: 'Tiles &copy; Esri &mdash; Source: Esri, i-cubed, USDA, USGS, AEX, GeoEye, Getmapping, Aerogrid, IGN, IGP, UPR-EGP, and GIS Community'
    });

    const osm = L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png', {
      maxZoom: 19,
      attribution: '&copy; <a href="https://www.openstreetmap.org/copyright">OpenStreetMap</a> contributors'
    });

    const dark = L.tileLayer('https://{s}.basemaps.cartocdn.com/dark_all/{z}/{x}/{y}{r}.png', {
      maxZoom: 19,
      attribution: '&copy; <a href="https://www.openstreetmap.org/copyright">OpenStreetMap</a> contributors &copy; <a href="https://carto.com/attributions">CARTO</a>'
    });

    leafletInstance = L.map('leaflet-map', {
      center: [-14.235, -51.925],
      zoom: 4,
      layers: [satellite]
    });

    const baseMaps = {
      "🌐 Imagem de Satélite (Esri)": satellite,
      "🗺️ Mapa Cartográfico (OpenStreetMap)": osm,
      "🌙 Tema Escuro (CartoDB Dark)": dark
    };

    L.control.layers(baseMaps, null, { position: 'topright' }).addTo(leafletInstance);

    const coordsControl = L.control({ position: 'bottomright' });
    coordsControl.onAdd = function() {
      const div = L.DomUtil.create('div', 'leaflet-coords-box');
      div.style.background = 'rgba(11, 19, 41, 0.9)';
      div.style.color = '#06b6d4';
      div.style.padding = '5px 12px';
      div.style.fontSize = '12px';
      div.style.fontFamily = 'monospace';
      div.style.borderRadius = '6px';
      div.style.border = '1px solid #26354a';
      div.innerHTML = 'Lat: - | Lon: -';
      return div;
    };
    coordsControl.addTo(leafletInstance);

    leafletInstance.on('mousemove', function(e) {
      const box = document.querySelector('.leaflet-coords-box');
      if (box) {
        box.textContent = `Lat: ${e.latlng.lat.toFixed(5)} | Lon: ${e.latlng.lng.toFixed(5)}`;
      }
    });
  }

  if (leafletGeoJsonLayer) {
    leafletInstance.removeLayer(leafletGeoJsonLayer);
    leafletGeoJsonLayer = null;
  }

  if (leafletRasterOverlay) {
    leafletInstance.removeLayer(leafletRasterOverlay);
    leafletRasterOverlay = null;
  }

  if (geojson && geojson.features && geojson.features.length) {
    leafletGeoJsonLayer = L.geoJSON(geojson, {
      style: { color: '#06b6d4', weight: 3.5, opacity: 0.95, fillColor: '#06b6d4', fillOpacity: 0.35 },
      pointToLayer: (feat, latlng) => L.circleMarker(latlng, {
        radius: 8, fillColor: '#06b6d4', color: '#ffffff', weight: 2, opacity: 1, fillOpacity: 0.9
      }),
      onEachFeature: (feat, layer) => {
        const type = feat.geometry ? feat.geometry.type : 'Feature';
        let popupText = `<strong>Geometria Vetorial SisTer</strong><br>Tipo: ${type}`;
        if (feat.geometry && feat.geometry.coordinates) {
          if (type === 'Point') {
            popupText += `<br>Coords: [${feat.geometry.coordinates[0].toFixed(5)}, ${feat.geometry.coordinates[1].toFixed(5)}]`;
          } else if (type === 'LineString') {
            popupText += `<br>Vértices: ${feat.geometry.coordinates.length}`;
          } else if (type === 'Polygon' && feat.geometry.coordinates[0]) {
            popupText += `<br>Vértices Anel Externo: ${feat.geometry.coordinates[0].length}`;
          }
        }
        layer.bindPopup(popupText);
      }
    }).addTo(leafletInstance);
  }

  if (currentJob && currentClippedInfo && currentClippedInfo.latlon_bounds) {
    try {
      await renderPGM(`api/jobs/${currentJob}/clipped_preview.pgm`);
      if (canvas) canvas.hidden = true;
    } catch (e) {}
  } else if (currentJob && (!canvas || canvas.width === 0 || canvas.hidden)) {
    try {
      await renderPGM(`api/jobs/${currentJob}/original_preview.pgm`);
      if (canvas) canvas.hidden = true;
    } catch (e) {}
  }

  let finalFitBounds = null;
  let rasterLatLngBounds = null;
  const activeInfo = (currentClippedInfo && currentClippedInfo.latlon_bounds) ? currentClippedInfo : currentRasterInfo;

  if (activeInfo && activeInfo.latlon_bounds) {
    const rb = activeInfo.latlon_bounds;
    rasterLatLngBounds = L.latLngBounds([[rb[0], rb[1]], [rb[2], rb[3]]]);
    finalFitBounds = rasterLatLngBounds;
  }

  if (currentJob && canvas && canvas.width > 0 && rasterLatLngBounds && rasterLatLngBounds.isValid && rasterLatLngBounds.isValid()) {
    try {
      const dataUrl = canvas.toDataURL('image/png');
      leafletRasterOverlay = L.imageOverlay(dataUrl, rasterLatLngBounds, { opacity: 0.85 }).addTo(leafletInstance);
    } catch (e) {}
  }

  if (leafletGeoJsonLayer) {
    try {
      const vecBounds = leafletGeoJsonLayer.getBounds();
      if (vecBounds && vecBounds.isValid()) {
        if (finalFitBounds && finalFitBounds.isValid && finalFitBounds.isValid()) {
          finalFitBounds.extend(vecBounds);
        } else {
          finalFitBounds = vecBounds;
        }
      }
    } catch (e) {}
  }

  if (activeInfo && geojson && rasterLatLngBounds && leafletGeoJsonLayer) {
    const rb = activeInfo.latlon_bounds;
    const vb = geojson.bbox || [0, 0, 0, 0];
    const overlap = !(rb[2] < vb[1] || rb[0] > vb[3] || rb[3] < vb[0] || rb[1] > vb[2]);
    if (overlap) {
      statusMessage(`🟢 Coincidência Espacial Verificada: Imagem (${activeInfo.crs || 'UTM'}) e Vetor sobrepostos em perfeito alinhamento.`);
    } else {
      statusMessage(`⚠️ Atenção: Os envelopes da imagem e do vetor não coincidem espacialmente na mesma região.`, true);
    }
  }

  setTimeout(() => {
    if (leafletInstance && finalFitBounds && finalFitBounds.isValid && finalFitBounds.isValid()) {
      leafletInstance.invalidateSize();
      try {
        leafletInstance.fitBounds(finalFitBounds, { padding: [40, 40] });
      } catch (e) {}
    }
  }, 100);
}

async function updateView(mode) {
  currentViewMode = mode;
  document.querySelectorAll('.v-btn').forEach(b => b.classList.remove('active'));
  if ($('v-' + mode)) $('v-' + mode).classList.add('active');

  const container = $('leaflet-map');
  const canvas = $('viewport-canvas');

  if (mode === 'leaflet') {
    renderLeafletMap(currentGeoJSON);
    statusMessage(currentGeoJSON ? 'Exibindo mapa cartográfico interativo Leaflet.' : 'Mapa Leaflet pronto. Carregue um vetor para sobreposição.');
    return;
  }

  if (container) container.hidden = true;

  if (mode === 'vector' && currentGeoJSON && !currentJob) {
    renderStandaloneVector(currentGeoJSON);
    statusMessage('Exibindo geometria do vetor carregado.');
    return;
  }

  if (!currentJob) return;

  try {
    if (mode === 'original') {
      await renderPGM(`api/jobs/${currentJob}/original_preview.pgm`);
      statusMessage('Exibindo prévia da imagem original.');
    } else if (mode === 'clipped') {
      await renderPGM(`api/jobs/${currentJob}/clipped_preview.pgm`);
      statusMessage('Exibindo prévia do GeoTIFF recortado.');
    } else {
      await renderPGM(`api/jobs/${currentJob}/preview.pgm`);
      statusMessage(mode === 'vector' ? 'Exibindo sobreposição de polígonos vetoriais.' : 'Exibindo mapa de regiões classificado.');
    }
  } catch (e) {
    statusMessage(e.message, true);
  }
}

// INHERENT VECTOR TOOL: Analyze SHP/KML/KMZ/GeoJSON
async function handleVectorUpload(file) {
  if (!file) return;
  if ($('vector-filename')) $('vector-filename').textContent = file.name;
  statusMessage('Analisando geometria vetorial…');

  try {
    const isJson = file.name.endsWith('.json') || file.name.endsWith('.geojson');
    const headers = isJson ? { 'Content-Type': 'application/json' } : { 'X-File-Name': file.name };
    const queryName = encodeURIComponent(file.name);
    const response = await api(`api/shapes/analyze?name=${queryName}`, { method: 'POST', headers, body: file });
    const data = await response.json();

    currentGeoJSON = data.geojson;
    const metrics = data.metrics;

    if ($('v-count-poly')) $('v-count-poly').textContent = metrics.polygons_count;
    if ($('v-count-pts')) $('v-count-pts').textContent = metrics.total_points;
    if ($('v-area')) $('v-area').textContent = metrics.approx_area.toFixed(4);
    if ($('v-perim')) $('v-perim').textContent = metrics.approx_perimeter.toFixed(4);
    if ($('v-bbox')) $('v-bbox').textContent = metrics.bbox.map(n => n.toFixed(3)).join(', ');

    if ($('vector-metrics-card')) $('vector-metrics-card').hidden = false;
    if ($('clip-vector-name')) $('clip-vector-name').textContent = file.name;
    if ($('exec-clip-btn')) $('exec-clip-btn').disabled = !(currentJob && currentGeoJSON);

    // Download GeoJSON button
    if ($('download-geojson-btn')) {
      $('download-geojson-btn').onclick = () => {
        const blob = new Blob([JSON.stringify(currentGeoJSON, null, 2)], { type: 'application/json' });
        const a = document.createElement('a');
        a.href = URL.createObjectURL(blob);
        a.download = (file.name.split('.')[0] || 'vector') + '.geojson';
        a.click();
      };
    }

    statusMessage(`Vetor "${file.name}" analisado. ${metrics.polygons_count} polígono(s), ${metrics.total_points} vértices.`);
    await updateView('leaflet');
  } catch (e) {
    statusMessage('Falha ao analisar arquivo vetorial: ' + e.message, true);
  }
}

// INHERENT RASTER TOOL: Inspect GeoTIFF / Sentinel JP2 Metadata
async function handleRasterInspect(file) {
  if (!file) return;
  if ($('raster-filename')) $('raster-filename').textContent = file.name;
  statusMessage('Inspecionando metadados da imagem raster…');

  try {
    const headers = { 'X-File-Name': file.name };
    const response = await api('api/raster/inspect', { method: 'POST', headers, body: file });
    const data = await response.json();
    currentRasterInfo = data.info;

    if ($('r-dim')) $('r-dim').textContent = `${currentRasterInfo.width} × ${currentRasterInfo.height}`;
    if ($('r-spp')) $('r-spp').textContent = currentRasterInfo.channels;
    if ($('r-bps')) $('r-bps').textContent = `${currentRasterInfo.bits_per_sample} bit`;
    if ($('r-geotiff')) $('r-geotiff').textContent = currentRasterInfo.has_geotiff_tags ? 'Sim (Georreferenciada)' : 'Não';
    if ($('r-scale')) $('r-scale').textContent = `${currentRasterInfo.scale_x}m × ${currentRasterInfo.scale_y}m`;
    if ($('r-tie')) $('r-tie').textContent = `${currentRasterInfo.tie_x}, ${currentRasterInfo.tie_y}`;
    if ($('r-pixel-size')) $('r-pixel-size').textContent = `${currentRasterInfo.pixel_size_meters || currentRasterInfo.scale_x} m/px`;
    if ($('r-area-ha')) $('r-area-ha').textContent = currentRasterInfo.area_ha ? `${currentRasterInfo.area_ha.toLocaleString()} ha` : '-';
    if ($('r-latlon')) {
      if (currentRasterInfo.latlon_bounds) {
        const b = currentRasterInfo.latlon_bounds;
        $('r-latlon').textContent = `[${b[0].toFixed(5)}, ${b[1].toFixed(5)}] a [${b[2].toFixed(5)}, ${b[3].toFixed(5)}]`;
      } else {
        $('r-latlon').textContent = 'Escala local (sem datum WGS84)';
      }
    }

    if ($('raster-info-card')) $('raster-info-card').hidden = false;
    if ($('clip-raster-name')) $('clip-raster-name').textContent = file.name;

    statusMessage(`Raster "${file.name}" inspecionado. ${currentRasterInfo.width} × ${currentRasterInfo.height} px (${currentRasterInfo.scale_x}m/px).`);
  } catch (e) {
    statusMessage('Falha ao inspecionar raster: ' + e.message, true);
  }
}

// SPATIAL CLIP TOOL: Crop GeoTIFF by Vector Mask
async function executeSpatialClip() {
  if (!currentJob || !currentGeoJSON) {
    statusMessage('Carregue uma imagem e um vetor para executar o recorte.', true);
    return;
  }

  statusMessage('Executando recorte espacial do GeoTIFF por máscara de polígono…');
  try {
    const response = await api(`api/jobs/${currentJob}/clip`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(currentGeoJSON)
    });
    const data = await response.json();

    if (data.result && data.result.tie_x > 0 && data.result.scale_x > 0) {
      const minX = data.result.tie_x;
      const maxY = data.result.tie_y;
      const maxX = minX + data.result.output_width * data.result.scale_x;
      const minY = maxY - data.result.output_height * data.result.scale_y;
      const zone = currentRasterInfo ? (currentRasterInfo.zone || 22) : 22;
      const southern = currentRasterInfo ? (currentRasterInfo.southern !== false) : true;

      const pt1 = utmToLatLon(minX, maxY, zone, southern);
      const pt2 = utmToLatLon(maxX, minY, zone, southern);
      currentClippedInfo = {
        ...data.result,
        latlon_bounds: [
          Math.min(pt1.lat, pt2.lat), Math.min(pt1.lon, pt2.lon),
          Math.max(pt1.lat, pt2.lat), Math.max(pt1.lon, pt2.lon)
        ]
      };
    }

    if ($('dl-clipped')) {
      $('dl-clipped').href = `api/jobs/${currentJob}/clipped.tif`;
      $('dl-clipped').hidden = false;
    }
    if ($('v-clipped')) $('v-clipped').hidden = false;

    await updateView('clipped');
    statusMessage(`Recorte concluído! Novo GeoTIFF: ${data.result.output_width} × ${data.result.output_height} px em ${data.result.elapsed_seconds.toFixed(2)}s`);
  } catch (e) {
    statusMessage('Falha no recorte espacial: ' + e.message, true);
  }
}

async function showJobResults(job) {
  currentJob = job.id;
  if (job.result && job.result.raster_info) {
    currentRasterInfo = job.result.raster_info;
  }
  if ($('state-badge')) $('state-badge').textContent = states[job.status] || job.status;

  if (job.status === 'failed') {
    setWorking(false);
    statusMessage(job.message || 'Falha na classificação da imagem.', true);
    return;
  }
  if (job.status !== 'completed') {
    setWorking(true);
    statusMessage('Classificação em andamento no servidor C++…');
    return;
  }

  setWorking(false);
  statusMessage('Classificação concluída. Alterne as visões acima para explorar os mapas.');
  await updateView('map');
  if ($('result-bar')) $('result-bar').hidden = false;

  const res = job.result;
  if ($('result-metrics')) {
    $('result-metrics').replaceChildren();
    for (const [val, lbl] of [
      [`${res.width.toLocaleString()} × ${res.height.toLocaleString()}`, 'Dimensões Originais'],
      [res.windows.toLocaleString(), 'Janelas Processadas'],
      [`${res.elapsed_seconds.toFixed(2)} s`, 'Tempo de Cálculo']
    ]) {
      const item = document.createElement('div');
      item.className = 'metric-item';
      item.innerHTML = `<span class="m-val">${val}</span><span class="m-lbl">${lbl}</span>`;
      $('result-metrics').append(item);
    }
  }

  if ($('dl-map')) $('dl-map').href = `api/jobs/${job.id}/map.tif`;
  if ($('dl-original')) $('dl-original').href = `api/jobs/${job.id}/original_preview.pgm`;
  if ($('dl-report')) $('dl-report').href = `api/jobs/${job.id}/report.json`;
  if ($('clip-raster-name')) $('clip-raster-name').textContent = `Job #${job.id.substring(0, 8)}`;
  if ($('exec-clip-btn')) $('exec-clip-btn').disabled = !currentGeoJSON;
}

async function refreshJobs() {
  const data = await (await api('api/jobs')).json();
  const jobs = data.jobs.sort((a, b) => b.created_at.localeCompare(a.created_at));

  if ($('jobs-list')) {
    $('jobs-list').replaceChildren();
    if (!jobs.length) {
      const p = document.createElement('p');
      p.className = 'help';
      p.textContent = 'Nenhuma execução registrada.';
      $('jobs-list').append(p);
    }
    for (const j of jobs) {
      const row = document.createElement('div');
      row.className = 'job-row';
      const openBtn = document.createElement('button');
      openBtn.textContent = `${new Date(j.created_at).toLocaleTimeString('pt-BR')} · ${states[j.status] || j.status}`;
      openBtn.onclick = () => showJobResults(j).catch(e => statusMessage(e.message, true));
      row.append(openBtn);

      if (['completed', 'failed'].includes(j.status)) {
        const delBtn = document.createElement('button');
        delBtn.className = 'del-btn';
        delBtn.textContent = 'Excluir';
        delBtn.onclick = async () => {
          if (!confirm('Excluir esta execução e seus arquivos?')) return;
          try {
            await api(`api/jobs/${j.id}`, { method: 'DELETE' });
            if (currentJob === j.id) {
              currentJob = null;
              if ($('result-bar')) $('result-bar').hidden = true;
              if ($('viewport-canvas')) $('viewport-canvas').hidden = true;
              if ($('viewport-empty')) $('viewport-empty').hidden = false;
              if ($('state-badge')) $('state-badge').textContent = 'Aguardando operação';
            }
            await refreshJobs();
          } catch (e) { statusMessage(e.message, true); }
        };
        row.append(delBtn);
      }
      $('jobs-list').append(row);
    }
  }

  setWorking(data.busy);
  const selected = jobs.find(j => j.id === currentJob) || (!currentJob && jobs.find(j => ['running', 'uploading'].includes(j.status)));
  if (selected && (selected.status !== 'completed' || ($('result-bar') && $('result-bar').hidden))) {
    await showJobResults(selected);
  }

  if (data.busy && !polling) {
    polling = true;
    setTimeout(async () => {
      polling = false;
      try { await refreshJobs(); } catch (e) { setWorking(false); statusMessage(e.message, true); }
    }, 1000);
  }
}

async function submitClassification(demo = false, customFile = null) {
  if (!demo && $('classify-form') && !$('classify-form').reportValidity()) return;
  const file = customFile || ($('classify-file') ? $('classify-file').files[0] : null);
  if (!demo && !file) { statusMessage('Escolha uma imagem TIFF ou Sentinel JP2 para classificar.', true); return; }

  setWorking(true);
  if ($('result-bar')) $('result-bar').hidden = true;
  if ($('viewport-canvas')) $('viewport-canvas').hidden = true;
  if ($('viewport-empty')) $('viewport-empty').hidden = false;
  statusMessage(demo ? 'Executando demonstração sintética…' : `Enviando e processando imagem ${file ? file.name : ''}…`);
  if ($('state-badge')) $('state-badge').textContent = 'Enviando';

  try {
    const nameParam = file ? `&name=${encodeURIComponent(file.name)}` : '';
    const headers = demo ? {} : { 'X-File-Name': file.name, 'Content-Type': 'application/octet-stream' };
    const response = await api(`${demo ? 'api/demo' : 'api/classify'}?${getClassifyParams()}${nameParam}`, {
      method: 'POST',
      headers,
      body: demo ? null : file
    });
    const jobData = await response.json();
    await showJobResults(jobData);
    await refreshJobs();
  } catch (e) { setWorking(false); statusMessage(e.message, true); }
}

// BINDINGS AND INITIALIZATION
document.querySelectorAll('.tool-btn').forEach(btn => {
  btn.onclick = () => {
    document.querySelectorAll('.tool-btn').forEach(b => b.classList.remove('active'));
    document.querySelectorAll('.tool-panel').forEach(p => p.classList.remove('active'));
    btn.classList.add('active');
    if ($(btn.dataset.tab)) $(btn.dataset.tab).classList.add('active');
  };
});

if ($('toggle-sidebar-btn')) {
  $('toggle-sidebar-btn').onclick = () => {
    if ($('control-dock')) $('control-dock').classList.toggle('collapsed');
  };
}

if ($('v-map')) $('v-map').onclick = () => updateView('map');
if ($('v-original')) $('v-original').onclick = () => updateView('original');
if ($('v-vector')) $('v-vector').onclick = () => updateView('vector');
if ($('v-leaflet')) $('v-leaflet').onclick = () => updateView('leaflet');
if ($('v-clipped')) $('v-clipped').onclick = () => updateView('clipped');

if ($('vector-file')) $('vector-file').onchange = () => handleVectorUpload($('vector-file').files[0]);
if ($('raster-file')) $('raster-file').onchange = () => {
  const file = $('raster-file').files[0];
  if (file) {
    handleRasterInspect(file);
    submitClassification(false, file);
  }
};

if ($('classify-file')) $('classify-file').onchange = () => {
  const file = $('classify-file').files[0];
  if (file) {
    if ($('classify-filename')) $('classify-filename').textContent = file.name;
    handleRasterInspect(file);
    submitClassification(false, file);
  }
};

if ($('classify-form')) $('classify-form').onsubmit = e => { e.preventDefault(); submitClassification(); };
if ($('demo-btn')) $('demo-btn').onclick = () => submitClassification(true);

if ($('exec-clip-btn')) $('exec-clip-btn').onclick = () => executeSpatialClip();

if ($('mode')) {
  $('mode').onchange = () => {
    const patches = $('mode').value === 'patches';
    if ($('classes-field')) $('classes-field').hidden = patches;
    if ($('homogeneity-field')) $('homogeneity-field').hidden = !patches;
  };
}

if ($('window')) {
  $('window').oninput = updateWindowSpatialScale;
  $('window').onchange = updateWindowSpatialScale;
}

refreshJobs().catch(e => statusMessage(e.message, true));
