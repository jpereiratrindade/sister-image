'use strict';

const $ = id => document.getElementById(id);
let currentJob = null;
let currentGeoJSON = null;
let currentRasterInfo = null;
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

// Draw vector polygon outlines and point markers on canvas
function drawVectorOverlay(ctx, width, height, geojson) {
  if (!geojson || !geojson.features || !geojson.features.length) return;
  const bbox = geojson.bbox || [0, 0, width, height];
  const minX = bbox[0], minY = bbox[1], maxX = bbox[2], maxY = bbox[3];
  const diffX = maxX - minX;
  const diffY = maxY - minY;
  const rangeX = diffX > 1e-9 ? diffX : 1;
  const rangeY = diffY > 1e-9 ? diffY : 1;
  const pad = 40;
  const availW = Math.max(10, width - pad * 2);
  const availH = Math.max(10, height - pad * 2);

  ctx.strokeStyle = '#06b6d4';
  ctx.fillStyle = 'rgba(6, 182, 212, 0.25)';
  ctx.lineWidth = 2;

  const mapX = x => diffX > 1e-9 ? (x - minX) / rangeX * availW + pad : width / 2;
  const mapY = y => diffY > 1e-9 ? (maxY - y) / rangeY * availH + pad : height / 2;

  for (const feat of geojson.features) {
    if (!feat.geometry) continue;
    const gtype = feat.geometry.type;

    if (gtype === 'Polygon') {
      for (const ring of feat.geometry.coordinates) {
        ctx.beginPath();
        for (let i = 0; i < ring.length; i++) {
          const px = mapX(ring[i][0]);
          const py = mapY(ring[i][1]);
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
        const px = mapX(feat.geometry.coordinates[i][0]);
        const py = mapY(feat.geometry.coordinates[i][1]);
        if (i === 0) ctx.moveTo(px, py);
        else ctx.lineTo(px, py);
      }
      ctx.stroke();
    } else if (gtype === 'Point') {
      const px = mapX(feat.geometry.coordinates[0]);
      const py = mapY(feat.geometry.coordinates[1]);

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

function renderLeafletMap(geojson) {
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

  setTimeout(() => {
    if (leafletInstance) {
      leafletInstance.invalidateSize();
      if (leafletGeoJsonLayer) {
        try {
          const bounds = leafletGeoJsonLayer.getBounds();
          if (bounds && bounds.isValid()) {
            leafletInstance.fitBounds(bounds, { padding: [40, 40] });
          }
        } catch (e) {}
      }
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
    if ($('r-scale')) $('r-scale').textContent = `${currentRasterInfo.scale_x}, ${currentRasterInfo.scale_y}`;
    if ($('r-tie')) $('r-tie').textContent = `${currentRasterInfo.tie_x}, ${currentRasterInfo.tie_y}`;

    if ($('raster-info-card')) $('raster-info-card').hidden = false;
    if ($('clip-raster-name')) $('clip-raster-name').textContent = file.name;

    statusMessage(`Raster "${file.name}" inspecionado. ${currentRasterInfo.width} × ${currentRasterInfo.height} px, ${currentRasterInfo.channels} canais.`);
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

async function submitClassification(demo = false) {
  if (!demo && $('classify-form') && !$('classify-form').reportValidity()) return;
  const file = $('classify-file') ? $('classify-file').files[0] : null;
  if (!demo && !file) { statusMessage('Escolha uma imagem TIFF ou Sentinel JP2 para classificar.', true); return; }

  setWorking(true);
  if ($('result-bar')) $('result-bar').hidden = true;
  if ($('viewport-canvas')) $('viewport-canvas').hidden = true;
  if ($('viewport-empty')) $('viewport-empty').hidden = false;
  statusMessage(demo ? 'Executando demonstração sintética…' : `Enviando imagem ${file ? file.name : ''}…`);
  if ($('state-badge')) $('state-badge').textContent = 'Enviando';

  try {
    const nameParam = file ? `&name=${encodeURIComponent(file.name)}` : '';
    const headers = demo ? {} : { 'X-File-Name': file.name, 'Content-Type': 'application/octet-stream' };
    const response = await api(`${demo ? 'api/demo' : 'api/classify'}?${getClassifyParams()}${nameParam}`, {
      method: 'POST',
      headers,
      body: demo ? null : file
    });
    await showJobResults(await response.json());
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
if ($('raster-file')) $('raster-file').onchange = () => handleRasterInspect($('raster-file').files[0]);

if ($('classify-file')) $('classify-file').onchange = () => {
  if ($('classify-filename')) $('classify-filename').textContent = $('classify-file').files[0]?.name || 'Escolha a Imagem TIFF';
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

refreshJobs().catch(e => statusMessage(e.message, true));
