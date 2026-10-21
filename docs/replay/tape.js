/* A display of checked state patches. No matching, venue allocation or actors. */
(() => {
  'use strict';
  const $=id=>document.getElementById(id), canvas=$('scene'), c=canvas.getContext('2d');
  const blue='#69bacb', red='#e4927e', gold='#e9cc83', white='#e4ece6', muted='#849c95', line='#293a39';
  const mono='ui-monospace, SFMono-Regular, Menlo, Consolas, monospace';
  const reduced=matchMedia('(prefers-reduced-motion: reduce)').matches;
  let data=null, orders=new Map(), index=0, cursor=0, playing=false, capturing=false, last=0, speed=5, duration=30000, dirty=true;
  let tape=[], marks=[], positions=new Map(), levels={B:[],S:[]}, density=[], counts={};
  const integer=n=>Number.isSafeInteger(n)&&n>=0;
  const fmt=n=>n.toLocaleString('en-US'), price=p=>(p/10000).toFixed(p%100?4:2);
  const text=(value,x,y,size=11,color=muted,align='left',weight='400')=>{c.fillStyle=color;c.font=`${weight} ${size}px ${mono}`;c.textAlign=align;c.fillText(String(value),x,y);};
  function time(ns){const ms=Math.floor(ns/1e6);return `${String(Math.floor(ms/3600000)).padStart(2,'0')}:${String(Math.floor(ms/60000)%60).padStart(2,'0')}:${String(Math.floor(ms/1000)%60).padStart(2,'0')}.${String(ms%1000).padStart(3,'0')}`;}
  function validOrder(o){return Array.isArray(o)&&o.length===4&&typeof o[0]==='string'&&/^\d{1,20}$/.test(o[0])&&'BS'.includes(o[1])&&o[1].length===1&&integer(o[2])&&integer(o[3])&&o[3]>0;}
  function validate(d){
    if(d?.schema!==1||d.verified!==true||!Array.isArray(d.initial)||!Array.isArray(d.events)||!d.events.length||d.events.length>200000||d.initial.length>4000000||typeof d.stock!=='string'||d.stock.length>8||typeof d.date_label!=='string'||d.date_label.length>64||!integer(d.start_ns)||!integer(d.end_ns)||d.start_ns>=d.end_ns||d.end_ns-d.start_ns>60e9||!/^[a-f0-9]{64}$/.test(d.input_sha256)||(d.playback_speed!==undefined&&![1,3,5,10].includes(d.playback_speed)))throw Error('Not a supported checked tape export.');
    let t=0,n=0;const live=new Set();
    for(const o of d.initial){if(!validOrder(o)||live.has(o[0]))throw Error('Invalid initial order.');live.add(o[0]);}
    for(const e of d.events){
      if(!integer(e.t)||e.t<t||e.t>=d.end_ns-d.start_ns||!integer(e.n)||e.n<=n||typeof e.k!=='string'||e.k.length!==1||!Array.isArray(e.updates)||!Array.isArray(e.removes)||!integer(e.qty)||!integer(e.price)||!integer(e.execution_price)||typeof e.match!=='string')throw Error('Invalid event ordering or patch.');
      for(const id of e.removes){if(!live.delete(id))throw Error('Patch removes an absent order.');}
      for(const o of e.updates){if(!validOrder(o))throw Error('Invalid order patch.');live.add(o[0]);}
      t=e.t;n=e.n;
    }
  }
  function load(d){
    validate(d);data=d;duration=(d.end_ns-d.start_ns)/1e6;speed=d.playback_speed??5;$('speed').value=String(speed);
    $('symbol').textContent=d.stock+' / NASDAQ ITCH';$('date').textContent=d.date_label+' · '+time(d.start_ns).slice(0,8)+' excerpt';
    $('banner-source').textContent=d.stock+' · NASDAQ · '+d.date_label;
    $('proof').textContent=fmt(d.events.length)+' EXCERPT EVENTS · INDEPENDENT RAW CHECK';
    $('identity').textContent=`Input SHA-256 ${d.input_sha256} · ${fmt(d.input_bytes)} bytes · implementation ${d.source_id} · producer ${d.executable_sha256} · ${fmt(d.events.length)} excerpt events · ${d.clock_regressions} timestamp regressions.`;
    $('loader').hidden=true;$('error').hidden=true;
    for(const id of ['play','restart','timeline'])$(id).disabled=false;
    $('timeline').max=duration;updateControls();
    density=new Array(Math.ceil(duration/50)).fill(0);
    for(const e of d.events)density[Math.min(density.length-1,Math.floor(e.t/5e7))]++;
    seek(0);playing=!reduced;updateControls();last=performance.now();
  }
  function fail(error){playing=false;$('error').hidden=false;$('error').textContent=error.message;$('proof').textContent='EXPORT NOT LOADED';updateControls();}
  function apply(e,animate){
    const t=e.t/1e6;
    for(const id of e.removes){const p=positions.get(id);if(animate&&p)marks.push({x:p.x,y:p.y,side:p.side,k:e.k,t:cursor});orders.delete(id);}
    for(const o of e.updates){const prior=orders.get(o[0]);orders.set(o[0],{ref:o[0],side:o[1],price:o[2],qty:o[3],born:prior?.born??t,touch:t,k:e.k});}
    counts[e.k]=(counts[e.k]||0)+1;
    if(e.k==='P'||e.k==='Q'||e.k==='E'||(e.k==='C'&&e.printable==='Y')){
      if(e.k!=='Q')counts.prints=(counts.prints||0)+1;
      tape.unshift({t,k:e.k,qty:e.qty,price:e.k==='C'?e.execution_price:e.price,match:e.match,broken:false});
      if(tape.length>80)tape.length=80;
    }
    if(e.k==='B'){for(const p of tape)if(p.match===e.match)p.broken=true;tape.unshift({t,k:'B',match:e.match,qty:0,price:0});if(tape.length>80)tape.length=80;}
    index++;dirty=true;
  }
  function seek(ms){
    if(!data)return;
    cursor=Math.max(0,Math.min(duration,ms));orders=new Map();index=0;tape=[];marks=[];positions.clear();counts={};
    for(const o of data.initial)orders.set(o[0],{ref:o[0],side:o[1],price:o[2],qty:o[3],born:-1000,touch:-1000,k:'A'});
    while(index<data.events.length&&data.events[index].t/1e6<=cursor)apply(data.events[index],false);
    dirty=true;render();
  }
  function group(){
    const maps={B:new Map(),S:new Map()};
    for(const o of orders.values()){let a=maps[o.side].get(o.price);if(!a){a={p:o.price,qty:0,orders:[]};maps[o.side].set(o.price,a);}a.qty+=o.qty;a.orders.push(o);}
    levels={B:[...maps.B.values()].sort((a,b)=>b.p-a.p),S:[...maps.S.values()].sort((a,b)=>a.p-b.p)};
    dirty=false;
  }
  function segment(x1,y1,x2,y2,color,width=1){c.strokeStyle=color;c.lineWidth=width;c.beginPath();c.moveTo(x1,y1);c.lineTo(x2,y2);c.stroke();}
  function metric(label,value,x,color=white){text(label,x,36,10,muted);text(value,x,77,34,color,'left','500');}
  function render(){
    c.clearRect(0,0,1440,1010);c.fillStyle='#111b1d';c.fillRect(0,0,1440,1010);
    if(!data){text('A verified slice of the actual tape.',44,90,25,white);text('Load tape.json from a local export to begin.',44,125,13);return;}
    if(dirty)group();
    const totalLevels=levels.B.length+levels.S.length,bid=levels.B[0]?.p,ask=levels.S[0]?.p;
    metric('LIVE DISPLAYED ORDERS',fmt(orders.size),36);
    metric('OCCUPIED PRICES',fmt(totalLevels),340);
    metric('EXCERPT EVENTS APPLIED',fmt(index),595);
    metric('TRADE REPORTS',fmt(counts.prints||0),925,gold);
    text('E / printable C / P · auctions separate · as reported',925,98,8,muted);
    segment(36,115,1404,115,line);
    const from=Math.max(0,cursor-250),window=[];
    for(let i=index-1;i>=0&&data.events[i].t/1e6>=from;i--)window.push(data.events[i]);
    const adds=window.filter(e=>'AFU'.includes(e.k)).length,withdraws=window.filter(e=>'XDU'.includes(e.k)).length,executions=window.filter(e=>'EC'.includes(e.k)).reduce((a,e)=>a+e.qty,0);
    text(`+ ${fmt(adds)} ARRIVALS`,36,148,12,blue);text(`↔ ${fmt(withdraws)} WITHDRAWALS`,280,148,12,muted);text(`${fmt(executions)} SH EXECUTED`,580,148,12,gold);text('LAST 250 ms OF TAPE',900,148,9,muted);
    text('BUY ORDERS',36,192,12,blue,'left','600');text('FIFO →',382,192,9,muted);text('BID',504,192,9,blue,'right');
    text('ASK',600,192,9,red);text('← FIFO',685,192,9,muted);text('SELL ORDERS',1090,192,12,red,'right','600');
    text('REPORTED EXECUTIONS',1142,192,11,gold);segment(1116,174,1116,594,line);
    positions=new Map();let drawn=0,shownOrders=0;
    const visiblePrices=12,rowStep=22,maxQty=Math.max(1,...levels.B.slice(0,visiblePrices).map(l=>l.qty),...levels.S.slice(0,visiblePrices).map(l=>l.qty));
    for(const side of 'BS'){
      const isBuy=side==='B',color=isBuy?blue:red;
      levels[side].slice(0,visiblePrices).forEach((level,r)=>{
        const y=221+r*rowStep,head=isBuy?449:655;
        segment(isBuy?36:585,y+16,isBuy?519:1090,y+16,'#20302f');
        c.fillStyle=isBuy?'#193033':'#302926';const w=level.qty/maxQty*405;
        c.fillRect(isBuy?449-w:655,y-8,w,16);
        text(price(level.p),isBuy?510:593,y+3,r===0?14:12,r===0?white:muted,isBuy?'right':'left',r===0?'600':'400');
        const nodes=level.orders.slice(0,40);shownOrders+=level.orders.length;
        nodes.forEach((o,j)=>{
          const target=isBuy?head-j*10:head+j*10;
          const age=cursor-o.born;
          const shift=!reduced&&(playing||capturing)&&age>=0&&age<100?Math.pow(1-age/100,2)*(isBuy?-70:70):0;
          const x=target+shift;
          c.fillStyle=cursor-o.touch<80&&'EC'.includes(o.k)?gold:color;
          c.globalAlpha=.83;c.fillRect(x,y-7,8,10);c.globalAlpha=1;
          positions.set(o.ref,{x:target,y:y-2,side});drawn++;
        });
        text(`${fmt(level.qty)} sh · ${fmt(level.orders.length)} orders${level.orders.length>40?' · +'+fmt(level.orders.length-40)+' not drawn':''}`,isBuy?449:655,y+12,8,muted,isBuy?'right':'left');
      });
    }
    c.fillStyle='#172624';c.fillRect(525,208,54,276);
    c.save();c.translate(552,346);c.rotate(-Math.PI/2);text(bid&&ask?`${((ask-bid)/100).toFixed(1)}¢ SPREAD`:'ONE SIDE',0,0,11,muted,'center');c.restore();
    text('BEST',552,225,8,white,'center');
    // Only trails are sampled. All patches above are applied, including those
    // between display frames and outside the visible price band.
    if(!reduced){
      marks=marks.filter(m=>cursor-m.t<150).slice(-48);
      for(const m of marks){const age=(cursor-m.t)/150,isExec='EC'.includes(m.k),dx=isExec?(m.side==='B'?85:-85):(m.side==='B'?-100:100);c.globalAlpha=1-age;c.fillStyle=isExec?gold:muted;c.fillRect(m.x+dx*age,m.y-3,4,6);c.globalAlpha=1;}
      window.filter(e=>'EC'.includes(e.k)).slice(0,8).forEach((e,j)=>{
        const rows=levels[e.side]||[],r=rows.findIndex(l=>l.p===e.price);if(r<0||r>=visiblePrices)return;
        const age=(cursor-e.t/1e6)/250;c.strokeStyle=gold;c.globalAlpha=(1-age)*.28;c.lineWidth=1;
        c.beginPath();c.moveTo(e.side==='B'?449:655,219+r*rowStep);c.bezierCurveTo(550,165-j*3,1070,160,1135,211+j*21);c.stroke();c.globalAlpha=1;
      });
    }
    tape.slice(0,14).forEach((p,i)=>{
      const y=224+i*25;
      if(p.k==='B'){text('BREAK #'+p.match,1142,y,10,red);return;}
      text(p.k==='Q'?'AUCTION':p.k==='P'?'REPORT':'BOOK',1142,y,8,p.k==='Q'?muted:gold);
      text(price(p.price),1280,y,13,p.broken?muted:white,'right');text(fmt(p.qty),1370,y,12,p.broken?muted:gold,'right');
      text('sh',1383,y,8,muted);
      if(p.broken)segment(1142,y-4,1385,y-4,red);
      text(p.k+' · '+time(data.start_ns+p.t*1e6).slice(3),1142,y+10,7,muted);
    });
    if(!tape.length)text('Awaiting first report',1142,225,10,muted);
    text(`${fmt(shownOrders)} orders at the nearest ${visiblePrices} prices / side · first 40 drawn / price`,36,495,9,muted);
    text('THE WHOLE BOOK',36,603,11,white,'left','600');text('All prices · 1 square = 1 displayed order · logical census, not memory',1404,603,9,muted,'right');
    const cell=4.25,cols=Math.floor(1368/cell),limit=cols*Math.floor(238/cell);let cellIndex=0;
    for(const side of 'BS')for(const level of levels[side])for(const o of level.orders){
      if(cellIndex>=limit)break;
      c.fillStyle=cursor-o.touch<150?('EC'.includes(o.k)?gold:white):(side==='B'?blue:red);
      c.fillRect(36+(cellIndex%cols)*cell,621+Math.floor(cellIndex/cols)*cell,cell-.85,cell-.85);cellIndex++;
    }
    text(orders.size>limit?'+'+fmt(orders.size-limit)+' orders beyond the overview':fmt(orders.size)+' orders shown · '+fmt(orders.size-shownOrders)+' outside the detailed price band',36,877,9,muted);
    segment(36,895,1404,895,line);
    text('EVENT DENSITY',36,912,9,muted);text('50 ms bins · file events, not network traffic',36,929,8,muted);
    const max=Math.max(1,...density),bar=1096/density.length;
    density.forEach((n,i)=>{c.fillStyle=i*50<=cursor?'#8ba89b':'#30463f';const h=Math.sqrt(n/max)*38;c.fillRect(290+i*bar,972-h,Math.max(1,bar-.5),h);});
    segment(290+cursor/duration*1096,898,290+cursor/duration*1096,975,white);
    text(time(data.start_ns).slice(0,8),290,990,8,muted);text(time(data.end_ns).slice(0,8),1386,990,8,muted,'right');
    text('Reconstructed display · capture continuity unverified · not a benchmark',835,1000,8,muted,'center');
    $('clock').textContent=time(data.start_ns+Math.round(cursor*1e6));$('timeline').value=cursor;
    $('position').textContent=`${(cursor/1000).toFixed(1)} / ${(duration/1000).toFixed(1)} s`;
    document.body.dataset.event=String(index);document.body.dataset.orders=String(orders.size);document.body.dataset.drawn=String(drawn);
  }
  function updateControls(){$('play').textContent=playing?'Pause':'Play';$('speed-label').textContent=speed+'× playback';}
  function tick(stamp){
    if(data&&playing&&!document.hidden){
      if(cursor>=duration)seek(0);
      else{
        cursor=Math.min(duration,cursor+Math.max(0,stamp-last)*speed);
        while(index<data.events.length&&data.events[index].t/1e6<=cursor)apply(data.events[index],true);
        render();
      }
    }
    last=stamp;requestAnimationFrame(tick);
  }
  $('play').addEventListener('click',()=>{playing=!playing;last=performance.now();updateControls();});
  $('restart').addEventListener('click',()=>seek(0));
  $('speed').addEventListener('change',()=>{speed=Number($('speed').value);updateControls();});
  $('timeline').addEventListener('input',()=>{playing=false;seek(Number($('timeline').value));updateControls();});
  $('file').addEventListener('change',async()=>{try{const f=$('file').files[0];if(!f||f.size>200*1024*1024)throw Error('Export is absent or larger than 200 MiB.');load(JSON.parse(await f.text()));}catch(error){fail(error);}});
  document.addEventListener('visibilitychange',()=>{last=performance.now();});
  // Deterministic capture and browser checks use the same renderer as playback.
  window.PMR_TAPE={load,seek(ms){playing=false;seek(ms);updateControls();},frame(ms){playing=false;capturing=true;if(ms<cursor)seek(ms);else{cursor=Math.min(duration,ms);while(index<data.events.length&&data.events[index].t/1e6<=cursor)apply(data.events[index],true);render();}capturing=false;updateControls();},state:()=>({index,cursor,orders:[...orders.values()].map(o=>[o.ref,o.side,o.price,o.qty]),counts:{...counts},tape:tape.map(p=>({...p})),playing})};
  if(location.hash==='#banner')document.body.classList.add('banner');
  render();requestAnimationFrame(tick);
  const embedded=$('tape-data');if(embedded){try{load(JSON.parse(embedded.textContent));}catch(error){fail(error);}}
})();
