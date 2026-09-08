'use strict';
const $ = id => document.getElementById(id);
let current = null, polling = false;
const states = {uploading:'Enviando',running:'Processando',completed:'Concluída',failed:'Falhou'};
function message(text, failure=false) { $('message').textContent=text; $('message').className=failure?'error':''; }
async function api(path, options={}) {
  const response = await fetch(path, options);
  if (!response.ok) { let text='Não foi possível concluir a solicitação.'; try { text=(await response.json()).message||text; } catch {} throw Error(text); }
  return response;
}
function params() {
  const p=new URLSearchParams();
  for (const id of ['mode','window','classes','homogeneity','min_valid','nodata']) p.set(id,$(id).value);
  p.set('ignore_zero',$('ignore_zero').checked);
  return p;
}
function working(value) { $('submit').disabled=value; $('demo').disabled=value; $('progress').hidden=!value; }
async function preview(id) {
  const bytes=new Uint8Array(await (await api(`api/jobs/${id}/preview.pgm`)).arrayBuffer());
  let end=0,lines=0;
  while(end<bytes.length && lines<3) if(bytes[end++]===10) lines++;
  const header=new TextDecoder().decode(bytes.slice(0,end)).trim().split(/\s+/);
  const width=Number(header[1]),height=Number(header[2]);
  if(header[0]!=='P5'||width>1024||height>1024||bytes.length-end!==width*height) throw Error('Prévia inválida.');
  const canvas=$('canvas'); canvas.width=width; canvas.height=height;
  const ctx=canvas.getContext('2d'),image=ctx.createImageData(width,height);
  for(let i=0;i<width*height;i++){ const c=bytes[end+i];image.data.set([c,c,c,255],i*4); }
  ctx.putImageData(image,0,0); canvas.hidden=false;$('empty').hidden=true;
}
async function show(job) {
  current=job.id;$('state').textContent=states[job.status]||job.status;
  if(job.status==='failed'){working(false);message(job.message||'Não foi possível classificar esta imagem.',true);return;}
  if(job.status!=='completed'){working(true);message('Classificação em andamento. Você pode manter esta página aberta para acompanhar.');return;}
  working(false);message('Mapa pronto. Baixe a imagem classificada e o relatório da execução.');
  await preview(job.id);$('result').hidden=false;
  const result=job.result;$('metrics').replaceChildren();
  for(const [value,label] of [[`${result.width.toLocaleString()} × ${result.height.toLocaleString()}`,'Dimensões originais'],[result.windows.toLocaleString(),'Janelas classificadas'],[`${result.elapsed_seconds.toFixed(2)} s`,'Tempo de processamento']]) {
    const box=document.createElement('div');box.className='metric';const strong=document.createElement('strong'),small=document.createElement('small');strong.textContent=value;small.textContent=label;box.append(strong,small);$('metrics').append(box);
  }
  $('map').href=`api/jobs/${job.id}/map.tif`;$('report').href=`api/jobs/${job.id}/report.json`;
}
async function refresh() {
  const data=await (await api('api/jobs')).json();
  const jobs=data.jobs.sort((a,b)=>b.created_at.localeCompare(a.created_at));$('jobs').replaceChildren();
  if(!jobs.length){const p=document.createElement('p');p.className='help';p.textContent='Nenhuma execução ainda.';$('jobs').append(p);}
  for(const job of jobs){
    const row=document.createElement('div');row.className='job';const open=document.createElement('button');open.textContent=`${new Date(job.created_at).toLocaleString('pt-BR')} · ${states[job.status]||job.status}`;open.onclick=()=>{ $('result').hidden=true;$('canvas').hidden=true;$('empty').hidden=false;show(job).catch(e=>message(e.message,true));};row.append(open);
    if(['completed','failed'].includes(job.status)){const del=document.createElement('button');del.textContent='Excluir';del.setAttribute('aria-label',`Excluir execução ${job.id}`);del.onclick=async()=>{if(!confirm('Excluir esta execução, sua imagem e os resultados?'))return;try{await api(`api/jobs/${job.id}`,{method:'DELETE'});if(current===job.id){current=null;$('result').hidden=true;$('canvas').hidden=true;$('empty').hidden=false;$('state').textContent='Aguardando imagem';message('Execução excluída.');}await refresh();}catch(e){message(e.message,true);}};row.append(del);}
    $('jobs').append(row);
  }
  working(data.busy);
  const selected=jobs.find(j=>j.id===current) || (!current && jobs.find(j=>['running','uploading'].includes(j.status)));
  if(selected && (selected.status!=='completed'||$('result').hidden)) await show(selected);
  if(data.busy&&!polling){polling=true;setTimeout(async()=>{polling=false;try{await refresh();}catch(e){working(false);message(e.message,true);}},1000);}
}
async function submit(demo=false) {
  if(!$('form').reportValidity())return;
  const file=$('file').files[0];if(!demo&&!file){message('Escolha uma imagem TIFF para continuar.',true);return;}
  if(file&&!demo&&file.size>1024**3){message('O limite por imagem é 1 GiB.',true);return;}
  working(true);$('result').hidden=true;$('canvas').hidden=true;$('empty').hidden=false;
  message(demo?'Preparando exemplo…':'Enviando imagem…');$('state').textContent='Enviando';
  try {
    const response=await api(`${demo?'api/demo':'api/classify'}?${params()}`,{method:'POST',headers:{'Content-Type':'image/tiff'},body:demo?null:file});
    await show(await response.json());await refresh();
  }catch(e){working(false);message(e.message,true);}
}
$('form').onsubmit=e=>{e.preventDefault();submit();};$('demo').onclick=()=>submit(true);
$('file').onchange=()=>{$('filename').textContent=$('file').files[0]?.name||'Escolha ou arraste um TIFF';};
$('drop').ondragover=e=>{e.preventDefault();$('drop').classList.add('drag');};$('drop').ondragleave=()=>$('drop').classList.remove('drag');
$('drop').ondrop=e=>{e.preventDefault();$('drop').classList.remove('drag');if(e.dataTransfer.files.length){$('file').files=e.dataTransfer.files;$('file').onchange();}};
$('mode').onchange=()=>{const patches=$('mode').value==='patches';$('classes-field').hidden=patches;$('homogeneity-field').hidden=!patches;$('method-help').textContent=patches?'Une janelas vizinhas semelhantes. Valores maiores de homogeneidade permitem unir regiões mais diferentes.':'Agrupa janelas com características semelhantes. Os tons representam classes visuais.';};
refresh().catch(e=>message(e.message,true));
