"use strict";
(() => {
  const $ = id => document.getElementById(id), ns = "http://www.w3.org/2000/svg";
  const data = window.MSIM_BOOK, reduced = matchMedia("(prefers-reduced-motion: reduce)");
  if (!data || data.schema !== 1 || !data.cases.length) {
    $("load-error").hidden = false;
    $("load-error").textContent = "Replay data is missing. Keep book-data.js beside this page.";
    $("action").textContent = "Unable to load checked scenes";
    document.querySelectorAll("button,input,select").forEach(el => { el.disabled = true; });
    return;
  }
  const svg = (tag, attrs = {}, text) => {
    const element = document.createElementNS(ns,tag);
    for (const [key,value] of Object.entries(attrs)) element.setAttribute(key,value);
    if (text !== undefined) element.textContent = text;
    return element;
  };
  const money = value => (value / 100).toFixed(2);
  const number = value => value.toLocaleString("en-US");
  // Patches contain explicit checked state, not commands for a second engine.
  const cases = data.cases.map(scene => {
    const orders = new Map(), tape = [];
    const frames = [{seq:0,orders:0,levels:0,status:"initial",state:[],tape:[],executions:[],label:scene.description}];
    for (const frame of scene.frames) {
      frame.remove.forEach(id => orders.delete(id));
      frame.updates.forEach(order => orders.set(order[0],order));
      frame.executions.forEach(execution => tape.push([frame.seq,...execution]));
      frames.push({...frame,state:[...orders.values()],tape:tape.slice(-10)});
    }
    return {...scene,frames};
  });
  const linked = /^#([a-z]+)-(\d+)$/.exec(location.hash);
  let sceneIndex = linked ? cases.findIndex(scene => scene.key === linked[1]) : 1;
  if (sceneIndex < 0) sceneIndex = 1;
  let index = linked && Number(linked[2]) < cases[sceneIndex].frames.length ? Number(linked[2]) : cases[sceneIndex].initial;
  let playing = !reduced.matches && !linked, timer = null;
  let nodes = new Map(), positions = new Map();
  const scene = () => cases[sceneIndex];
  const frame = () => scene().frames[index];
  const inspect = order => {
    $("inspect").textContent = `Order #${order[0]} · ${order[1] === "B" ? "buy" : "sell"} ${order[3]} at $${money(order[2])} · position ${order[4] + 1} in its queue`;
  };
  for (const [i,entry] of cases.entries()) {
    const button = document.createElement("button");
    button.type = "button"; button.textContent = entry.name; button.dataset.scene = entry.key;
    button.addEventListener("click",() => changeScene(i,false));
    $("scenes").append(button);
  }

  function pause() {
    playing = false; clearTimeout(timer); timer = null;
    $("play").textContent = "Play";
    $("play").setAttribute("aria-label","Play matching demonstration");
    document.querySelector(".explanation").setAttribute("aria-live","polite");
  }
  function clearDrawing() {
    nodes.clear(); positions.clear(); $("orders").replaceChildren(); $("effects").replaceChildren();
  }
  function changeScene(value,continuePlaying) {
    pause(); sceneIndex = value; index = scene().initial; clearDrawing(); render(false);
    if (continuePlaying) play();
  }
  function go(value) {
    pause(); const previous = index;
    index = Math.max(0,Math.min(scene().frames.length - 1,Number(value)));
    render(index === previous + 1);
  }
  function play() {
    if (index === scene().frames.length - 1) { index = scene().initial; clearDrawing(); render(false); }
    playing = true; $("play").textContent = "Pause";
    $("play").setAttribute("aria-label","Pause matching demonstration");
    document.querySelector(".explanation").setAttribute("aria-live","off");
    schedule();
  }
  function schedule() {
    clearTimeout(timer);
    const duration = index === scene().frames.length - 1 ? 1100 : frame().executions.length > 4 ? 850 : 1000 / scene().pace;
    timer = setTimeout(() => {
      if (!playing) return;
      if (index === scene().frames.length - 1) { changeScene((sceneIndex + 1) % cases.length,true); return; }
      index += 1; render(true); schedule();
    },duration / Number($("pace").value));
  }

  function render(animate) {
    const current = frame(), active = scene();
    animate = animate && !reduced.matches;
    $("book").classList.toggle("snap",!animate);
    if (!animate) { $("orders").querySelectorAll(".exiting").forEach(node => node.remove()); $("effects").replaceChildren(); }
    document.body.dataset.scene = active.key; document.body.dataset.frame = index;
    $("timeline").max = active.frames.length - 1; $("timeline").value = index;
    $("timeline").setAttribute("aria-valuetext",`Command ${index} of ${active.frames.length - 1}: ${current.label}`);
    $("progress").textContent = `${number(index)} / ${number(active.frames.length - 1)} checked commands · animation pace only`;
    $("live-count").textContent = number(current.orders); $("price-count").textContent = number(current.levels);
    $("back").disabled = index === 0; $("next").disabled = index === active.frames.length - 1;
    document.querySelectorAll("#scenes button").forEach(button => button.setAttribute("aria-pressed",String(button.dataset.scene === active.key)));
    $("scene-scale").textContent = `Up to ${active.peak_orders} waiting orders across ${active.peak_levels} prices in this scene`;
    const rejected = current.kind === "add" && current.status !== "accepted";
    const names = {capacity_full:"NO ROOM",duplicate_id:"DUPLICATE ID",invalid_qty:"BAD QUANTITY",invalid_tick:"BAD PRICE"};
    $("status").textContent = rejected ? names[current.status] || "REJECTED" : current.kind === "cancel" ? current.status === "removed" ? "CANCELLED" : "MISSING ID" : current.executions.length ? "TRADE" : "WAITING";
    $("status").classList.toggle("rejected",rejected);
    $("action").textContent = current.label;
    $("reason").textContent = current.kind === "add" ? rejected ? `Order #${current.id} was refused. No waiting order or trade was changed.` : current.filled ? `${current.side === "B" ? "Buy" : "Sell"} ${number(current.quantity)}: ${number(current.filled)} traded against ${current.executions.length} resting orders${current.rested ? `; ${number(current.rested)} now waits in the book` : ""}.` : `Order #${current.id}: ${current.side === "B" ? "buy" : "sell"} ${number(current.quantity)} at $${money(current.price)}. It joins behind older orders at that price.` : current.kind === "cancel" ? `ID #${current.id} ${current.status === "removed" ? "was found and removed. Its neighbours keep their order" : "is not resting. The book is unchanged"}.` : active.description;

    const groups = new Map();
    for (const order of current.state) {
      const key = `${order[1]}:${order[2]}`;
      if (!groups.has(key)) groups.set(key,[]);
      groups.get(key).push(order);
    }
    for (const group of groups.values()) group.sort((a,b) => a[4] - b[4]);
    const prices = [...new Set(current.state.map(order => order[2]))].sort((a,b) => b-a);
    const height = Math.min(30,440 / Math.max(1,prices.length)), top = 75 + (440 - height * Math.max(0,prices.length - 1)) / 2;
    const nextPositions = new Map();
    $("rows").replaceChildren(); $("routes").replaceChildren();
    $("rows").append(svg("text",{x:550,y:60,"text-anchor":"middle",class:"queue-total"},"PRICE ($)"),svg("text",{x:47,y:60,"text-anchor":"end",class:"queue-total"},"shares"),svg("text",{x:1053,y:60,class:"queue-total"},"shares"));
    if (current.kind) $("routes").append(svg("text",{x:550,y:28,"text-anchor":"middle",class:"incoming"},rejected ? "REJECTED · book unchanged" : current.kind === "cancel" ? `CANCEL #${current.id}` : `${current.side === "B" ? "BUY" : "SELL"} ${current.quantity} at $${money(current.price)}`));
    if (!prices.length) $("rows").append(svg("text",{x:550,y:290,"text-anchor":"middle",class:"empty"},"No orders are waiting yet"));
    for (const [rank,price] of prices.entries()) {
      const y = top + rank * height;
      const row = svg("g",{class:"level-row"});
      row.append(svg("line",{x1:70,y1:y+12,x2:1030,y2:y+12,class:"level-line"}),svg("rect",{x:510,y:y-11,width:80,height:23,rx:3,class:"price-bg"}),svg("text",{x:550,y:y+4,"text-anchor":"middle",class:"price-label"},money(price)));
      for (const side of ["B","S"]) {
        const queue = groups.get(`${side}:${price}`) || [], width = Math.min(42,410 / Math.max(1,queue.length) - 3);
        let shares = 0;
        for (const [position,order] of queue.entries()) {
          const x = side === "B" ? 490 - (position+1)*(width+3) : 610 + position*(width+3);
          nextPositions.set(order[0],{x,y:y-9,width:Math.max(4,width),side}); shares += order[3];
        }
        if (shares) row.append(svg("text",{x:side === "B" ? 47 : 1053,y:y+4,"text-anchor":side === "B" ? "end" : "start",class:"queue-total"},number(shares)));
      }
      $("rows").append(row);
    }

    const now = new Map(current.state.map(order => [order[0],order]));
    const executed = new Set(current.executions.map(execution => execution[0]));
    for (const [id,node] of nodes) {
      if (now.has(id)) continue;
      nodes.delete(id);
      const previous = positions.get(id);
      node.dataset.live = "false";
      if (animate && previous) {
        node.classList.add("exiting"); if (executed.has(id)) node.classList.add("executed");
        node.style.transform = `translate(${executed.has(id) ? 550 : previous.side === "B" ? -50 : 1150}px,${previous.y}px)`;
        setTimeout(() => node.remove(),220);
      } else node.remove();
    }
    for (const order of current.state) {
      const [id,side] = order, position = nextPositions.get(id);
      let node = nodes.get(id);
      if (!node) {
        node = svg("g",{class:`node ${side}`,tabindex:0,role:"button","data-ref":id,"data-live":"true"});
        node.append(svg("rect",{height:18}),svg("text",{x:3,y:12}),svg("title"));
        node.addEventListener("click",() => { pause(); const selected = frame().state.find(o => o[0] === id); if (selected) inspect(selected); });
        node.addEventListener("focus",() => { pause(); const selected = frame().state.find(o => o[0] === id); if (selected) inspect(selected); });
        node.addEventListener("mouseenter",() => { const selected = frame().state.find(o => o[0] === id); if (selected) inspect(selected); });
        node.addEventListener("keydown",event => { if (event.key === "Enter" || event.key === " ") { event.preventDefault(); node.click(); } });
        $("orders").append(node); nodes.set(id,node);
        if (animate) { node.style.transform = `translate(${side === "B" ? -45 : 1140}px,${position.y}px)`; void node.getBoundingClientRect(); }
      }
      node.classList.toggle("executed",animate && executed.has(id));
      node.querySelector("rect").setAttribute("width",position.width);
      node.querySelector("text").textContent = position.width >= 20 ? String(order[3]) : "";
      const label = `Order ${id}, ${side === "B" ? "buy" : "sell"} ${order[3]} at ${money(order[2])}, queue position ${order[4]+1}`;
      node.querySelector("title").textContent = label; node.setAttribute("aria-label",label);
      node.style.transform = `translate(${position.x}px,${position.y}px)`;
    }
    if (animate) {
      for (const execution of current.executions.slice(0,18)) {
        const position = positions.get(execution[0]) || nextPositions.get(execution[0]);
        if (!position) continue;
        const origin = current.side === "B" ? 20 : 1080;
        $("routes").append(svg("path",{d:`M ${origin} 35 Q 550 10 ${position.x+position.width/2} ${position.y+9}`,class:"route"}));
        const pulse = svg("circle",{cx:position.x+position.width/2,cy:position.y+9,r:4,class:"pulse"});
        $("effects").append(pulse); setTimeout(() => pulse.remove(),720);
      }
      if (current.kind === "cancel") {
        const target = positions.get(current.id);
        if (target) $("routes").append(svg("path",{d:`M ${target.side === "B" ? 15 : 1085} 35 Q 550 5 ${target.x+target.width/2} ${target.y+9}`,class:"route cancel"}));
      }
    }
    positions = nextPositions;

    $("tape").replaceChildren();
    for (const trade of [...current.tape].reverse()) {
      const card = document.createElement("div"), caption = document.createElement("small");
      card.className = "trade"; if (animate && trade[0] === current.seq) card.classList.add("new");
      card.textContent = `${number(trade[4])} at $${money(trade[3])}`;
      caption.textContent = `Order #${trade[2]} ↔ waiting #${trade[1]}`;
      card.append(caption); $("tape").append(card);
    }
    if (!current.tape.length) { const empty = document.createElement("span"); empty.className = "no-trades"; empty.textContent = "No matches yet. Waiting orders and cancellations do not create trades."; $("tape").append(empty); }
    for (const [side,id] of [["B","bid-index"],["S","ask-index"]]) {
      const keys = [...new Set(current.state.filter(order => order[1] === side).map(order => order[2]))].sort((a,b) => side === "B" ? a-b : b-a);
      $(id).replaceChildren();
      keys.forEach((price,i) => { const cell = document.createElement("span"); cell.className = `price-cell${i === keys.length-1 ? " best" : ""}`; cell.textContent = money(price); $(id).append(cell); });
    }
    $("capacity-track").classList.toggle("full",current.orders === active.capacity);
    $("capacity-track").querySelector("i").style.width = `${100*current.orders/active.capacity}%`;
    $("capacity").textContent = `${current.orders} / ${active.capacity} order entries · at most ${active.level_capacity} prices per side`;
    $("lookup").textContent = current.kind === "cancel" ? `ID #${current.id} → ${current.status === "removed" ? "queue entry unlinked; capacity released" : "not found; no mutation"}.` : "Logical price order and reserved capacity, not a raw-memory dump.";
    $("identity").textContent = `Source ${data.source_id} · checked exporter ${data.executable_sha256} · scene input ${active.trace_sha256}`;
  }

  $("play").addEventListener("click",() => playing ? pause() : play());
  $("back").addEventListener("click",() => go(index-1)); $("next").addEventListener("click",() => go(index+1));
  $("restart").addEventListener("click",() => go(0)); $("timeline").addEventListener("input",event => go(event.target.value));
  $("pace").addEventListener("change",() => { if (playing) schedule(); });
  document.addEventListener("visibilitychange",() => { if (document.hidden) pause(); });
  document.addEventListener("keydown",event => {
    if (event.target.closest("button,input,select,summary,a,.node")) return;
    if (event.key === "ArrowRight") { event.preventDefault(); go(index+1); }
    if (event.key === "ArrowLeft") { event.preventDefault(); go(index-1); }
    if (event.code === "Space") { event.preventDefault(); $("play").click(); }
  });
  render(false);
  const drawing = document.querySelector(".drawing");
  const center = () => { if (innerWidth <= 540) drawing.scrollLeft = (800-drawing.clientWidth)/2; };
  center(); addEventListener("resize",center);
  if (playing) play(); else pause();
})();
