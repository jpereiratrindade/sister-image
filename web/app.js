'use strict';
const $ = id => document.getElementById(id);
let currentJob = null, currentGeoJSON = null, currentViewMode = 'map', polling = false;

const states = { uploading: 'Enviando', running: 'Processando', completed: 'Concluída', failed: 'Falhou' };

function message(text, failure = false) {
  $('message').textContent = text;
  $('message').className = failure ? 'error' : '';
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

function params() {
  const p = new URLSearchParams();
  for (const id of ['mode', 'window', 'classes', 'homogeneity', 'min_valid', 'nodata']) {
    p.set(id, $(id).value);
  }
  p.set('ignore_zero', $('ignore_zero').checked);
  return p;
}

function working(value) {
  $('submit').disabled = value;
  $('demo').disabled = value;
  $('progress').hidden = !value;
}

// Render PGM raw binary scanline data into HTML5 Canvas
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
    throw Error('Prévia de imagem inválida.');
  }

  const canvas = $('canvas');
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
  $('empty').hidden = true;

  // If overlay mode and vector shape exists, draw vector paths
  if (currentViewMode === 'overlay' && currentGeoJSON) {
    drawVectorOverlay(ctx, width, height, currentGeoJSON);
  }
}

function drawVectorOverlay(ctx, canvasWidth, canvasHeight, geojson) {
  if (!geojson || !geojson.features) return;
  const bbox = geojson.bbox || [0, 0, canvasWidth, canvasHeight];
  const minX = bbox[0], minY = bbox[1], maxX = bbox[2], maxY = bbox[3];
  const rangeX = maxX - minX || 1, rangeY = maxY - minY || 1;

  ctx.strokeStyle = '#06b6d4';
  ctx.fillStyle = 'rgba(6, 182, 212, 0.25)';
  ctx.lineWidth = 2;

  for (const feat of geojson.features) {
    if (feat.geometry && feat.geometry.type === 'Polygon') {
      for (const ring of feat.geometry.coordinates) {
        ctx.beginPath();
        for (let i = 0; i < ring.length; i++) {
          const pt = ring[i];
          const px = (pt[0] - minX) / rangeX * canvasWidth;
          const py = (maxY - pt[1]) / rangeY * canvasHeight;
          if (i === 0) ctx.moveTo(px, py);
          else ctx.lineTo(px, py);
        }
        ctx.closePath();
        ctx.stroke();
        ctx.fill();
      }
    }
  }
}

async function updateView(mode) {
  currentViewMode = mode;
  document.querySelectorAll('.view-btn').forEach(btn => btn.classList.remove('active'));
  if (mode === 'map') $('view-map').classList.add('active');
  if (mode === 'original') $('view-original').classList.add('active');
  if (mode === 'overlay') $('view-overlay').classList.add('active');
  if (mode === 'clipped') $('view-clipped').classList.add('active');

  if (!currentJob) return;
  try {
    if (mode === 'original') {
      await renderPGM(`api/jobs/${currentJob}/original_preview.pgm`);
      message('Exibindo prévia da imagem original.');
    } else if (mode === 'clipped') {
      await renderPGM(`api/jobs/${currentJob}/clipped_preview.pgm`);
      message('Exibindo prévia do GeoTIFF recortado.');
    } else {
      await renderPGM(`api/jobs/${currentJob}/preview.pgm`);
      message(mode === 'overlay' ? 'Exibindo sobreposição do vetor sobre o mapa.' : 'Exibindo mapa de regiões classificado.');
    }
  } catch (e) {
    message(e.message, true);
  }
}

async function show(job) {
  currentJob = job.id;
  $('state').textContent = states[job.status] || job.status;

  if (job.status === 'failed') {
    working(false);
    message(job.message || 'Não foi possível classificar esta imagem.', true);
    return;
  }
  if (job.status !== 'completed') {
    working(true);
    message('Classificação em andamento. Acompanhe nesta página.');
    return;
  }

  working(false);
  message('Processamento concluído. Visualize e baixe os resultados.');
  $('viewer-controls').hidden = false;
  await updateView('map');
  $('result').hidden = false;

  const result = job.result;
  $('metrics').replaceChildren();
  for (const [value, label] of [
    [`${result.width.toLocaleString()} × ${result.height.toLocaleString()}`, 'Dimensões Originais'],
    [result.windows.toLocaleString(), 'Janelas Classificadas'],
    [`${result.elapsed_seconds.toFixed(2)} s`, 'Tempo de Processamento']
  ]) {
    const box = document.createElement('div');
    box.className = 'metric';
    const strong = document.createElement('strong'), small = document.createElement('small');
    strong.textContent = value;
    small.textContent = label;
    box.append(strong, small);
    $('metrics').append(box);
  }

  $('map').href = `api/jobs/${job.id}/map.tif`;
  $('download-original').href = `api/jobs/${job.id}/original_preview.pgm`;
  $('report').href = `api/jobs/${job.id}/report.json`;
  if ($('clip-btn')) $('clip-btn').disabled = !currentGeoJSON;
}

async function refresh() {
  const data = await (await api('api/jobs')).json();
  const jobs = data.jobs.sort((a, b) => b.created_at.localeCompare(a.created_at));
  $('jobs').replaceChildren();

  if (!jobs.length) {
    const p = document.createElement('p');
    p.className = 'help';
    p.textContent = 'Nenhuma execução ainda.';
    $('jobs').append(p);
  }

  for (const job of jobs) {
    const row = document.createElement('div');
    row.className = 'job';
    const open = document.createElement('button');
    open.textContent = `${new Date(job.created_at).toLocaleString('pt-BR')} · ${states[job.status] || job.status}`;
    open.onclick = () => {
      $('result').hidden = true;
      $('canvas').hidden = true;
      $('empty').hidden = false;
      show(job).catch(e => message(e.message, true));
    };
    row.append(open);

    if (['completed', 'failed'].includes(job.status)) {
      const del = document.createElement('button');
      del.textContent = 'Excluir';
      del.setAttribute('aria-label', `Excluir execução ${job.id}`);
      del.onclick = async () => {
        if (!confirm('Excluir esta execução, sua imagem e os resultados?')) return;
        try {
          await api(`api/jobs/${job.id}`, { method: 'DELETE' });
          if (currentJob === job.id) {
            currentJob = null;
            $('result').hidden = true;
            $('canvas').hidden = true;
            $('empty').hidden = false;
            $('viewer-controls').hidden = true;
            $('state').textContent = 'Aguardando imagem';
            message('Execução excluída.');
          }
          await refresh();
        } catch (e) { message(e.message, true); }
      };
      row.append(del);
    }
    $('jobs').append(row);
  }

  working(data.busy);
  const selected = jobs.find(j => j.id === currentJob) || (!currentJob && jobs.find(j => ['running', 'uploading'].includes(j.status)));
  if (selected && (selected.status !== 'completed' || $('result').hidden)) await show(selected);

  if (data.busy && !polling) {
    polling = true;
    setTimeout(async () => {
      polling = false;
      try { await refresh(); } catch (e) { working(false); message(e.message, true); }
    }, 1000);
  }
}

async function submit(demo = false) {
  if (!$('form').reportValidity()) return;
  const file = $('file').files[0];
  if (!demo && !file) { message('Escolha uma imagem TIFF para continuar.', true); return; }
  if (file && !demo && file.size > 1024 ** 3) { message('O limite por imagem é 1 GiB.', true); return; }

  working(true);
  $('result').hidden = true;
  $('canvas').hidden = true;
  $('empty').hidden = false;
  message(demo ? 'Preparando exemplo…' : 'Enviando imagem…');
  $('state').textContent = 'Enviando';

  try {
    const response = await api(`${demo ? 'api/demo' : 'api/classify'}?${params()}`, {
      method: 'POST',
      headers: { 'Content-Type': 'image/tiff' },
      body: demo ? null : file
    });
    await show(await response.json());
    await refresh();
  } catch (e) { working(false); message(e.message, true); }
}

async function handleShapeUpload(file) {
  if (!file) return;
  $('shape-filename').textContent = file.name;
  message('Processando arquivo vetorial…');

  try {
    const isJson = file.name.endsWith('.json') || file.name.endsWith('.geojson');
    const headers = isJson ? { 'Content-Type': 'application/json' } : {};
    const body = file;

    const response = await api('api/shapes/parse', { method: 'POST', headers, body });
    const data = await response.json();
    currentGeoJSON = data.geojson;

    $('shape-name').textContent = file.name;
    $('shape-poly-count').textContent = currentGeoJSON.features.length;
    $('shape-info').hidden = false;
    if ($('clip-btn')) $('clip-btn').disabled = !currentJob;

    message('Vetor carregado com sucesso. Selecione a aba "Sobreposição Vetor" para visualizar.');
    if (currentJob) await updateView('overlay');
  } catch (e) {
    message('Falha ao processar arquivo vetorial: ' + e.message, true);
  }
}

async function performClip() {
  if (!currentJob || !currentGeoJSON) {
    message('Carregue uma imagem e um vetor para recortar.', true);
    return;
  }

  message('Executando recorte do GeoTIFF por polígono…');
  try {
    const response = await api(`api/jobs/${currentJob}/clip`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(currentGeoJSON)
    });
    const data = await response.json();
    $('download-clipped').href = `api/jobs/${currentJob}/clipped.tif`;
    $('download-clipped').hidden = false;
    $('view-clipped').hidden = false;
    await updateView('clipped');
    message(`GeoTIFF recortado com sucesso! Dimensões: ${data.result.output_width} × ${data.result.output_height}`);
  } catch (e) {
    message('Falha ao recortar GeoTIFF: ' + e.message, true);
  }
}

// TAB NAVIGATION
document.querySelectorAll('.tab-btn').forEach(btn => {
  btn.onclick = () => {
    document.querySelectorAll('.tab-btn').forEach(b => { b.classList.remove('active'); b.setAttribute('aria-selected', 'false'); });
    document.querySelectorAll('.tab-content').forEach(c => c.classList.remove('active'));
    btn.classList.add('active');
    btn.setAttribute('aria-selected', 'true');
    $(btn.dataset.tab).classList.add('active');
  };
});

// VIEWER CONTROLS
$('view-map').onclick = () => updateView('map');
$('view-original').onclick = () => updateView('original');
$('view-overlay').onclick = () => updateView('overlay');
$('view-clipped').onclick = () => updateView('clipped');

// FORM & BUTTON BINDINGS
$('form').onsubmit = e => { e.preventDefault(); submit(); };
$('demo').onclick = () => submit(true);
$('file').onchange = () => { $('filename').textContent = $('file').files[0]?.name || 'Escolha ou arraste um TIFF'; };
$('drop').ondragover = e => { e.preventDefault(); $('drop').classList.add('drag'); };
$('drop').ondragleave = () => $('drop').classList.remove('drag');
$('drop').ondrop = e => {
  e.preventDefault();
  $('drop').classList.remove('drag');
  if (e.dataTransfer.files.length) {
    $('file').files = e.dataTransfer.files;
    $('file').onchange();
  }
};

$('shape-file').onchange = () => handleShapeUpload($('shape-file').files[0]);
$('shape-drop').ondragover = e => { e.preventDefault(); $('shape-drop').classList.add('drag'); };
$('shape-drop').ondragleave = () => $('shape-drop').classList.remove('drag');
$('shape-drop').ondrop = e => {
  e.preventDefault();
  $('shape-drop').classList.remove('drag');
  if (e.dataTransfer.files.length) {
    $('shape-file').files = e.dataTransfer.files;
    handleShapeUpload(e.dataTransfer.files[0]);
  }
};

if ($('clip-btn')) $('clip-btn').onclick = () => performClip();

$('mode').onchange = () => {
  const patches = $('mode').value === 'patches';
  $('classes-field').hidden = patches;
  $('homogeneity-field').hidden = !patches;
  $('method-help').textContent = patches ?
    'Une janelas vizinhas semelhantes. Valores maiores de homogeneidade permitem unir regiões mais diferentes.' :
    'Agrupa janelas com características semelhantes. Os tons representam classes visuais.';
};

refresh().catch(e => message(e.message, true));
