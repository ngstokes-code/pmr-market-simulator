"use strict";
(() => {
  const root = document.getElementById("replay");
  const data = window.MSIM_REPLAY;
  if (!data || data.schema !== 1 || data.frames.length !== 24) {
    document.getElementById("event-title").textContent = "Replay data could not be loaded";
    document.getElementById("event-note").textContent = "Keep feed.html, feed.css, feed.js and demo-data.js together.";
    document.querySelectorAll("button, input, select").forEach(el => { el.disabled = true; });
    return;
  }
  const $ = id => document.getElementById(id);
  const price = value => (value / 10000).toLocaleString("en-US", { minimumFractionDigits:2, maximumFractionDigits:4 });
  const clock = value => {
    const ms = Math.floor(value / 1e6);
    return `${String(Math.floor(ms / 3600000)).padStart(2,"0")}:${String(Math.floor(ms / 60000) % 60).padStart(2,"0")}:${String(Math.floor(ms / 1000) % 60).padStart(2,"0")}.${String(ms % 1000).padStart(3,"0")}`;
  };
  const number = value => value.toLocaleString("en-US");
  const motion = window.matchMedia("(prefers-reduced-motion: reduce)");
  const linkedEvent = /^#event-(\d+)$/.exec(window.location.hash);
  let index = linkedEvent && Number(linkedEvent[1]) < data.frames.length ? Number(linkedEvent[1]) : data.initial_frame;
  let selected = { side:"B", price:data.scenario_price };
  let timer = null;
  let playing = false;
  let previousFrame = null;

  // Nodes are constructed once so quantity bars can move between snapshots.
  for (const value of [100200,100100,100000,99900]) {
    const row = document.createElement("div");
    row.className = "level";
    row.setAttribute("role", "row");
    for (const side of ["B","price","S"]) {
      const cell = document.createElement("div");
      cell.setAttribute("role", "cell");
      if (side === "price") {
        cell.className = "price";
        cell.textContent = price(value);
      } else {
        const button = document.createElement("button");
        button.type = "button";
        button.className = `side ${side === "B" ? "bid" : "ask"}`;
        button.dataset.price = value;
        button.dataset.side = side;
        button.setAttribute("aria-label", `Inspect ${side === "B" ? "bid" : "ask"} queue at $${price(value)}`);
        button.innerHTML = "<i aria-hidden=\"true\"></i><span>—</span>";
        button.style.width = "100%";
        button.addEventListener("click", () => { selected = { side, price:value }; render(false); });
        cell.append(button);
      }
      row.append(cell);
    }
    $("levels").append(row);
  }

  function pause() {
    playing = false;
    window.clearTimeout(timer);
    timer = null;
    $("play").textContent = "Play";
    $("play").setAttribute("aria-label", "Play replay");
  }

  function go(value) {
    pause();
    const next = Math.max(0, Math.min(data.frames.length - 1, Number(value)));
    const forward = next === index + 1;
    index = next;
    render(forward);
  }

  function schedule() {
    if (!playing) return;
    timer = window.setTimeout(() => {
      if (!playing) return;
      index += 1;
      render(true);
      if (index === data.frames.length - 1) pause();
      else schedule();
    }, 1200 / Number($("speed").value));
  }

  function render(animate) {
    const frame = data.frames[index];
    root.dataset.frame = frame.ordinal;
    $("timeline").value = index;
    $("timeline").setAttribute("aria-valuetext", `Event ${index} of 23: ${frame.title}`);
    $("event-count").textContent = `Event ${String(index).padStart(2,"0")} / 23`;
    $("clock").textContent = clock(frame.timestamp);
    $("clock").title = `${frame.timestamp} nanoseconds since midnight`;
    $("event-kind").textContent = frame.kind;
    $("event-title").textContent = frame.title;
    $("event-note").textContent = frame.note;
    $("event-flow").textContent = frame.flow;
    $("market-state").textContent = frame.active ? "Market hours · trading" : frame.kind === "S" && frame.code === "Q" ? "Market hours begin" : index >= 19 ? "Market hours ended" : index >= 17 ? "Trading halted" : "Before market hours";
    $("previous").disabled = index === 0;
    $("next").disabled = index === data.frames.length - 1;
    document.querySelectorAll(".chapters [data-frame]").forEach(button => {
      const start = Number(button.dataset.frame);
      const end = start === 9 ? 12 : start === 13 ? 17 : 23;
      button.setAttribute("aria-current", String(index >= start && index <= end));
    });

    document.querySelectorAll(".level .side").forEach(button => {
      const value = Number(button.dataset.price), side = button.dataset.side;
      const shares = frame.orders.filter(o => o.side === side && o.price === value).reduce((total,o) => total + o.shares, 0);
      const prior = previousFrame ? previousFrame.orders.filter(o => o.side === side && o.price === value).reduce((total,o) => total + o.shares, 0) : shares;
      button.querySelector("span").textContent = shares ? number(shares) : "—";
      button.querySelector("i").style.width = `${Math.min(100,shares / 1.2)}%`;
      button.classList.toggle("selected", selected.side === side && selected.price === value);
      button.setAttribute("aria-pressed", String(selected.side === side && selected.price === value));
      button.classList.remove("changed");
      if (animate && shares !== prior && !motion.matches) {
        void button.offsetWidth;
        button.classList.add("changed");
      }
    });
    $("queue-title").textContent = `FIFO at $${price(selected.price)}`;
    $("queue-side").textContent = selected.side === "B" ? "Bid" : "Ask";
    const queue = frame.orders.filter(o => o.side === selected.side && o.price === selected.price);
    $("queue-orders").classList.toggle("ask", selected.side === "S");
    $("queue-orders").replaceChildren();
    for (const order of queue) {
      const block = document.createElement("div");
      block.className = "order";
      block.dataset.ref = order.ref;
      if (animate && previousFrame && !previousFrame.orders.some(o => o.ref === order.ref)) block.classList.add("arriving");
      const ref = document.createElement("strong"), shares = document.createElement("span");
      ref.textContent = `#${order.ref}`;
      shares.textContent = `${number(order.shares)} shares`;
      block.append(ref, shares);
      $("queue-orders").append(block);
    }
    if (!queue.length) {
      const empty = document.createElement("p");
      empty.className = "no-orders";
      empty.textContent = "No displayed orders at this price.";
      $("queue-orders").append(empty);
    }
    $("queue-caption").textContent = queue.length ? "Order references in reported arrival order. No participant identity inferred." : "An empty queue does not, by itself, establish an execution opportunity.";

    for (const mode of ["optimistic","pessimistic"]) {
      const arm = $(mode), state = frame.shadow && frame.shadow[mode];
      arm.querySelector(".ahead strong").textContent = state ? number(state.ahead) : "—";
      arm.querySelector(".ahead-track i").style.width = `${state ? state.ahead : 0}%`;
      arm.classList.toggle("opportunity", Boolean(state && state.opportunity));
      arm.querySelector(".outcome").textContent = !state ? "Entry not placed yet" : state.opportunity ? "One-share model opportunity" : state.ahead === 0 ? "At front · no excess service yet" : "No model opportunity";
    }
    $("scenario-clock").textContent = !frame.shadow ? "Scenario starts at market open." : frame.shadow.complete ? "One-second horizon complete. Scenario states are held at that boundary." : "Same reported path in both scenarios. Neither is a bound on actual venue fills.";

    $("prints").replaceChildren();
    for (const print of [...frame.tape].reverse()) {
      const row = document.createElement("tr");
      row.dataset.match = print.match;
      if (print.broken) row.classList.add("broken");
      if (animate && print.ordinal === frame.ordinal) row.classList.add("new-print");
      for (const value of [clock(print.timestamp),price(print.price),number(print.shares)]) {
        const cell = document.createElement("td"), text = document.createElement("span");
        text.className = "print-value";
        text.textContent = value;
        cell.append(text);
        row.append(cell);
      }
      const report = document.createElement("td"), tag = document.createElement("span"), detail = document.createElement("span"), match = document.createElement("small");
      tag.className = "report-tag";
      tag.textContent = print.kind;
      detail.className = "report-detail";
      detail.textContent = print.broken ? "Broken print" : print.detail;
      match.textContent = `Match #${print.match}`;
      report.append(tag, detail, match);
      row.append(report);
      $("prints").append(row);
    }
    $("empty-tape").hidden = frame.tape.length > 0;
    $("eligible-shares").textContent = number(frame.eligible_shares);
    $("cross-shares").textContent = number(frame.cross_shares);
    if (!playing) $("announcement").textContent = `Event ${index}: ${frame.title}. ${frame.note}`;
    previousFrame = frame;
  }

  $("play").addEventListener("click", () => {
    if (playing) { pause(); return; }
    if (index === data.frames.length - 1) { index = data.initial_frame; render(false); }
    playing = true;
    $("play").textContent = "Pause";
    $("play").setAttribute("aria-label", "Pause replay");
    schedule();
  });
  $("previous").addEventListener("click", () => go(index - 1));
  $("next").addEventListener("click", () => go(index + 1));
  $("reset").addEventListener("click", () => { selected = {side:"B",price:data.scenario_price}; go(data.initial_frame); });
  $("timeline").addEventListener("input", event => go(event.target.value));
  $("speed").addEventListener("change", () => { if (playing) { window.clearTimeout(timer); schedule(); } });
  document.querySelectorAll(".chapters button").forEach(button => button.addEventListener("click", () => go(button.dataset.frame)));
  document.addEventListener("keydown", event => {
    if (event.target.matches("input, select, button, summary, a")) return;
    if (event.key === "ArrowLeft") { event.preventDefault(); go(index - 1); }
    if (event.key === "ArrowRight") { event.preventDefault(); go(index + 1); }
    if (event.code === "Space") { event.preventDefault(); $("play").click(); }
  });
  document.addEventListener("visibilitychange", () => { if (document.hidden) pause(); });
  $("provenance").textContent = `Input SHA-256: ${data.input_sha256} · Export source: ${data.source_id}`;
  render(false);
})();
